#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include "common/identity.h"
#include "common/key-store.h"
#include "lastfm-keys.h"
#include "lastfm.h"
#include "song-info.h"

/* Requests follow Last.fm's guides at last.fm/api/desktopauth and last.fm/api/scrobbling */
#define API_ADDRESS "https://ws.audioscrobbler.com/2.0/"
#define AUTH_ADDRESS "https://www.last.fm/api/auth/?api_key=%s&token=%s"
#define SESSION_FILE "lastfm.ini"
#define QUEUE_FILE "scrobbles.ini"
/* The browser is asked every few seconds whether the listener allowed Crimson Music, for as long as a token lives */
#define APPROVAL_POLL_MS 3000
#define APPROVAL_WAIT_US (G_GINT64_CONSTANT(60) * 60 * G_USEC_PER_SEC)
/* Last.fm takes at most this many scrobbles in one request */
#define BATCH_SIZE 50
/* Scrobbles that could not be sent wait this long, twice as long after each failure up to the longest wait */
#define RETRY_MS 60000
#define RETRY_MAXIMUM_MS (30 * 60000)
/* Errors after which Last.fm asks for the same request again later, and the one for a revoked session */
#define ERROR_UNAVAILABLE 11
#define ERROR_TEMPORARY 16
#define ERROR_SESSION 9
#define ERROR_NOT_ALLOWED_YET 14
#define SIMILAR_LIMIT "8"
/* Each similar song or artist costs a YouTube Music search, so a page asks for a fair number, not all */
#define SIMILAR_LOOKUP_LIMIT "25"
#define LOVED_PAGE_SIZE "1000"
#define LOVED_PAGE_LIMIT 20
#define FIRST_REFRESH_SECONDS 3
#define LASTFM_MUSIC "https://www.last.fm/music/"
#define PICTURE_FILE "lastfm-picture"
#define REFRESH_SECONDS (15 * 60)

typedef struct {
	char *artist, *track, *album;
	gint64 timestamp;
	int duration;
} Scrobble;

static SoupSession *session;
static LastfmChanged changed;
static gpointer changed_data;
static char *user, *session_key;
static char *token;
/* From asking for a token until the listener allowed Crimson Music, gave up or cancelled */
static gboolean is_connecting;
static gint64 token_time;
static guint approval_source;
static GQueue *queue;
static guint sending_count, retry_source;
static int failures;
static guint64 scrobble_total;
/* The loved tracks by love key, and the set a refresh fills before it takes over */
static GHashTable *loved, *incoming_loved;
static char *profile;
static GdkTexture *picture;


static void notify(void) {
	if (changed)
		changed(changed_data);
}


static void free_scrobble(gpointer data) {
	Scrobble *scrobble = data;
	g_free(scrobble->artist);
	g_free(scrobble->track);
	g_free(scrobble->album);
	g_free(scrobble);
}


/* Requests ================================================================ */


/* Every parameter by name in order, then the shared secret, hashed with MD5, as Last.fm signs requests */
static char *sign(GHashTable *parameters) {
	g_autoptr(GList) names = g_list_sort(g_hash_table_get_keys(parameters), (GCompareFunc)g_strcmp0);
	g_autoptr(GString) text = g_string_new(NULL);
	for (GList *name = names; name; name = name->next) {
		if (g_str_equal(name->data, "format"))
			continue;
		g_string_append(text, name->data);
		g_string_append(text, g_hash_table_lookup(parameters, name->data));
	}
	g_string_append(text, LASTFM_SECRET);
	return g_compute_checksum_for_string(G_CHECKSUM_MD5, text->str, -1);
}


static GHashTable *create_parameters(const char *method) {
	GHashTable *parameters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(parameters, g_strdup("method"), g_strdup(method));
	g_hash_table_insert(parameters, g_strdup("api_key"), g_strdup(LASTFM_KEY));
	if (session_key)
		g_hash_table_insert(parameters, g_strdup("sk"), g_strdup(session_key));
	return parameters;
}


static void send_signed(GHashTable *parameters, GAsyncReadyCallback done, gpointer data) {
	g_hash_table_insert(parameters, g_strdup("api_sig"), sign(parameters));
	g_hash_table_insert(parameters, g_strdup("format"), g_strdup("json"));
	char *form = soup_form_encode_hash(parameters);
	g_autoptr(SoupMessage) message = soup_message_new_from_encoded_form(SOUP_METHOD_POST, API_ADDRESS, form);
	soup_session_send_and_read_async(session, message, G_PRIORITY_LOW, NULL, done, data);
}


/* The answers are small and fixed, so the few values wanted are picked out by name */
static char *read_answer(GObject *source, GAsyncResult *result, int *error) {
	g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, NULL);
	*error = bytes ? 0 : -1;
	if (!bytes)
		return NULL;
	char *text = g_strndup(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
	g_autoptr(GRegex) code = g_regex_new("\"error\"\\s*:\\s*(\\d+)", 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;
	if (g_regex_match(code, text, 0, &match)) {
		g_autofree char *number = g_match_info_fetch(match, 1);
		*error = atoi(number);
	}
	return text;
}


/* JSON may write a slash as a backslash and a slash, which Last.fm does in every address */
static char *unescape_slashes(char *text) {
	if (!text)
		return NULL;
	g_auto(GStrv) parts = g_strsplit(text, "\\/", -1);
	g_free(text);
	return g_strjoinv("/", parts);
}


static char *read_value(const char *text, const char *name) {
	g_autofree char *pattern = g_strdup_printf("\"%s\"\\s*:\\s*\"([^\"]*)\"", name);
	g_autoptr(GRegex) regex = g_regex_new(pattern, 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;
	return text && g_regex_match(regex, text, 0, &match) ? unescape_slashes(g_match_info_fetch(match, 1)) : NULL;
}


/* Session ================================================================= */


static void save_session(void) {
	g_autofree char *path = get_config_file_path(SESSION_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	if (session_key) {
		g_key_file_set_string(file, "session", "user", user ? user : "");
		g_key_file_set_string(file, "session", "key", session_key);
	}
	save_key_file(file, path);
	g_chmod(path, 0600);
}


static void load_session(void) {
	g_autofree char *path = get_config_file_path(SESSION_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	session_key = g_key_file_get_string(file, "session", "key", NULL);
	user = g_key_file_get_string(file, "session", "user", NULL);
	if (session_key && !*session_key)
		g_clear_pointer(&session_key, g_free);
}


static void stop_waiting(void) {
	is_connecting = FALSE;
	g_clear_handle_id(&approval_source, g_source_remove);
	g_clear_pointer(&token, g_free);
}


static void flush_queue(void);
static void fetch_profile(void);


static gboolean ask_for_session(gpointer data);


static void receive_session(GObject *source, GAsyncResult *result, gpointer data) {
	int error = 0;
	g_autofree char *text = read_answer(source, result, &error);
	if (!is_connecting)
		return;
	g_autofree char *key = error ? NULL : read_value(text, "key");
	if (key) {
		g_free(session_key);
		session_key = g_steal_pointer(&key);
		g_free(user);
		user = read_value(text, "name");
		stop_waiting();
		save_session();
		notify();
		flush_queue();
		fetch_profile();
		refresh_lastfm_loves();
		return;
	}
	gboolean is_waiting = error == ERROR_NOT_ALLOWED_YET || error == -1;
	if (!is_waiting || g_get_monotonic_time() - token_time > APPROVAL_WAIT_US) {
		stop_waiting();
		notify();
		return;
	}
	approval_source = g_timeout_add(APPROVAL_POLL_MS, ask_for_session, NULL);
}


static gboolean ask_for_session(gpointer data) {
	approval_source = 0;
	GHashTable *parameters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(parameters, g_strdup("method"), g_strdup("auth.getSession"));
	g_hash_table_insert(parameters, g_strdup("api_key"), g_strdup(LASTFM_KEY));
	g_hash_table_insert(parameters, g_strdup("token"), g_strdup(token));
	send_signed(parameters, receive_session, NULL);
	g_hash_table_unref(parameters);
	return G_SOURCE_REMOVE;
}


static void receive_token(GObject *source, GAsyncResult *result, gpointer data) {
	GtkWidget *parent = data;
	int error = 0;
	g_autofree char *text = read_answer(source, result, &error);
	g_autofree char *new_token = error ? NULL : read_value(text, "token");
	if (!is_connecting) {
		g_clear_object(&parent);
		return;
	}
	if (!new_token) {
		stop_waiting();
		notify();
		g_clear_object(&parent);
		return;
	}
	token = g_steal_pointer(&new_token);
	token_time = g_get_monotonic_time();
	g_autofree char *address = g_strdup_printf(AUTH_ADDRESS, LASTFM_KEY, token);
	g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(address);
	GtkRoot *root = parent ? gtk_widget_get_root(parent) : NULL;
	gtk_uri_launcher_launch(launcher, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL, NULL, NULL);
	g_clear_object(&parent);
	approval_source = g_timeout_add(APPROVAL_POLL_MS, ask_for_session, NULL);
	notify();
}


/* Profile ================================================================= */


/* The largest of the profile pictures, which Last.fm lists by size */
static char *read_picture_address(const char *text) {
	static const char *const SIZES[] = {"extralarge", "large", "medium"};
	for (guint index = 0; index < G_N_ELEMENTS(SIZES); index++) {
		g_autofree char *pattern = g_strdup_printf("\\{\"size\":\"%s\",\"#text\":\"([^\"]+)\"\\}|"
			"\\{\"#text\":\"([^\"]+)\",\"size\":\"%s\"\\}", SIZES[index], SIZES[index]);
		g_autoptr(GRegex) regex = g_regex_new(pattern, 0, 0, NULL);
		g_autoptr(GMatchInfo) match = NULL;
		if (!g_regex_match(regex, text, 0, &match))
			continue;
		g_autofree char *first = g_match_info_fetch(match, 1);
		return unescape_slashes(first && *first ? g_steal_pointer(&first) : g_match_info_fetch(match, 2));
	}
	return NULL;
}


/* The picture is kept on disk, so the next launch shows it at once rather than the initial first */
static char *get_picture_path(void) {
	return g_build_filename(g_get_user_cache_dir(), get_application_identity()->slug, PICTURE_FILE, NULL);
}


static void load_picture(void) {
	g_autofree char *path = get_picture_path();
	g_autoptr(GFile) file = g_file_new_for_path(path);
	if (session_key && g_file_query_exists(file, NULL))
		picture = gdk_texture_new_from_file(file, NULL);
}


static void receive_picture(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, NULL);
	g_autoptr(GdkTexture) texture = bytes ? gdk_texture_new_from_bytes(bytes, NULL) : NULL;
	if (!texture || !session_key)
		return;
	g_autofree char *path = get_picture_path();
	g_autofree char *folder = g_path_get_dirname(path);
	g_mkdir_with_parents(folder, 0700);
	g_file_set_contents(path, g_bytes_get_data(bytes, NULL), (gssize)g_bytes_get_size(bytes), NULL);
	g_set_object(&picture, texture);
	notify();
}


GdkTexture *get_lastfm_picture(void) {
	return session_key ? picture : NULL;
}


static void receive_profile(GObject *source, GAsyncResult *result, gpointer data) {
	int error = 0;
	g_autofree char *text = read_answer(source, result, &error);
	if (error)
		return;
	g_autofree char *count = read_value(text, "playcount");
	scrobble_total = count ? g_ascii_strtoull(count, NULL, 10) : 0;
	g_free(profile);
	profile = read_value(text, "url");
	g_autofree char *address = read_picture_address(text);
	if (address && *address) {
		g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, address);
		if (message)
			soup_session_send_and_read_async(session, message, G_PRIORITY_LOW, NULL, receive_picture, NULL);
	}
	notify();
}


static void fetch_profile(void) {
	if (!session_key || !user)
		return;
	GHashTable *parameters = create_parameters("user.getInfo");
	g_hash_table_insert(parameters, g_strdup("user"), g_strdup(user));
	send_signed(parameters, receive_profile, NULL);
	g_hash_table_unref(parameters);
}


guint64 get_lastfm_scrobbles(void) {
	return session_key ? scrobble_total : 0;
}


const char *get_lastfm_profile(void) {
	return session_key ? profile : NULL;
}


/* Public ================================================================== */


gboolean is_lastfm_available(void) {
	return *LASTFM_KEY && *LASTFM_SECRET;
}


const char *get_lastfm_key(void) {
	return LASTFM_KEY;
}


const char *get_lastfm_user(void) {
	return session_key ? (user ? user : "") : NULL;
}


gboolean is_lastfm_connecting(void) {
	return is_connecting;
}


void connect_lastfm(GtkWidget *parent) {
	if (!is_lastfm_available() || is_lastfm_connecting())
		return;
	is_connecting = TRUE;
	GHashTable *parameters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(parameters, g_strdup("method"), g_strdup("auth.getToken"));
	g_hash_table_insert(parameters, g_strdup("api_key"), g_strdup(LASTFM_KEY));
	send_signed(parameters, receive_token, parent ? g_object_ref(parent) : NULL);
	g_hash_table_unref(parameters);
	notify();
}


void cancel_lastfm(void) {
	stop_waiting();
	notify();
}


void disconnect_lastfm(void) {
	stop_waiting();
	scrobble_total = 0;
	g_clear_pointer(&profile, g_free);
	g_clear_pointer(&loved, g_hash_table_unref);
	g_clear_object(&picture);
	g_autofree char *picture_path = get_picture_path();
	g_remove(picture_path);
	g_clear_pointer(&session_key, g_free);
	g_clear_pointer(&user, g_free);
	save_session();
	notify();
}


/* Scrobbles =============================================================== */


static void save_queue(void) {
	g_autofree char *path = get_state_file_path(QUEUE_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	guint index = 0;
	for (GList *link = queue->head; link; link = link->next, index++) {
		Scrobble *scrobble = link->data;
		g_autofree char *group = g_strdup_printf("scrobble-%u", index);
		g_key_file_set_string(file, group, "artist", scrobble->artist);
		g_key_file_set_string(file, group, "track", scrobble->track);
		g_key_file_set_string(file, group, "album", scrobble->album);
		g_key_file_set_int64(file, group, "timestamp", scrobble->timestamp);
		g_key_file_set_integer(file, group, "duration", scrobble->duration);
	}
	save_key_file(file, path);
}


static void load_queue(void) {
	g_autofree char *path = get_state_file_path(QUEUE_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	g_auto(GStrv) groups = g_key_file_get_groups(file, NULL);
	for (char **group = groups; group && *group; group++) {
		Scrobble *scrobble = g_new0(Scrobble, 1);
		scrobble->artist = g_key_file_get_string(file, *group, "artist", NULL);
		scrobble->track = g_key_file_get_string(file, *group, "track", NULL);
		scrobble->album = g_key_file_get_string(file, *group, "album", NULL);
		scrobble->timestamp = g_key_file_get_int64(file, *group, "timestamp", NULL);
		scrobble->duration = g_key_file_get_integer(file, *group, "duration", NULL);
		if (scrobble->artist && scrobble->track)
			g_queue_push_tail(queue, scrobble);
		else
			free_scrobble(scrobble);
	}
}


static gboolean flush_later(gpointer data) {
	retry_source = 0;
	flush_queue();
	return G_SOURCE_REMOVE;
}


static void receive_scrobbles(GObject *source, GAsyncResult *result, gpointer data) {
	int error = 0;
	g_autofree char *text = read_answer(source, result, &error);
	guint sent = sending_count;
	sending_count = 0;
	if (error == -1 || error == ERROR_UNAVAILABLE || error == ERROR_TEMPORARY) {
		failures++;
		retry_source = g_timeout_add(MIN(RETRY_MS << MIN(failures - 1, 5), RETRY_MAXIMUM_MS), flush_later, NULL);
		return;
	}
	failures = 0;
	if (error == ERROR_SESSION) {
		disconnect_lastfm();
		return;
	}
	for (guint index = 0; index < sent; index++)
		free_scrobble(g_queue_pop_head(queue));
	scrobble_total += sent;
	save_queue();
	notify();
	flush_queue();
}


/* Waiting scrobbles go out oldest first, as many at a time as Last.fm takes */
static void flush_queue(void) {
	if (!session_key || sending_count || retry_source || g_queue_is_empty(queue))
		return;
	GHashTable *parameters = create_parameters("track.scrobble");
	guint index = 0;
	for (GList *link = queue->head; link && index < BATCH_SIZE; link = link->next, index++) {
		Scrobble *scrobble = link->data;
		g_hash_table_insert(parameters, g_strdup_printf("artist[%u]", index), g_strdup(scrobble->artist));
		g_hash_table_insert(parameters, g_strdup_printf("track[%u]", index), g_strdup(scrobble->track));
		g_hash_table_insert(parameters, g_strdup_printf("timestamp[%u]", index),
			g_strdup_printf("%" G_GINT64_FORMAT, scrobble->timestamp));
		if (scrobble->album && *scrobble->album)
			g_hash_table_insert(parameters, g_strdup_printf("album[%u]", index), g_strdup(scrobble->album));
		if (scrobble->duration > 0)
			g_hash_table_insert(parameters, g_strdup_printf("duration[%u]", index),
				g_strdup_printf("%d", scrobble->duration));
	}
	sending_count = index;
	send_signed(parameters, receive_scrobbles, NULL);
	g_hash_table_unref(parameters);
}


/* Last.fm knows songs by their main artist, which YouTube Music lists first of several */
static char *get_first_artist(CrimsonSong *song) {
	g_auto(GStrv) artists = g_strsplit(song->artist ? song->artist : "", ",", 2);
	return g_strdup(artists[0] ? g_strstrip(artists[0]) : "");
}


char *get_song_lastfm_artist(CrimsonSong *song) {
	return get_first_artist(song);
}


/* Last.fm writes spaces as plus signs and escapes everything else in a name, slashes and plus signs included */
static char *escape_name(const char *name) {
	g_autofree char *escaped = g_uri_escape_string(name, NULL, FALSE);
	g_auto(GStrv) parts = g_strsplit(escaped, "%20", -1);
	return g_strjoinv("+", parts);
}


char *make_lastfm_address(const char *artist, const char *album, const char *track) {
	g_autofree char *escaped_artist = escape_name(artist);
	if (track) {
		g_autofree char *escaped_track = escape_name(track);
		return g_strdup_printf(LASTFM_MUSIC "%s/_/%s", escaped_artist, escaped_track);
	}
	if (album) {
		g_autofree char *escaped_album = escape_name(album);
		return g_strdup_printf(LASTFM_MUSIC "%s/%s", escaped_artist, escaped_album);
	}
	return g_strdup_printf(LASTFM_MUSIC "%s", escaped_artist);
}


static void add_song_parameters(GHashTable *parameters, CrimsonSong *song) {
	g_hash_table_insert(parameters, g_strdup("artist"), get_first_artist(song));
	g_hash_table_insert(parameters, g_strdup("track"), g_strdup(song->title));
}


static void ignore_answer(GObject *source, GAsyncResult *result, gpointer data) {
	int error = 0;
	g_autofree char *text = read_answer(source, result, &error);
}


void send_now_playing(CrimsonSong *song, double duration) {
	if (!session_key || !is_lastfm_available())
		return;
	GHashTable *parameters = create_parameters("track.updateNowPlaying");
	add_song_parameters(parameters, song);
	if (*song->album)
		g_hash_table_insert(parameters, g_strdup("album"), g_strdup(song->album));
	if (duration > 0)
		g_hash_table_insert(parameters, g_strdup("duration"), g_strdup_printf("%d", (int)duration));
	send_signed(parameters, ignore_answer, NULL);
	g_hash_table_unref(parameters);
}


void scrobble_song(CrimsonSong *song, gint64 started, double duration) {
	if (!session_key || !is_lastfm_available())
		return;
	Scrobble *scrobble = g_new0(Scrobble, 1);
	scrobble->artist = get_first_artist(song);
	scrobble->track = g_strdup(song->title);
	scrobble->album = g_strdup(song->album);
	scrobble->timestamp = started;
	scrobble->duration = (int)duration;
	g_queue_push_tail(queue, scrobble);
	save_queue();
	flush_queue();
}


guint count_waiting_scrobbles(void) {
	return g_queue_get_length(queue);
}


/* Songs match a loved track by main artist and title, both in lowercase and without bracketed parts such as
 * "(Official Video)", since YouTube and Last.fm often word them a little differently */
static char *make_love_key(const char *artist, const char *title) {
	static GRegex *brackets;
	if (!brackets)
		brackets = g_regex_new("\\([^)]*\\)|\\[[^\\]]*\\]", G_REGEX_OPTIMIZE, 0, NULL);
	g_autofree char *plain = g_regex_replace_literal(brackets, title, -1, 0, "", 0, NULL);
	g_autofree char *lower_artist = g_utf8_strdown(artist, -1);
	g_autofree char *lower_title = g_utf8_strdown(g_strstrip(plain), -1);
	return g_strconcat(g_strstrip(lower_artist), "\n", lower_title, NULL);
}


static char *make_song_love_key(CrimsonSong *song) {
	g_autofree char *artist = get_first_artist(song);
	return make_love_key(artist, song->title);
}


gboolean is_song_loved(CrimsonSong *song) {
	g_autofree char *key = make_song_love_key(song);
	return loved && g_hash_table_contains(loved, key);
}


void toggle_song_love(CrimsonSong *song) {
	if (!session_key || !is_lastfm_available())
		return;
	gboolean is_loved = !is_song_loved(song);
	if (!loved)
		loved = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autofree char *key = make_song_love_key(song);
	if (is_loved)
		g_hash_table_add(loved, g_steal_pointer(&key));
	else
		g_hash_table_remove(loved, key);
	GHashTable *parameters = create_parameters(is_loved ? "track.love" : "track.unlove");
	add_song_parameters(parameters, song);
	send_signed(parameters, ignore_answer, NULL);
	g_hash_table_unref(parameters);
	notify();
}


static void fetch_loves(int page);


/* Every page of loved tracks is read into a new set, which replaces the old one once the last page is in */
static void receive_loves(GObject *source, GAsyncResult *result, gpointer data) {
	int page = GPOINTER_TO_INT(data);
	int error = 0;
	g_autofree char *text = read_answer(source, result, &error);
	int pages = 0;
	g_auto(GStrv) tracks = error ? NULL : read_lastfm_loved(text, &pages);
	if (!tracks) {
		g_clear_pointer(&incoming_loved, g_hash_table_unref);
		return;
	}
	for (char **track = tracks; *track && track[1]; track += 2)
		g_hash_table_add(incoming_loved, make_love_key(track[0], track[1]));
	if (page < pages && page < LOVED_PAGE_LIMIT) {
		fetch_loves(page + 1);
		return;
	}
	g_clear_pointer(&loved, g_hash_table_unref);
	loved = g_steal_pointer(&incoming_loved);
	notify();
}


static void fetch_loves(int page) {
	GHashTable *parameters = create_parameters("user.getLovedTracks");
	g_hash_table_insert(parameters, g_strdup("user"), g_strdup(user));
	g_hash_table_insert(parameters, g_strdup("limit"), g_strdup(LOVED_PAGE_SIZE));
	g_hash_table_insert(parameters, g_strdup("page"), g_strdup_printf("%d", page));
	send_signed(parameters, receive_loves, GINT_TO_POINTER(page));
	g_hash_table_unref(parameters);
}


void refresh_lastfm_loves(void) {
	if (!session_key || !user || incoming_loved || !is_lastfm_available())
		return;
	incoming_loved = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	fetch_loves(1);
}


void tag_on_lastfm(CrimsonSong *song, const char *tag, gboolean is_added) {
	if (!session_key || !is_lastfm_available())
		return;
	GHashTable *parameters = create_parameters(is_added ? "track.addTags" : "track.removeTag");
	add_song_parameters(parameters, song);
	g_hash_table_insert(parameters, g_strdup(is_added ? "tags" : "tag"), g_strdup(tag));
	send_signed(parameters, ignore_answer, NULL);
	g_hash_table_unref(parameters);
}


/* Details ================================================================= */


typedef struct {
	char *video_id;
	char *answers[LASTFM_DETAIL_COUNT];
	int waiting;
	LastfmDetailsDone done;
	gpointer data;
} DetailsRequest;


typedef struct {
	DetailsRequest *request;
	int index;
} DetailsPart;


static void receive_detail(GObject *source, GAsyncResult *result, gpointer data) {
	DetailsPart *part = data;
	DetailsRequest *request = part->request;
	int error = 0;
	char *text = read_answer(source, result, &error);
	if (error) {
		g_free(text);
		text = NULL;
	}
	request->answers[part->index] = text;
	g_free(part);
	if (--request->waiting > 0)
		return;
	request->done(request->video_id, request->answers, request->data);
	for (int index = 0; index < LASTFM_DETAIL_COUNT; index++)
		g_free(request->answers[index]);
	g_free(request->video_id);
	g_free(request);
}


/* Reading needs no signature, only the key, and the listener's name adds their own plays and love */
void fetch_lastfm_details(CrimsonSong *song, LastfmDetailsDone done, gpointer data) {
	if (!is_lastfm_available())
		return;
	static const char *const METHODS[LASTFM_DETAIL_COUNT] = {
		"track.getInfo", "artist.getInfo", "track.getSimilar", "artist.getSimilar",
	};
	DetailsRequest *request = g_new0(DetailsRequest, 1);
	request->video_id = g_strdup(song->id);
	request->done = done;
	request->data = data;
	request->waiting = LASTFM_DETAIL_COUNT;
	g_autofree char *artist = get_first_artist(song);
	for (int index = 0; index < LASTFM_DETAIL_COUNT; index++) {
		g_autoptr(GHashTable) parameters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_hash_table_insert(parameters, g_strdup("method"), g_strdup(METHODS[index]));
		g_hash_table_insert(parameters, g_strdup("api_key"), g_strdup(LASTFM_KEY));
		g_hash_table_insert(parameters, g_strdup("artist"), g_strdup(artist));
		g_hash_table_insert(parameters, g_strdup("autocorrect"), g_strdup("1"));
		g_hash_table_insert(parameters, g_strdup("format"), g_strdup("json"));
		if (g_str_has_prefix(METHODS[index], "track."))
			g_hash_table_insert(parameters, g_strdup("track"), g_strdup(song->title));
		if (g_str_has_suffix(METHODS[index], "Similar"))
			g_hash_table_insert(parameters, g_strdup("limit"), g_strdup(SIMILAR_LIMIT));
		if (user && session_key)
			g_hash_table_insert(parameters, g_strdup("username"), g_strdup(user));
		g_autofree char *query = soup_form_encode_hash(parameters);
		g_autofree char *address = g_strconcat(API_ADDRESS, "?", query, NULL);
		g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, address);
		DetailsPart *part = g_new0(DetailsPart, 1);
		part->request = request;
		part->index = index;
		soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, receive_detail, part);
	}
}


/* The profile, picture, scrobble count and loves are read again soon after startup, once everything else has
 * settled, and then now and again, so changes made on Last.fm meanwhile show up */
static gboolean refresh_lastfm(gpointer data) {
	fetch_profile();
	refresh_lastfm_loves();
	flush_queue();
	return G_SOURCE_CONTINUE;
}


static gboolean refresh_first(gpointer data) {
	refresh_lastfm(NULL);
	g_timeout_add_seconds(REFRESH_SECONDS, refresh_lastfm, NULL);
	return G_SOURCE_REMOVE;
}


typedef struct {
	LastfmSimilarDone done;
	gpointer data;
} SimilarRequest;


static void receive_similar(GObject *source, GAsyncResult *result, gpointer data) {
	SimilarRequest *request = data;
	int error = 0;
	g_autofree char *text = read_answer(source, result, &error);
	g_auto(GStrv) found = read_lastfm_similar(error ? NULL : text);
	request->done(found, request->data);
	g_free(request);
}


void fetch_lastfm_similar(const char *artist, const char *track, LastfmSimilarDone done, gpointer data) {
	if (!is_lastfm_available()) {
		g_auto(GStrv) none = g_new0(char *, 1);
		done(none, data);
		return;
	}
	g_autoptr(GHashTable) parameters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(parameters, g_strdup("method"), g_strdup(track ? "track.getSimilar" : "artist.getSimilar"));
	g_hash_table_insert(parameters, g_strdup("api_key"), g_strdup(LASTFM_KEY));
	g_hash_table_insert(parameters, g_strdup("artist"), g_strdup(artist));
	if (track)
		g_hash_table_insert(parameters, g_strdup("track"), g_strdup(track));
	g_hash_table_insert(parameters, g_strdup("autocorrect"), g_strdup("1"));
	g_hash_table_insert(parameters, g_strdup("limit"), g_strdup(SIMILAR_LOOKUP_LIMIT));
	g_hash_table_insert(parameters, g_strdup("format"), g_strdup("json"));
	g_autofree char *query = soup_form_encode_hash(parameters);
	g_autofree char *address = g_strconcat(API_ADDRESS, "?", query, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, address);
	SimilarRequest *request = g_new0(SimilarRequest, 1);
	request->done = done;
	request->data = data;
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, receive_similar, request);
}


typedef struct {
	LastfmAboutDone done;
	gpointer data;
} AboutRequest;


static void receive_about(GObject *source, GAsyncResult *result, gpointer data) {
	AboutRequest *request = data;
	int error = 0;
	g_autofree char *text = read_answer(source, result, &error);
	g_autoptr(GHashTable) about = error ? NULL : read_lastfm_about(text);
	request->done(about, request->data);
	g_free(request);
}


void fetch_lastfm_about(const char *artist, const char *album, LastfmAboutDone done, gpointer data) {
	if (!is_lastfm_available() || !artist || !*artist) {
		done(NULL, data);
		return;
	}
	g_autoptr(GHashTable) parameters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(parameters, g_strdup("method"), g_strdup(album ? "album.getInfo" : "artist.getInfo"));
	g_hash_table_insert(parameters, g_strdup("api_key"), g_strdup(LASTFM_KEY));
	g_hash_table_insert(parameters, g_strdup("artist"), g_strdup(artist));
	if (album)
		g_hash_table_insert(parameters, g_strdup("album"), g_strdup(album));
	g_hash_table_insert(parameters, g_strdup("autocorrect"), g_strdup("1"));
	g_hash_table_insert(parameters, g_strdup("format"), g_strdup("json"));
	g_autofree char *query = soup_form_encode_hash(parameters);
	g_autofree char *address = g_strconcat(API_ADDRESS, "?", query, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, address);
	AboutRequest *request = g_new0(AboutRequest, 1);
	request->done = done;
	request->data = data;
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, receive_about, request);
}


void start_lastfm(LastfmChanged on_changed, gpointer data) {
	changed = on_changed;
	changed_data = data;
	queue = g_queue_new();
	g_autofree char *agent = g_strdup_printf("CrimsonMusic/%s", get_application_identity()->version);
	session = soup_session_new_with_options("user-agent", agent, NULL);
	load_session();
	load_picture();
	load_queue();
	flush_queue();
	fetch_profile();
	g_timeout_add_seconds(FIRST_REFRESH_SECONDS, refresh_first, NULL);
}

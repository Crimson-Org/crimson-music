#include <jsc/jsc.h>
#include <libsoup/soup.h>
#include <sqlite3.h>

#include "common/identity.h"
#include "lastfm.h"
#include "local-library.h"
#include "song-info.h"

#define DATABASE_FILE "song-info.db"
#define SCRIPT_NAME "song-info.js"
/* MusicBrainz asks for a name and a way to reach the makers in every request */
#define CONTACT "( https://github.com/Crimson-Org/crimson-music )"
#define YOUTUBE_AGENT "Mozilla/5.0"
/* Deezer names its genres in the language of wherever the request comes from unless asked for one */
#define ANSWER_LANGUAGE "en"
/* A service that is busy is asked again after this long, times the number of tries, up to the last try */
#define BUSY_WAIT_MS 3000
#define BUSY_TRIES 4
/* libsoup names no status for this, which services send when asked too often */
#define TOO_MANY_REQUESTS 429
/* Without a connection the gathering rests, twice as long after each failure up to the longest rest */
#define OFFLINE_WAIT_MS 30000
#define OFFLINE_WAIT_MAXIMUM_MS 600000

/* The names data/song-info.js gives each field, in the order of InfoField, which are also the database columns */
static const char *const FIELD_NAMES[INFO_FIELD_COUNT] = {
	"title", "artist", "album", "seconds", "released", "uploaded", "bpm", "key", "gain", "isrc", "explicit", "label",
	"genres", "tags", "tagSource", "styles", "lastfm", "views", "channel", "description", "chapters", "sources", "cover", "danceable", "energetic", "calm",
	"happy", "sad", "acoustic", "instrumental",
};

/* Each service is asked at most once in this many milliseconds, well inside what each of them allows */
static const struct {
	const char *host;
	int interval;
} PACES[] = {
	{"www.youtube.com", 500},
	{"api.deezer.com", 300},
	{"musicbrainz.org", 1100},
	{"api.reccobeats.com", 500},
	{"ws.audioscrobbler.com", 300},
};
#define DEFAULT_PACE_MS 500

typedef struct {
	char *url, *body;
	int tries;
} Request;

static sqlite3 *database;
static GHashTable *infos, *custom_tags, *waiting;
static GQueue *queue;
static SoupSession *session;
static JSCContext *context;
static int script_version;
static SongInfoChanged changed;
static gpointer changed_data;

/* The song being gathered, its chain of requests in the script, and the request it waits on */
static CrimsonSong *current;
static JSCValue *job;
static Request *pending;
static guint source;
static gboolean is_sending;
static gboolean is_allowed;
static int offline_failures;
static GHashTable *host_ready;


static void free_song_info(gpointer data) {
	SongInfo *info = data;
	for (int field = 0; field < INFO_FIELD_COUNT; field++)
		g_free(info->values[field]);
	g_free(info);
}


static void free_request(Request *request) {
	g_free(request->url);
	g_free(request->body);
	g_free(request);
}


/* Database ================================================================ */


static void run_sql(const char *sql) {
	char *error = NULL;
	if (sqlite3_exec(database, sql, NULL, NULL, &error) != SQLITE_OK) {
		g_debug("Song information: %s", error);
		sqlite3_free(error);
	}
}


/* Every field is a column, added to an older database when a new field appears */
static void open_database(void) {
	g_autofree char *folder = g_build_filename(g_get_user_data_dir(), get_application_identity()->slug, NULL);
	g_mkdir_with_parents(folder, 0700);
	g_autofree char *path = g_build_filename(folder, DATABASE_FILE, NULL);
	if (sqlite3_open(path, &database) != SQLITE_OK) {
		g_warning("Song information store did not open: %s", sqlite3_errmsg(database));
		return;
	}
	run_sql("CREATE TABLE IF NOT EXISTS songs (video_id TEXT PRIMARY KEY, version INTEGER, gathered INTEGER)");
	run_sql("CREATE TABLE IF NOT EXISTS custom_tags (video_id TEXT PRIMARY KEY, tags TEXT)");
	g_autoptr(GHashTable) columns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sqlite3_stmt *row = NULL;
	sqlite3_prepare_v2(database, "PRAGMA table_info(songs)", -1, &row, NULL);
	while (row && sqlite3_step(row) == SQLITE_ROW)
		g_hash_table_add(columns, g_strdup((const char *)sqlite3_column_text(row, 1)));
	sqlite3_finalize(row);
	for (int field = 0; field < INFO_FIELD_COUNT; field++) {
		if (g_hash_table_contains(columns, FIELD_NAMES[field]))
			continue;
		g_autofree char *sql = g_strdup_printf("ALTER TABLE songs ADD COLUMN \"%s\" TEXT", FIELD_NAMES[field]);
		run_sql(sql);
	}
}


static char *join_columns(const char *before, const char *each) {
	GString *text = g_string_new(before);
	for (int field = 0; field < INFO_FIELD_COUNT; field++)
		g_string_append_printf(text, each, FIELD_NAMES[field]);
	return g_string_free(text, FALSE);
}


static void load_database(void) {
	g_autofree char *columns = join_columns("SELECT video_id, version", ", \"%s\"");
	g_autofree char *sql = g_strconcat(columns, " FROM songs", NULL);
	sqlite3_stmt *row = NULL;
	sqlite3_prepare_v2(database, sql, -1, &row, NULL);
	while (row && sqlite3_step(row) == SQLITE_ROW) {
		SongInfo *info = g_new0(SongInfo, 1);
		info->version = sqlite3_column_int(row, 1);
		for (int field = 0; field < INFO_FIELD_COUNT; field++) {
			const char *value = (const char *)sqlite3_column_text(row, field + 2);
			info->values[field] = g_strdup(value ? value : "");
		}
		g_hash_table_replace(infos, g_strdup((const char *)sqlite3_column_text(row, 0)), info);
	}
	sqlite3_finalize(row);
	sqlite3_prepare_v2(database, "SELECT video_id, tags FROM custom_tags", -1, &row, NULL);
	while (row && sqlite3_step(row) == SQLITE_ROW) {
		char **tags = g_strsplit((const char *)sqlite3_column_text(row, 1), "\n", -1);
		g_hash_table_replace(custom_tags, g_strdup((const char *)sqlite3_column_text(row, 0)), tags);
	}
	sqlite3_finalize(row);
}


static void save_song_info(const char *video_id, const SongInfo *info) {
	g_autofree char *columns = join_columns("INSERT OR REPLACE INTO songs (video_id, version, gathered", ", \"%s\"");
	GString *sql = g_string_new(columns);
	g_string_append(sql, ") VALUES (?, ?, ?");
	for (int field = 0; field < INFO_FIELD_COUNT; field++)
		g_string_append(sql, ", ?");
	g_string_append(sql, ")");
	sqlite3_stmt *row = NULL;
	if (sqlite3_prepare_v2(database, sql->str, -1, &row, NULL) == SQLITE_OK) {
		sqlite3_bind_text(row, 1, video_id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(row, 2, info->version);
		sqlite3_bind_int64(row, 3, g_get_real_time() / G_USEC_PER_SEC);
		for (int field = 0; field < INFO_FIELD_COUNT; field++)
			sqlite3_bind_text(row, field + 4, info->values[field], -1, SQLITE_TRANSIENT);
		sqlite3_step(row);
	}
	sqlite3_finalize(row);
	g_string_free(sql, TRUE);
}


/* The script ============================================================== */


static JSCValue *call_script(const char *name, GType first_type, ...) {
	g_autoptr(JSCValue) function = jsc_context_get_value(context, name);
	va_list arguments;
	va_start(arguments, first_type);
	GPtrArray *values = g_ptr_array_new_with_free_func(g_object_unref);
	for (GType type = first_type; type != G_TYPE_NONE; type = va_arg(arguments, GType)) {
		if (type == G_TYPE_STRING) {
			const char *text = va_arg(arguments, const char *);
			g_ptr_array_add(values, text ? jsc_value_new_string(context, text) : jsc_value_new_null(context));
		} else {
			g_ptr_array_add(values, g_object_ref(va_arg(arguments, JSCValue *)));
		}
	}
	va_end(arguments);
	JSCValue *result = jsc_value_function_callv(function, values->len, (JSCValue **)values->pdata);
	g_ptr_array_unref(values);
	JSCException *exception = jsc_context_get_exception(context);
	if (exception) {
		g_warning("Song information script failed in %s: %s", name, jsc_exception_get_message(exception));
		jsc_context_clear_exception(context);
	}
	return result;
}


static char *read_property(JSCValue *object, const char *name) {
	g_autoptr(JSCValue) value = jsc_value_object_get_property(object, name);
	if (!value || jsc_value_is_undefined(value) || jsc_value_is_null(value))
		return g_strdup("");
	return jsc_value_to_string(value);
}


static void load_script(void) {
	g_autoptr(JSCVirtualMachine) machine = jsc_virtual_machine_new();
	context = jsc_context_new_with_virtual_machine(machine);
	g_autoptr(JSCValue) key = jsc_value_new_string(context, get_lastfm_key());
	jsc_context_set_value(context, "LASTFM_KEY", key);
	g_autofree char *path = get_resource_path(SCRIPT_NAME);
	g_autoptr(GBytes) source_bytes = g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
	gsize size = 0;
	const char *text = g_bytes_get_data(source_bytes, &size);
	g_autofree char *uri = g_strconcat("resource://", path, NULL);
	g_autoptr(JSCValue) ignored = jsc_context_evaluate_with_source_uri(context, text, (gssize)size, uri, 1);
	g_autoptr(JSCValue) version = jsc_context_evaluate(context, "VERSION", -1);
	script_version = jsc_value_to_int32(version);
}


/* Gathering =============================================================== */


static void advance(void);


static gboolean advance_later(gpointer data) {
	source = 0;
	advance();
	return G_SOURCE_REMOVE;
}


static void schedule(guint milliseconds) {
	if (!source)
		source = g_timeout_add(milliseconds, advance_later, NULL);
}


static gboolean is_current(const SongInfo *info) {
	return info && (!script_version || info->version >= script_version);
}


static void finish_job(void) {
	g_autoptr(JSCValue) result = call_script("finishJob", JSC_TYPE_VALUE, job, G_TYPE_NONE);
	SongInfo *info = g_new0(SongInfo, 1);
	for (int field = 0; field < INFO_FIELD_COUNT; field++)
		info->values[field] = read_property(result, FIELD_NAMES[field]);
	g_autofree char *version = read_property(result, "version");
	info->version = atoi(version);
	g_autofree char *video_id = g_strdup(current->id);
	save_song_info(video_id, info);
	g_hash_table_replace(infos, g_strdup(video_id), info);
	g_clear_object(&job);
	g_clear_object(&current);
	if (changed)
		changed(video_id, changed_data);
}


static gboolean start_job(void) {
	while (!g_queue_is_empty(queue)) {
		current = g_queue_pop_head(queue);
		g_hash_table_remove(waiting, current->id);
		if (!is_current(g_hash_table_lookup(infos, current->id)))
			break;
		g_clear_object(&current);
	}
	if (!current)
		return FALSE;
	job = call_script("startJob", G_TYPE_STRING, current->id, G_TYPE_STRING, current->title, G_TYPE_STRING,
		current->artist, G_TYPE_STRING, current->album, G_TYPE_STRING, current->duration, G_TYPE_NONE);
	return TRUE;
}


static gint64 *find_host_ready(const char *url, int *interval) {
	g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	const char *host = uri ? g_uri_get_host(uri) : "";
	*interval = DEFAULT_PACE_MS;
	for (guint index = 0; index < G_N_ELEMENTS(PACES); index++) {
		if (g_str_equal(host, PACES[index].host))
			*interval = PACES[index].interval;
	}
	gint64 *ready = g_hash_table_lookup(host_ready, host);
	if (!ready) {
		ready = g_new0(gint64, 1);
		g_hash_table_insert(host_ready, g_strdup(host), ready);
	}
	return ready;
}


static void take_answer(const char *text) {
	g_clear_pointer(&pending, free_request);
	g_autoptr(JSCValue) ignored = call_script("takeAnswer", JSC_TYPE_VALUE, job, G_TYPE_STRING, text, G_TYPE_NONE);
	advance();
}


static void receive_answer(GObject *object, GAsyncResult *result, gpointer data) {
	is_sending = FALSE;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(SOUP_SESSION(object), result, &error);
	SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(object), result);
	guint status = message ? soup_message_get_status(message) : 0;
	if (!bytes) {
		offline_failures++;
		guint wait = MIN(OFFLINE_WAIT_MS << MIN(offline_failures - 1, 5), OFFLINE_WAIT_MAXIMUM_MS);
		g_debug("Song information request failed, resting %u ms: %s", wait, error ? error->message : "");
		schedule(wait);
		return;
	}
	offline_failures = 0;
	gboolean is_busy = status == TOO_MANY_REQUESTS || status == SOUP_STATUS_SERVICE_UNAVAILABLE
		|| status == SOUP_STATUS_BAD_GATEWAY || status == SOUP_STATUS_GATEWAY_TIMEOUT;
	if (is_busy && ++pending->tries < BUSY_TRIES) {
		schedule(BUSY_WAIT_MS * pending->tries);
		return;
	}
	if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
		take_answer(NULL);
		return;
	}
	g_autofree char *text = g_strndup(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
	take_answer(text);
}


static void send_request(Request *request) {
	g_autoptr(SoupMessage) message = soup_message_new(request->body ? SOUP_METHOD_POST : SOUP_METHOD_GET, request->url);
	if (!message) {
		take_answer(NULL);
		return;
	}
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	soup_message_headers_replace(headers, "Accept-Language", ANSWER_LANGUAGE);
	if (g_str_has_prefix(request->url, "https://www.youtube.com/"))
		soup_message_headers_replace(headers, "User-Agent", YOUTUBE_AGENT);
	if (request->body) {
		g_autoptr(GBytes) body = g_bytes_new(request->body, strlen(request->body));
		soup_message_set_request_body_from_bytes(message, "application/json", body);
	}
	is_sending = TRUE;
	soup_session_send_and_read_async(session, message, G_PRIORITY_LOW, NULL, receive_answer, NULL);
}


/* Asks for the next thing the script wants, once its service may be asked again, and moves on to the next song
 * when the script has all it can get */
static void advance(void) {
	if (source || is_sending || !is_allowed)
		return;
	if (!job && !start_job())
		return;
	if (!pending) {
		g_autoptr(JSCValue) request = call_script("nextRequest", JSC_TYPE_VALUE, job, G_TYPE_NONE);
		if (!request || !jsc_value_is_object(request)) {
			finish_job();
			schedule(0);
			return;
		}
		pending = g_new0(Request, 1);
		pending->url = read_property(request, "url");
		g_autofree char *body = read_property(request, "body");
		pending->body = *body ? g_steal_pointer(&body) : NULL;
	}
	int interval = 0;
	gint64 *ready = find_host_ready(pending->url, &interval);
	gint64 now = g_get_monotonic_time();
	if (*ready > now) {
		schedule((guint)((*ready - now) / 1000) + 1);
		return;
	}
	*ready = now + (gint64)interval * 1000;
	send_request(pending);
}


/* Public ================================================================== */


void start_song_info(SongInfoChanged on_changed, gpointer data) {
	changed = on_changed;
	changed_data = data;
	infos = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_song_info);
	custom_tags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_strfreev);
	waiting = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	host_ready = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	queue = g_queue_new();
	g_autofree char *agent = g_strdup_printf("CrimsonMusic/%s %s", get_application_identity()->version, CONTACT);
	session = soup_session_new_with_options("user-agent", agent, "max-conns", 2, NULL);
	open_database();
	load_database();
}


/* WebKit has to set up JavaScriptCore in this process first, so the script waits for the page */
void allow_song_info(void) {
	if (!context)
		load_script();
	is_allowed = TRUE;
	advance();
}


const SongInfo *get_song_info(const char *video_id) {
	return video_id && infos ? g_hash_table_lookup(infos, video_id) : NULL;
}


const char *get_song_info_value(const char *video_id, InfoField field) {
	const SongInfo *info = get_song_info(video_id);
	return info ? info->values[field] : "";
}


double get_song_info_number(const char *video_id, InfoField field) {
	const char *value = get_song_info_value(video_id, field);
	return *value ? g_ascii_strtod(value, NULL) : -1;
}


static void add_waiting(CrimsonSong *song, gboolean is_first) {
	if (!song || !*song->id || (current && g_str_equal(current->id, song->id)))
		return;
	if (g_hash_table_contains(waiting, song->id)) {
		if (!is_first)
			return;
		for (GList *link = queue->head; link; link = link->next) {
			if (g_str_equal(((CrimsonSong *)link->data)->id, song->id)) {
				g_queue_unlink(queue, link);
				g_queue_push_head_link(queue, link);
				break;
			}
		}
		return;
	}
	g_hash_table_add(waiting, g_strdup(song->id));
	if (is_first)
		g_queue_push_head(queue, g_object_ref(song));
	else
		g_queue_push_tail(queue, g_object_ref(song));
}


/* A local file is looked up only when its tags name the artist, since a bare file name finds the wrong songs */
static gboolean is_gatherable(CrimsonSong *song) {
	return !is_local_song(song->id) || (song->artist && *song->artist);
}


void gather_song_info(GListModel *songs) {
	guint count = g_list_model_get_n_items(songs);
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonSong) song = g_list_model_get_item(songs, index);
		if (is_gatherable(song) && !is_current(g_hash_table_lookup(infos, song->id)))
			add_waiting(song, FALSE);
	}
	advance();
}


void gather_song_info_now(CrimsonSong *song) {
	if (!is_gatherable(song) || is_current(g_hash_table_lookup(infos, song->id)))
		return;
	add_waiting(song, TRUE);
	advance();
}


guint count_waiting_song_info(void) {
	return g_queue_get_length(queue) + (current ? 1 : 0);
}


gboolean is_gathering_song_info(const char *video_id) {
	return (current && g_str_equal(current->id, video_id)) || g_hash_table_contains(waiting, video_id);
}


char **get_custom_tags(const char *video_id) {
	char **tags = g_hash_table_lookup(custom_tags, video_id);
	return g_strdupv(tags);
}


void forget_song_info(const char *video_id) {
	g_hash_table_remove(infos, video_id);
	sqlite3_stmt *row = NULL;
	if (sqlite3_prepare_v2(database, "DELETE FROM songs WHERE video_id = ?", -1, &row, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_text(row, 1, video_id, -1, SQLITE_TRANSIENT);
	sqlite3_step(row);
	sqlite3_finalize(row);
}


void set_custom_tags(const char *video_id, const char *const *tags) {
	g_autofree char *joined = g_strjoinv("\n", (char **)tags);
	sqlite3_stmt *row = NULL;
	gboolean is_empty = !tags || !tags[0];
	const char *sql = is_empty ? "DELETE FROM custom_tags WHERE video_id = ?"
		: "INSERT OR REPLACE INTO custom_tags (video_id, tags) VALUES (?, ?)";
	if (sqlite3_prepare_v2(database, sql, -1, &row, NULL) == SQLITE_OK) {
		sqlite3_bind_text(row, 1, video_id, -1, SQLITE_TRANSIENT);
		if (!is_empty)
			sqlite3_bind_text(row, 2, joined, -1, SQLITE_TRANSIENT);
		sqlite3_step(row);
	}
	sqlite3_finalize(row);
	if (is_empty)
		g_hash_table_remove(custom_tags, video_id);
	else
		g_hash_table_replace(custom_tags, g_strdup(video_id), g_strdupv((char **)tags));
	if (changed)
		changed(video_id, changed_data);
}


void free_chapter(gpointer chapter) {
	g_free(((Chapter *)chapter)->title);
	g_free(chapter);
}


GPtrArray *get_song_chapters(const char *video_id) {
	GPtrArray *chapters = g_ptr_array_new_with_free_func(free_chapter);
	const char *json = get_song_info_value(video_id, INFO_CHAPTERS);
	if (!*json || !context)
		return chapters;
	g_autoptr(JSCValue) list = jsc_value_new_from_json(context, json);
	g_autoptr(JSCValue) length = list ? jsc_value_object_get_property(list, "length") : NULL;
	int count = length ? jsc_value_to_int32(length) : 0;
	for (int index = 0; index < count; index++) {
		g_autoptr(JSCValue) item = jsc_value_object_get_property_at_index(list, (guint)index);
		Chapter *chapter = g_new0(Chapter, 1);
		chapter->title = read_property(item, "title");
		g_autofree char *start = read_property(item, "start");
		chapter->start = atoi(start);
		g_ptr_array_add(chapters, chapter);
	}
	return chapters;
}


GHashTable *read_lastfm_details(char **answers) {
	if (!context)
		return NULL;
	g_autoptr(JSCValue) result = call_script("readLastfmDetails", G_TYPE_STRING, answers[0], G_TYPE_STRING,
		answers[1], G_TYPE_STRING, answers[2], G_TYPE_STRING, answers[3], G_TYPE_NONE);
	GHashTable *details = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_auto(GStrv) names = result && jsc_value_is_object(result) ? jsc_value_object_enumerate_properties(result) : NULL;
	for (char **name = names; name && *name; name++)
		g_hash_table_insert(details, g_strdup(*name), read_property(result, *name));
	return details;
}


GHashTable *read_lastfm_about(const char *text) {
	if (!context || !text)
		return NULL;
	g_autoptr(JSCValue) result = call_script("readLastfmAbout", G_TYPE_STRING, text, G_TYPE_NONE);
	if (!result || !jsc_value_is_object(result))
		return NULL;
	GHashTable *about = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_auto(GStrv) names = jsc_value_object_enumerate_properties(result);
	for (char **name = names; name && *name; name++)
		g_hash_table_insert(about, g_strdup(*name), read_property(result, *name));
	return about;
}


char **read_lastfm_loved(const char *text, int *pages) {
	if (!context)
		return NULL;
	g_autoptr(JSCValue) result = call_script("readLovedTracks", G_TYPE_STRING, text, G_TYPE_NONE);
	if (!result || !jsc_value_is_object(result))
		return NULL;
	g_autofree char *tracks = read_property(result, "tracks");
	g_autofree char *count = read_property(result, "pages");
	*pages = atoi(count);
	return *tracks ? g_strsplit(tracks, "\n", -1) : g_new0(char *, 1);
}


char **read_lastfm_similar(const char *text) {
	if (!context || !text)
		return g_new0(char *, 1);
	g_autoptr(JSCValue) result = call_script("readSimilarList", G_TYPE_STRING, text, G_TYPE_NONE);
	g_autofree char *list = result && jsc_value_is_string(result) ? jsc_value_to_string(result) : g_strdup("");
	return *list ? g_strsplit(list, "\n", -1) : g_new0(char *, 1);
}

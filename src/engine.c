#include <string.h>

#include "common/identity.h"
#include "common/log.h"
#include "engine.h"
#include "pear-session.h"

#define START_ADDRESS "https://music.youtube.com/library"
#define ALLOWED_PAGES "https://music.youtube.com/*"
#define MUSIC_ADDRESS "https://music.youtube.com/"
#define MESSAGE_HANDLER "crimson"
#define BRIDGE_RESOURCE "engine.js"
#define AD_PRUNER_RESOURCE "ad-pruner.js"
#define BLOCKLIST_RESOURCE "blocklist.json"
/* Followed by a checksum of the rules, so a changed blocklist compiles once and is loaded after that */
#define BLOCKLIST_ID_FORMAT "crimson-blocklist-%s"
#define COOKIE_FILE "cookies.sqlite"
#define CONSENT_COOKIE "SOCS"
#define CONSENT_REJECTED "CAI"
#define CONSENT_DOMAIN ".youtube.com"
#define CONSENT_SECONDS (SOUP_COOKIE_MAX_AGE_ONE_YEAR * 2)
/* The page stays this small and fully transparent, so it never shows but still counts as visible */
#define HIDDEN_SIZE 1
/* A page drawn without the GPU shows video frames as plain green when WebKit hands video to the GPU */
#define CPU_RENDERING_VARIABLE "WEBKIT_SKIA_ENABLE_CPU_RENDERING"
#define GL_VIDEO_OFF_VARIABLE "WEBKIT_GST_DISABLE_GL_SINK"

struct Engine {
	const EngineEvents *events;
	gpointer data;
	WebKitNetworkSession *session;
	WebKitUserContentManager *content;
	WebKitWebView *view;
	gboolean is_ready, is_video_shown;
	/* Requests the page had under way fail as its sign in is cleared, which says nothing worth showing */
	gboolean is_signing_out;
	EngineVideoDrawn video_drawn;
	/* Calls made before the page was ready, run in order once it is */
	GPtrArray *waiting_calls;
};

static const char *const ANSWER_TYPES[] = {"search", "artist", "album", "album-found", "liked", "disliked", "created", "edited",
	"similar-songs", "similar-artists", "suggestions", "artist-more", "lyrics", "comments", "replies", "home", "account", NULL};

/* A function body for the page with its arguments, kept until the page can run it */
typedef struct {
	char *body;
	GVariant *arguments;
} PageCall;


static void free_page_call(gpointer data) {
	PageCall *call = data;
	g_free(call->body);
	if (call->arguments)
		g_variant_unref(call->arguments);
	g_free(call);
}


static void finish_page_call(GObject *view, GAsyncResult *result, gpointer data) {
	g_autoptr(GError) error = NULL;
	g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(view),
		result, &error);
	if (error)
		g_warning("Page call failed: %s", error->message);
}


static void run_page_call(Engine *engine, PageCall *call) {
	webkit_web_view_call_async_javascript_function(engine->view, call->body, -1, call->arguments, NULL, NULL, NULL,
		finish_page_call, engine);
}


/* Takes ownership of the arguments, which may be NULL */
static void call_page(Engine *engine, const char *body, GVariant *arguments) {
	PageCall *call = g_new0(PageCall, 1);
	call->body = g_strdup(body);
	call->arguments = arguments ? g_variant_ref_sink(arguments) : NULL;
	if (!engine->is_ready) {
		g_ptr_array_add(engine->waiting_calls, call);
		return;
	}
	run_page_call(engine, call);
	free_page_call(call);
}


/* WebKit takes strings and numbers as arguments but refuses booleans, so yes and no go as 1 and 0 */
static GVariant *create_arguments(const char *first_name, ...) {
	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	va_list list;
	va_start(list, first_name);
	for (const char *name = first_name; name; name = va_arg(list, const char *))
		g_variant_builder_add(&builder, "{sv}", name, va_arg(list, GVariant *));
	va_end(list);
	return g_variant_builder_end(&builder);
}


static char *read_text(JSCValue *object, const char *name) {
	g_autoptr(JSCValue) value = jsc_value_object_get_property(object, name);
	return jsc_value_is_string(value) ? jsc_value_to_string(value) : g_strdup("");
}


static double read_number(JSCValue *object, const char *name) {
	g_autoptr(JSCValue) value = jsc_value_object_get_property(object, name);
	return jsc_value_is_number(value) ? jsc_value_to_double(value) : 0;
}


static gboolean read_flag(JSCValue *object, const char *name) {
	g_autoptr(JSCValue) value = jsc_value_object_get_property(object, name);
	return jsc_value_to_boolean(value);
}


static void become_ready(Engine *engine, gboolean is_signed_in) {
	note_startup_moment("YouTube Music page ready");
	engine->is_ready = TRUE;
	engine->is_signing_out = FALSE;
	for (guint index = 0; index < engine->waiting_calls->len; index++)
		run_page_call(engine, g_ptr_array_index(engine->waiting_calls, index));
	g_ptr_array_set_size(engine->waiting_calls, 0);
	engine->events->ready(is_signed_in, engine->data);
}


static void receive_message(WebKitUserContentManager *manager, JSCValue *message, Engine *engine) {
	g_autofree char *type = read_text(message, "type");
	if (strcmp(type, "ready") == 0) {
		become_ready(engine, read_flag(message, "signedIn"));
	} else if (strcmp(type, "playlists") == 0) {
		g_autoptr(JSCValue) playlists = jsc_value_object_get_property(message, "playlists");
		engine->events->playlists_arrived(playlists, engine->data);
	} else if (strcmp(type, "songs") == 0) {
		g_autofree char *playlist_id = read_text(message, "playlist");
		g_autoptr(JSCValue) songs = jsc_value_object_get_property(message, "songs");
		engine->events->songs_arrived(playlist_id, songs, read_flag(message, "editable"), read_flag(message, "first"),
			read_flag(message, "done"), engine->data);
	} else if (strcmp(type, "player") == 0) {
		g_autofree char *video_id = read_text(message, "video");
		PlayerReport report = {video_id, (int)read_number(message, "state"), read_number(message, "time"),
			read_number(message, "duration"), read_flag(message, "isVideo")};
		engine->events->player_changed(&report, engine->data);
	} else if (g_strv_contains(ANSWER_TYPES, type)) {
		engine->events->answer_arrived(type, message, engine->data);
	} else if (strcmp(type, "media-key") == 0) {
		g_autofree char *action = read_text(message, "action");
		engine->events->media_key(action, engine->data);
	} else if (strcmp(type, "error") == 0 && !engine->is_signing_out) {
		g_autofree char *text = read_text(message, "message");
		g_autofree char *playlist_id = read_text(message, "playlist");
		engine->events->failed(text, *playlist_id ? playlist_id : NULL, engine->data);
	}
}


/* Signed out visitors can land on Google's consent page, where the bridge never runs to say so itself */
static void notice_page(WebKitWebView *view, WebKitLoadEvent event, Engine *engine) {
	const char *address = webkit_web_view_get_uri(view);
	if (event == WEBKIT_LOAD_FINISHED && !engine->is_ready && !g_str_has_prefix(address, MUSIC_ADDRESS))
		become_ready(engine, FALSE);
}


/* The page never asks for anything the player needs, so notifications, location and the like are all refused */
static gboolean refuse_permission(WebKitWebView *view, WebKitPermissionRequest *request, gpointer data) {
	webkit_permission_request_deny(request);
	return TRUE;
}


static GBytes *read_resource(const char *name) {
	g_autofree char *path = get_resource_path(name);
	return g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
}


static void add_page_script(WebKitUserContentManager *manager, const char *name, WebKitUserScriptInjectionTime time) {
	g_autoptr(GBytes) source = read_resource(name);
	g_autofree char *text = g_strndup(g_bytes_get_data(source, NULL), g_bytes_get_size(source));
	static const char *const ALLOWED[] = {ALLOWED_PAGES, NULL};
	g_autoptr(WebKitUserScript) script = webkit_user_script_new(text, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, time,
		ALLOWED, NULL);
	webkit_user_content_manager_add_script(manager, script);
}


/* The ad pruner runs before any of the page's own scripts, so YouTube never sees the ads it removes */
static WebKitUserContentManager *create_content_manager(Engine *engine) {
	WebKitUserContentManager *manager = webkit_user_content_manager_new();
	add_page_script(manager, AD_PRUNER_RESOURCE, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START);
	add_page_script(manager, BRIDGE_RESOURCE, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END);
	webkit_user_content_manager_register_script_message_handler(manager, MESSAGE_HANDLER, NULL);
	g_signal_connect(manager, "script-message-received::" MESSAGE_HANDLER, G_CALLBACK(receive_message), engine);
	return manager;
}


static char *get_engine_path(const char *base, const char *name) {
	return g_build_filename(base, get_application_identity()->slug, "web", name, NULL);
}


Engine *create_engine(const EngineEvents *events, gpointer data) {
	Engine *engine = g_new0(Engine, 1);
	engine->events = events;
	engine->data = data;
	/* The web process copies these settings when it starts, so they are set before the first page */
	if (g_getenv(CPU_RENDERING_VARIABLE))
		g_setenv(GL_VIDEO_OFF_VARIABLE, "1", FALSE);
	engine->waiting_calls = g_ptr_array_new_with_free_func(free_page_call);
	g_autofree char *data_path = get_engine_path(g_get_user_data_dir(), NULL);
	g_autofree char *cache_path = get_engine_path(g_get_user_cache_dir(), NULL);
	engine->session = webkit_network_session_new(data_path, cache_path);
	engine->content = create_content_manager(engine);
	g_autoptr(WebKitWebsitePolicies) policies = webkit_website_policies_new_with_policies("autoplay",
		WEBKIT_AUTOPLAY_ALLOW, NULL);
	engine->view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", engine->session,
		"user-content-manager", engine->content, "website-policies", policies, NULL);
	g_object_ref_sink(engine->view);
	/* The page is never seen, so its pictures are never fetched, which lets it get ready to play sooner */
	webkit_settings_set_auto_load_images(webkit_web_view_get_settings(engine->view), FALSE);
	gtk_widget_set_size_request(GTK_WIDGET(engine->view), HIDDEN_SIZE, HIDDEN_SIZE);
	gtk_widget_set_halign(GTK_WIDGET(engine->view), GTK_ALIGN_START);
	gtk_widget_set_valign(GTK_WIDGET(engine->view), GTK_ALIGN_START);
	gtk_widget_set_opacity(GTK_WIDGET(engine->view), 0);
	gtk_widget_set_can_target(GTK_WIDGET(engine->view), FALSE);
	gtk_widget_set_can_focus(GTK_WIDGET(engine->view), FALSE);
	g_signal_connect(engine->view, "permission-request", G_CALLBACK(refuse_permission), NULL);
	g_signal_connect(engine->view, "load-changed", G_CALLBACK(notice_page), engine);
	return engine;
}


GtkWidget *get_engine_widget(Engine *engine) {
	return GTK_WIDGET(engine->view);
}


WebKitNetworkSession *get_engine_session(Engine *engine) {
	return engine->session;
}


static void open_music(GObject *cookies, GAsyncResult *result, gpointer data) {
	Engine *engine = data;
	webkit_cookie_manager_add_cookie_finish(WEBKIT_COOKIE_MANAGER(cookies), result, NULL);
	webkit_web_view_load_uri(engine->view, START_ADDRESS);
}


/* Visitors who are not signed in meet Google's cookie consent page first, which the bridge never runs on, so the
 * choice to reject the optional cookies is made in advance, as YouTube's consent cookie writes it */
void reload_engine(Engine *engine) {
	note_startup_moment("Opening YouTube Music");
	engine->is_ready = FALSE;
	WebKitCookieManager *cookies = webkit_network_session_get_cookie_manager(engine->session);
	g_autoptr(SoupCookie) consent = soup_cookie_new(CONSENT_COOKIE, CONSENT_REJECTED, CONSENT_DOMAIN, "/",
		CONSENT_SECONDS);
	soup_cookie_set_secure(consent, TRUE);
	webkit_cookie_manager_add_cookie(cookies, consent, NULL, open_music, engine);
}


static void open_start_page(gpointer data) {
	reload_engine(data);
}


/* A first launch borrows the Pear Desktop sign in when there is one, so most people never see a sign in page */
static void open_session(Engine *engine) {
	note_startup_moment("Blocklist loaded");
	g_autofree char *cookie_path = get_engine_path(g_get_user_data_dir(), COOKIE_FILE);
	gboolean is_first_launch = !g_file_test(cookie_path, G_FILE_TEST_EXISTS);
	WebKitCookieManager *cookies = webkit_network_session_get_cookie_manager(engine->session);
	g_autofree char *directory = g_path_get_dirname(cookie_path);
	g_mkdir_with_parents(directory, 0700);
	webkit_cookie_manager_set_persistent_storage(cookies, cookie_path, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
	if (is_first_launch)
		import_pear_session(cookies, open_start_page, engine);
	else
		reload_engine(engine);
}


static void use_blocklist(Engine *engine, WebKitUserContentFilter *filter, GError *error) {
	if (filter)
		webkit_user_content_manager_add_filter(engine->content, filter);
	else
		g_warning("The ad and tracker blocklist was not loaded: %s", error->message);
	open_session(engine);
}


static void finish_compiling_blocklist(GObject *store, GAsyncResult *result, gpointer data) {
	g_autoptr(GError) error = NULL;
	g_autoptr(WebKitUserContentFilter) filter = webkit_user_content_filter_store_save_finish(
		WEBKIT_USER_CONTENT_FILTER_STORE(store), result, &error);
	use_blocklist(data, filter, error);
}


static char *get_blocklist_id(GBytes *rules) {
	g_autofree char *checksum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA1, rules);
	return g_strdup_printf(BLOCKLIST_ID_FORMAT, checksum);
}


/* Compiling takes about half a second, so it happens only when the stored copy is missing */
static void finish_loading_blocklist(GObject *store, GAsyncResult *result, gpointer data) {
	Engine *engine = data;
	g_autoptr(WebKitUserContentFilter) filter = webkit_user_content_filter_store_load_finish(
		WEBKIT_USER_CONTENT_FILTER_STORE(store), result, NULL);
	if (filter) {
		use_blocklist(engine, filter, NULL);
		return;
	}
	g_autoptr(GBytes) rules = read_resource(BLOCKLIST_RESOURCE);
	g_autofree char *id = get_blocklist_id(rules);
	webkit_user_content_filter_store_save(WEBKIT_USER_CONTENT_FILTER_STORE(store), id, rules, NULL,
		finish_compiling_blocklist, engine);
}


/* The blocklist is in place before the first request, so the page never reaches the ad and tracker hosts */
void start_engine(Engine *engine) {
	note_startup_moment("Engine starting");
	g_autofree char *store_path = get_engine_path(g_get_user_cache_dir(), "filters");
	g_autoptr(WebKitUserContentFilterStore) store = webkit_user_content_filter_store_new(store_path);
	g_autoptr(GBytes) rules = read_resource(BLOCKLIST_RESOURCE);
	g_autofree char *id = get_blocklist_id(rules);
	webkit_user_content_filter_store_load(store, id, NULL, finish_loading_blocklist, engine);
}


static void finish_sign_out(GObject *manager, GAsyncResult *result, gpointer data) {
	webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(manager), result, NULL);
	reload_engine(data);
}


void sign_out_engine(Engine *engine) {
	engine->is_ready = FALSE;
	engine->is_signing_out = TRUE;
	webkit_web_view_stop_loading(engine->view);
	WebKitWebsiteDataManager *manager = webkit_network_session_get_website_data_manager(engine->session);
	webkit_website_data_manager_clear(manager, WEBKIT_WEBSITE_DATA_ALL, 0, NULL, finish_sign_out, engine);
}


void request_library(Engine *engine) {
	call_page(engine, "return crimson.loadLibrary()", NULL);
}


void request_home(Engine *engine) {
	call_page(engine, "return crimson.loadHome()", NULL);
}


void request_playlist(Engine *engine, const char *playlist_id) {
	call_page(engine, "return crimson.loadPlaylist(id)", create_arguments("id", g_variant_new_string(playlist_id), NULL));
}


void request_suggestions(Engine *engine, const char *playlist_id) {
	call_page(engine, "return crimson.loadSuggestions(id)",
		create_arguments("id", g_variant_new_string(playlist_id), NULL));
}


void play_video(Engine *engine, const char *video_id, int volume, gboolean is_resuming) {
	call_page(engine, "return crimson.play(id, volume, resume)", create_arguments("id", g_variant_new_string(video_id),
		"volume", g_variant_new_int32(volume), "resume", g_variant_new_int32(is_resuming), NULL));
}


void pause_video(Engine *engine) {
	call_page(engine, "crimson.pause()", NULL);
}


static void finish_video_call(GObject *view, GAsyncResult *result, gpointer data) {
	Engine *engine = data;
	g_autoptr(JSCValue) value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(view), result, NULL);
	if (engine->is_video_shown && engine->video_drawn)
		engine->video_drawn(engine->data);
}


void show_engine_video(Engine *engine, gboolean is_shown, EngineVideoDrawn drawn) {
	engine->is_video_shown = is_shown;
	engine->video_drawn = drawn;
	gtk_widget_set_opacity(GTK_WIDGET(engine->view), is_shown);
	if (!engine->is_ready)
		return;
	g_autoptr(GVariant) arguments = g_variant_ref_sink(create_arguments("shown", g_variant_new_int32(is_shown), NULL));
	webkit_web_view_call_async_javascript_function(engine->view, "return crimson.showVideo(shown)", -1, arguments, NULL,
		NULL, NULL, finish_video_call, engine);
}


/* Muting takes effect at once, where pausing waits for the page's script, and the page's later reports no longer
 * reach the window that is closing */
void silence_engine(Engine *engine) {
	webkit_web_view_set_is_muted(engine->view, TRUE);
	pause_video(engine);
	g_signal_handlers_disconnect_by_data(engine->content, engine);
}


void resume_video(Engine *engine) {
	call_page(engine, "crimson.resume()", NULL);
}


void seek_video(Engine *engine, double seconds) {
	call_page(engine, "crimson.seek(seconds)", create_arguments("seconds", g_variant_new_double(seconds), NULL));
}


void set_video_volume(Engine *engine, int volume) {
	call_page(engine, "crimson.setVolume(volume)", create_arguments("volume", g_variant_new_int32(volume), NULL));
}


void request_search(Engine *engine, const char *query, int token) {
	call_page(engine, "return crimson.search(query, token)", create_arguments("query", g_variant_new_string(query),
		"token", g_variant_new_int32(token), NULL));
}


void request_similar_songs(Engine *engine, int token, const char *video_id, char **pairs) {
	/* WebKit passes neither nested nor empty lists into the page, so the names go as lines of one text, artist and
	 * title after each other */
	g_autofree char *lines = pairs ? g_strjoinv("\n", pairs) : g_strdup("");
	call_page(engine, "return crimson.similarSongs(token, video, lines ? lines.split('\\n') : [])", create_arguments(
		"token", g_variant_new_int32(token), "video", g_variant_new_string(video_id), "lines",
		g_variant_new_string(lines), NULL));
}


void request_similar_artists(Engine *engine, int token, const char *artist_id, char **names) {
	g_autofree char *lines = names ? g_strjoinv("\n", names) : g_strdup("");
	call_page(engine, "return crimson.similarArtists(token, id, lines ? lines.split('\\n') : [])", create_arguments(
		"token", g_variant_new_int32(token), "id", g_variant_new_string(artist_id ? artist_id : ""), "lines",
		g_variant_new_string(lines), NULL));
}


void request_artist(Engine *engine, const char *artist_id) {
	call_page(engine, "return crimson.loadArtist(id)", create_arguments("id", g_variant_new_string(artist_id), NULL));
}


void request_album(Engine *engine, const char *album_id) {
	call_page(engine, "return crimson.loadAlbum(id)", create_arguments("id", g_variant_new_string(album_id), NULL));
}


void create_playlist(Engine *engine, const char *title) {
	call_page(engine, "return crimson.createPlaylist(title)", create_arguments("title", g_variant_new_string(title),
		NULL));
}


void add_to_playlist(Engine *engine, const char *playlist_id, const char *video_id) {
	call_page(engine, "return crimson.addToPlaylist(id, video)", create_arguments("id",
		g_variant_new_string(playlist_id), "video", g_variant_new_string(video_id), NULL));
}


void remove_from_playlist(Engine *engine, const char *playlist_id, const char *video_id, const char *set_id) {
	call_page(engine, "return crimson.removeFromPlaylist(id, video, set)", create_arguments("id",
		g_variant_new_string(playlist_id), "video", g_variant_new_string(video_id), "set", g_variant_new_string(set_id),
		NULL));
}


void move_in_playlist(Engine *engine, const char *playlist_id, const char *set_id, const char *successor_id) {
	call_page(engine, "return crimson.moveInPlaylist(id, set, successor)", create_arguments("id",
		g_variant_new_string(playlist_id), "set", g_variant_new_string(set_id), "successor",
		g_variant_new_string(successor_id), NULL));
}


void rename_playlist(Engine *engine, const char *playlist_id, const char *title) {
	call_page(engine, "return crimson.renamePlaylist(id, title)", create_arguments("id",
		g_variant_new_string(playlist_id), "title", g_variant_new_string(title), NULL));
}


void delete_playlist(Engine *engine, const char *playlist_id) {
	call_page(engine, "return crimson.deletePlaylist(id)", create_arguments("id", g_variant_new_string(playlist_id),
		NULL));
}


void request_comments(Engine *engine, const char *video_id, const char *token) {
	call_page(engine, "return crimson.loadComments(video, token)", create_arguments("video",
		g_variant_new_string(video_id), "token", g_variant_new_string(token ? token : ""), NULL));
}


void request_replies(Engine *engine, const char *video_id, const char *token, guint id) {
	call_page(engine, "return crimson.loadReplies(video, token, id)", create_arguments("video",
		g_variant_new_string(video_id), "token", g_variant_new_string(token), "id", g_variant_new_int32((int)id), NULL));
}


void request_lyrics(Engine *engine, const char *video_id) {
	call_page(engine, "return crimson.loadLyrics(video)", create_arguments("video", g_variant_new_string(video_id), NULL));
}


void find_album(Engine *engine, const char *video_id) {
	call_page(engine, "return crimson.findAlbum(video)", create_arguments("video", g_variant_new_string(video_id), NULL));
}


void like_song(Engine *engine, const char *video_id, gboolean is_liked) {
	call_page(engine, "return crimson.likeSong(video, liked)", create_arguments("video", g_variant_new_string(video_id),
		"liked", g_variant_new_int32(is_liked), NULL));
}


void dislike_song(Engine *engine, const char *video_id, gboolean is_disliked) {
	call_page(engine, "return crimson.dislikeSong(video, disliked)",
		create_arguments("video", g_variant_new_string(video_id), "disliked", g_variant_new_int32(is_disliked), NULL));
}

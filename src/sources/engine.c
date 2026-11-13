#include <string.h>

#include "sources/engine.h"

#define START_ADDRESS "https://music.youtube.com/library"
#define MUSIC_ADDRESS "https://music.youtube.com/"
#define SITE_NAME "YouTube Music"
#define BRIDGE_RESOURCE "engine.js"
/* The page stays this small and fully transparent, so it never shows but still counts as visible */
#define HIDDEN_SIZE 1

static const YouTubeSite MUSIC_SITE = {MUSIC_ADDRESS, START_ADDRESS, BRIDGE_RESOURCE, SITE_NAME};

struct Engine {
	const EngineEvents *events;
	gpointer data;
	YouTubePage *page;
	EngineVideoDrawn video_drawn;
};

static const char *const ANSWER_TYPES[] = {"search", "artist", "album", "album-found", "liked", "disliked", "created", "edited",
	"similar-songs", "similar-artists", "suggestions", "artist-more", "lyrics", "comments", "replies", "home", "account", NULL};


static void notice_ready(gboolean is_signed_in, gpointer data) {
	Engine *engine = data;
	engine->events->ready(is_signed_in, engine->data);
}


static void notice_player(const YouTubePlayer *player, JSCValue *message, gpointer data) {
	Engine *engine = data;
	PlayerReport report = {player->video_id, player->state, player->time, player->duration,
		read_page_flag(message, "isVideo")};
	engine->events->player_changed(&report, engine->data);
}


static void notice_message(const char *type, JSCValue *message, gpointer data) {
	Engine *engine = data;
	if (strcmp(type, "playlists") == 0) {
		g_autoptr(JSCValue) playlists = jsc_value_object_get_property(message, "playlists");
		engine->events->playlists_arrived(playlists, engine->data);
	} else if (strcmp(type, "songs") == 0) {
		g_autofree char *playlist_id = read_page_text(message, "playlist");
		g_autoptr(JSCValue) songs = jsc_value_object_get_property(message, "songs");
		engine->events->songs_arrived(playlist_id, songs, read_page_flag(message, "editable"),
			read_page_flag(message, "first"), read_page_flag(message, "done"), engine->data);
	} else if (g_strv_contains(ANSWER_TYPES, type)) {
		engine->events->answer_arrived(type, message, engine->data);
	}
}


static void notice_failure(const char *text, JSCValue *message, gpointer data) {
	Engine *engine = data;
	g_autofree char *playlist_id = read_page_text(message, "playlist");
	engine->events->failed(text, *playlist_id ? playlist_id : NULL, engine->data);
}


static void notice_media_key(const char *action, gpointer data) {
	Engine *engine = data;
	engine->events->media_key(action, engine->data);
}


static const YouTubePageEvents PAGE_EVENTS = {notice_ready, notice_player, notice_message, notice_failure,
	notice_media_key};


Engine *create_engine(const EngineEvents *events, gpointer data) {
	Engine *engine = g_new0(Engine, 1);
	engine->events = events;
	engine->data = data;
	engine->page = create_youtube_page(&MUSIC_SITE, &PAGE_EVENTS, engine);
	GtkWidget *view = GTK_WIDGET(get_youtube_page_view(engine->page));
	/* The page is never seen, so its pictures are never fetched, which lets it get ready to play sooner */
	webkit_settings_set_auto_load_images(webkit_web_view_get_settings(WEBKIT_WEB_VIEW(view)), FALSE);
	gtk_widget_set_size_request(view, HIDDEN_SIZE, HIDDEN_SIZE);
	gtk_widget_set_halign(view, GTK_ALIGN_START);
	gtk_widget_set_valign(view, GTK_ALIGN_START);
	gtk_widget_set_opacity(view, 0);
	gtk_widget_set_can_target(view, FALSE);
	gtk_widget_set_can_focus(view, FALSE);
	return engine;
}


GtkWidget *get_engine_widget(Engine *engine) {
	return GTK_WIDGET(get_youtube_page_view(engine->page));
}


WebKitNetworkSession *get_engine_session(Engine *engine) {
	return get_youtube_page_session(engine->page);
}


void start_engine(Engine *engine) {
	start_youtube_page(engine->page);
}


void reload_engine(Engine *engine) {
	reload_youtube_page(engine->page);
}


void sign_out_engine(Engine *engine) {
	sign_out_youtube_page(engine->page);
}


void show_sign_in(Engine *engine, GtkWidget *parent, const char *title, GDestroyNotify done, gpointer data) {
	show_youtube_sign_in(engine->page, parent, title, done, data);
}


void play_video(Engine *engine, const char *video_id, int volume, gboolean is_resuming) {
	play_youtube_video(engine->page, video_id, volume, is_resuming);
}


void pause_video(Engine *engine) {
	pause_youtube_video(engine->page);
}


void resume_video(Engine *engine) {
	resume_youtube_video(engine->page);
}


void seek_video(Engine *engine, double seconds) {
	seek_youtube_video(engine->page, seconds);
}


void set_video_volume(Engine *engine, int volume) {
	set_youtube_volume(engine->page, volume);
}


void silence_engine(Engine *engine) {
	silence_youtube_page(engine->page);
}


static void notice_video_drawn(gpointer data) {
	Engine *engine = data;
	engine->video_drawn(engine->data);
}


/* The page shows only while its video does */
void show_engine_video(Engine *engine, gboolean is_shown, EngineVideoDrawn drawn) {
	engine->video_drawn = drawn;
	gtk_widget_set_opacity(get_engine_widget(engine), is_shown);
	show_youtube_video(engine->page, is_shown, drawn ? notice_video_drawn : NULL);
}


void request_comments(Engine *engine, const char *video_id, const char *token) {
	request_youtube_comments(engine->page, video_id, token);
}


void request_replies(Engine *engine, const char *video_id, const char *token, guint id) {
	request_youtube_replies(engine->page, video_id, token, id);
}


void request_library(Engine *engine) {
	call_youtube_page(engine->page, "return crimson.loadLibrary()", NULL);
}


void request_home(Engine *engine) {
	call_youtube_page(engine->page, "return crimson.loadHome()", NULL);
}


void request_playlist(Engine *engine, const char *playlist_id) {
	call_youtube_page(engine->page, "return crimson.loadPlaylist(id)", create_page_arguments("id", g_variant_new_string(playlist_id), NULL));
}


void request_suggestions(Engine *engine, const char *playlist_id) {
	call_youtube_page(engine->page, "return crimson.loadSuggestions(id)",
		create_page_arguments("id", g_variant_new_string(playlist_id), NULL));
}


void request_search(Engine *engine, const char *query, int token) {
	call_youtube_page(engine->page, "return crimson.search(query, token)", create_page_arguments("query", g_variant_new_string(query),
		"token", g_variant_new_int32(token), NULL));
}


void request_similar_songs(Engine *engine, int token, const char *video_id, char **pairs) {
	/* WebKit passes neither nested nor empty lists into the page, so the names go as lines of one text, artist and
	 * title after each other */
	g_autofree char *lines = pairs ? g_strjoinv("\n", pairs) : g_strdup("");
	call_youtube_page(engine->page, "return crimson.similarSongs(token, video, lines ? lines.split('\\n') : [])", create_page_arguments(
		"token", g_variant_new_int32(token), "video", g_variant_new_string(video_id), "lines",
		g_variant_new_string(lines), NULL));
}


void request_similar_artists(Engine *engine, int token, const char *artist_id, char **names) {
	g_autofree char *lines = names ? g_strjoinv("\n", names) : g_strdup("");
	call_youtube_page(engine->page, "return crimson.similarArtists(token, id, lines ? lines.split('\\n') : [])", create_page_arguments(
		"token", g_variant_new_int32(token), "id", g_variant_new_string(artist_id ? artist_id : ""), "lines",
		g_variant_new_string(lines), NULL));
}


void request_artist(Engine *engine, const char *artist_id) {
	call_youtube_page(engine->page, "return crimson.loadArtist(id)", create_page_arguments("id", g_variant_new_string(artist_id), NULL));
}


void request_album(Engine *engine, const char *album_id) {
	call_youtube_page(engine->page, "return crimson.loadAlbum(id)", create_page_arguments("id", g_variant_new_string(album_id), NULL));
}


void create_playlist(Engine *engine, const char *title) {
	call_youtube_page(engine->page, "return crimson.createPlaylist(title)", create_page_arguments("title", g_variant_new_string(title),
		NULL));
}


void add_to_playlist(Engine *engine, const char *playlist_id, const char *video_id) {
	call_youtube_page(engine->page, "return crimson.addToPlaylist(id, video)", create_page_arguments("id",
		g_variant_new_string(playlist_id), "video", g_variant_new_string(video_id), NULL));
}


void remove_from_playlist(Engine *engine, const char *playlist_id, const char *video_id, const char *set_id) {
	call_youtube_page(engine->page, "return crimson.removeFromPlaylist(id, video, set)", create_page_arguments("id",
		g_variant_new_string(playlist_id), "video", g_variant_new_string(video_id), "set", g_variant_new_string(set_id),
		NULL));
}


void move_in_playlist(Engine *engine, const char *playlist_id, const char *set_id, const char *successor_id) {
	call_youtube_page(engine->page, "return crimson.moveInPlaylist(id, set, successor)", create_page_arguments("id",
		g_variant_new_string(playlist_id), "set", g_variant_new_string(set_id), "successor",
		g_variant_new_string(successor_id), NULL));
}


void rename_playlist(Engine *engine, const char *playlist_id, const char *title) {
	call_youtube_page(engine->page, "return crimson.renamePlaylist(id, title)", create_page_arguments("id",
		g_variant_new_string(playlist_id), "title", g_variant_new_string(title), NULL));
}


void delete_playlist(Engine *engine, const char *playlist_id) {
	call_youtube_page(engine->page, "return crimson.deletePlaylist(id)", create_page_arguments("id", g_variant_new_string(playlist_id),
		NULL));
}


void request_lyrics(Engine *engine, const char *video_id) {
	call_youtube_page(engine->page, "return crimson.loadLyrics(video)", create_page_arguments("video", g_variant_new_string(video_id), NULL));
}


void find_album(Engine *engine, const char *video_id) {
	call_youtube_page(engine->page, "return crimson.findAlbum(video)", create_page_arguments("video", g_variant_new_string(video_id), NULL));
}


void like_song(Engine *engine, const char *video_id, gboolean is_liked) {
	call_youtube_page(engine->page, "return crimson.likeSong(video, liked)", create_page_arguments("video", g_variant_new_string(video_id),
		"liked", g_variant_new_int32(is_liked), NULL));
}


void dislike_song(Engine *engine, const char *video_id, gboolean is_disliked) {
	call_youtube_page(engine->page, "return crimson.dislikeSong(video, disliked)",
		create_page_arguments("video", g_variant_new_string(video_id), "disliked", g_variant_new_int32(is_disliked), NULL));
}

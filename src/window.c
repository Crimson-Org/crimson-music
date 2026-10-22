#include <string.h>

#include "../lang/en.h"
#include "artwork.h"
#include "discover.h"
#include "common/key-store.h"
#include "common/loading.h"
#include "common/log.h"
#include "common/sidebar-width.h"
#include "common/smooth-scroll.h"
#include "common/startup.h"
#include "dynamic.h"
#include "last-song.h"
#include "lastfm.h"
#include "local-library.h"
#include "motion.h"
#include "settings-dialog.h"
#include "settings.h"
#include "sign-in.h"
#include "song-info.h"
#include "song-list.h"
#include "song-page.h"
#include "window-private.h"

#define SCROLL_FILE "scroll.ini"
#define SCROLL_GROUP "playlists"

#define NARROW_CONDITION "max-width: 640sp"
#define REVEAL_MILLISECONDS 400
#define SEARCH_DELAY_MILLISECONDS 300
#define LASTFM_AVATAR_SIZE 26
#define LASTFM_PANEL_CHARACTERS 32
#define LASTFM_RECENT_PATH "/library"
#define LASTFM_LOVED_PATH "/loved"
#define LASTFM_ARTISTS_PATH "/library/artists"
#define SEARCH_WIDTH 420


CrimsonPlaylist *find_playlist(Window *window, const char *playlist_id) {
	if (g_strcmp0(playlist_id, LOCAL_PLAYLIST) == 0)
		return window->local_playlist;
	if (playlist_id && g_str_has_prefix(playlist_id, DYNAMIC_PREFIX))
		return find_dynamic_playlist(window, playlist_id);
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(window->playlists)); index++) {
		g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(G_LIST_MODEL(window->playlists), index);
		if (g_strcmp0(playlist->id, playlist_id) == 0)
			return playlist;
	}
	return NULL;
}


void show_message(Window *window, const char *text) {
	adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(window->toasts), adw_toast_new(text));
}


/* The taskbar names the playing song, or the app while nothing plays */
void update_top_title(Window *window) {
	CrimsonSong *song = window->current_video ? get_queue_song(window->queue) : NULL;
	g_autofree char *title = !song ? g_strdup(TEXT_APPLICATION_NAME) : *song->artist
		? g_strdup_printf("%s · %s", song->title, song->artist) : g_strdup(song->title);
	gtk_window_set_title(GTK_WINDOW(window->window), title);
}


void update_playlist_page(Window *window) {
	CrimsonPlaylist *playlist = window->shown;
	gboolean is_signed_out = window->is_ready && !window->is_signed_in;
	if (is_signed_out)
		set_song_page_empty(window->playlist_page, "system-search-symbolic", TEXT_SEARCH_TITLE, TEXT_SEARCH_DETAIL);
	else
		set_song_page_empty(window->playlist_page, "view-list-symbolic", TEXT_EMPTY_PLAYLIST, NULL);
	if (!playlist) {
		update_song_page(window->playlist_page, window->has_library || is_signed_out);
		return;
	}
	set_song_page_songs(window->playlist_page, G_LIST_MODEL(playlist->songs), playlist->is_editable, FALSE);
	update_song_page(window->playlist_page, playlist->has_songs);
	show_suggestions(window);
}


void refresh_playlist(Window *window, CrimsonPlaylist *playlist) {
	if (!playlist || playlist->incoming || !window->is_signed_in || g_str_has_prefix(playlist->id, DYNAMIC_PREFIX)
		|| playlist == window->local_playlist || is_local_playlist(playlist))
		return;
	playlist->incoming = g_ptr_array_new_with_free_func(g_object_unref);
	request_playlist(window->engine, playlist->id);
}


/* Where each playlist was scrolled to, kept between launches ================ */


static char *get_scroll_path(void) {
	return get_state_file_path(SCROLL_FILE);
}


static void load_scroll_places(Window *window) {
	g_autofree char *path = get_scroll_path();
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	window->scroll_places = file ? g_key_file_ref(file) : g_key_file_new();
}


/* The playlist on screen keeps the place it was scrolled to, written down when it is left and when the app closes */
static void remember_scroll(Window *window) {
	if (!window->shown || !window->scroll_places)
		return;
	g_key_file_set_double(window->scroll_places, SCROLL_GROUP, window->shown->id,
		get_song_page_scroll(window->playlist_page));
	g_autofree char *path = get_scroll_path();
	save_key_file(window->scroll_places, path);
}


static double recall_scroll(Window *window, CrimsonPlaylist *playlist) {
	return g_key_file_get_double(window->scroll_places, SCROLL_GROUP, playlist->id, NULL);
}


/* Only choosing another playlist slides it in, so a refresh of the one on screen never moves it */
void show_playlist(Window *window, CrimsonPlaylist *playlist) {
	gboolean is_another = playlist != window->shown;
	if (is_another)
		remember_scroll(window);
	g_set_object(&window->shown, playlist);
	g_set_str(&window->state.playlist_id, playlist ? playlist->id : NULL);
	if (playlist && !playlist->has_songs)
		load_cached_songs(playlist);
	refresh_playlist(window, playlist);
	if (playlist && is_another) {
		set_song_page_heading(window->playlist_page, playlist->title, playlist->subtitle, playlist->artwork, FALSE);
		set_song_page_quiet_likes(window->playlist_page, g_str_equal(playlist->id, LIKED_PLAYLIST)
			|| g_str_has_prefix(playlist->id, DYNAMIC_PREFIX));
		set_song_page_songs(window->playlist_page, G_LIST_MODEL(playlist->songs), playlist->is_editable, TRUE);
	}
	update_playlist_page(window);
	if (playlist && is_another)
		set_song_page_scroll(window->playlist_page, recall_scroll(window, playlist));
}


/* Selects the row of the playlist, or the first row when no playlist is named, and says whether the list had it */
static gboolean select_row_in(GtkWidget *list, GListModel *playlists, const char *playlist_id) {
	for (guint index = 0; index < g_list_model_get_n_items(playlists); index++) {
		g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(playlists, index);
		if (playlist_id && g_strcmp0(playlist->id, playlist_id) != 0)
			continue;
		gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), (int)index));
		return TRUE;
	}
	return FALSE;
}


/* Home keeps its row lit while its page is open, so playlists arriving or changing then light none of theirs, and
 * the playlist's row lights again once the playlist page itself is back in view */
void select_playlist_row(Window *window, const char *playlist_id) {
	if (window->home_list && gtk_list_box_get_selected_row(GTK_LIST_BOX(window->home_list)))
		return;
	if (g_strcmp0(playlist_id, LOCAL_PLAYLIST) == 0) {
		select_local_row(window);
		return;
	}
	if (playlist_id && g_str_has_prefix(playlist_id, DYNAMIC_PREFIX) && find_dynamic_playlist(window, playlist_id)) {
		select_dynamic_row(window, playlist_id);
		return;
	}
	if (playlist_id && g_str_has_prefix(playlist_id, DYNAMIC_PREFIX))
		playlist_id = NULL;
	if (playlist_id && select_row_in(window->local_list, window->local_section, playlist_id))
		return;
	select_row_in(window->playlist_list, window->youtube_playlists, playlist_id);
}


/* Signed out, the sidebar keeps its pins under a card asking to sign in */
static void update_sidebar(Window *window) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->playlists));
	gboolean is_signed_out = window->is_ready && !window->is_signed_in;
	gtk_widget_set_visible(window->sign_in_card, is_signed_out);
	gboolean has_youtube = FALSE;
	for (guint index = 0; index < count && !has_youtube; index++) {
		g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(G_LIST_MODEL(window->playlists), index);
		has_youtube = !is_local_playlist(playlist);
	}
	window->has_library = window->has_library || has_youtube;
	/* Playlists kept from last time show Home with them, before YouTube Music has said the listener is signed in */
	gtk_widget_set_visible(window->home_list, window->is_signed_in || (has_youtube && !is_signed_out));
	show_youtube_gap(window);
	gtk_widget_set_visible(window->playlists_spinner, !has_youtube && !window->has_library && !is_signed_out);
	gtk_widget_set_visible(window->playlists_empty, !has_youtube && window->has_library && !is_signed_out);
}


gboolean require_account(Window *window, const char *feature) {
	if (window->is_signed_in)
		return TRUE;
	g_autofree char *text = g_strdup_printf(TEXT_NEEDS_ACCOUNT, feature);
	AdwToast *toast = adw_toast_new(text);
	adw_toast_set_button_label(toast, TEXT_SIGN_IN);
	adw_toast_set_action_name(toast, "win.sign-in");
	adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(window->toasts), toast);
	return FALSE;
}


/* Local media ============================================================== */


/* Playlists made here sit among the YouTube ones, first until the listener moves them, so a refresh of those keeps
 * them in place */
static void add_local_playlists(Window *window, GPtrArray *playlists) {
	guint place = 0;
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(window->playlists)); index++) {
		CrimsonPlaylist *playlist = g_list_model_get_item(G_LIST_MODEL(window->playlists), index);
		if (is_local_playlist(playlist))
			g_ptr_array_insert(playlists, (gint)place++, playlist);
		else
			g_object_unref(playlist);
	}
}


/* A local playlist shows how many songs it has and the cover of its first song with one */
void describe_local_playlist(CrimsonPlaylist *playlist) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(playlist->songs));
	g_free(playlist->subtitle);
	playlist->subtitle = g_strdup_printf(count == 1 ? TEXT_SONG_COUNT_ONE : TEXT_SONG_COUNT, count);
	const char *artwork = "";
	for (guint index = 0; index < count && !*artwork; index++) {
		g_autoptr(CrimsonSong) song = g_list_model_get_item(G_LIST_MODEL(playlist->songs), index);
		artwork = *song->artwork ? g_intern_string(song->artwork) : "";
	}
	if (g_strcmp0(artwork, playlist->artwork) == 0)
		return;
	g_set_str(&playlist->artwork, artwork);
	GtkWidget *cover = g_object_get_data(G_OBJECT(playlist), "sidebar-cover");
	if (cover)
		show_artwork(cover, artwork);
}


/* Saves a playlist after an edit, to the listener's own file for a local one and to the cache for the others */
void save_playlist(Window *window, CrimsonPlaylist *playlist) {
	if (!is_local_playlist(playlist)) {
		save_cached_songs(playlist);
		return;
	}
	describe_local_playlist(playlist);
	save_local_playlists(window->playlists);
	if (playlist == window->shown)
		set_song_page_heading(window->playlist_page, playlist->title, playlist->subtitle, playlist->artwork, FALSE);
}


static void load_local_playlists_into(Window *window) {
	g_autoptr(GPtrArray) playlists = load_local_playlists();
	for (guint index = 0; index < playlists->len; index++)
		describe_local_playlist(g_ptr_array_index(playlists, index));
	g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func(g_object_unref);
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(window->playlists)); index++) {
		CrimsonPlaylist *playlist = g_list_model_get_item(G_LIST_MODEL(window->playlists), index);
		if (!is_local_playlist(playlist))
			g_ptr_array_add(all, playlist);
		else
			g_object_unref(playlist);
	}
	for (guint index = 0; index < playlists->len; index++)
		g_ptr_array_insert(all, (gint)index, g_object_ref(g_ptr_array_index(playlists, index)));
	order_playlists(all, (const char *const *)window->state.playlist_order);
	replace_playlists(window->playlists, all);
}


static CrimsonPlaylist *create_media_library_playlist(void) {
	CrimsonPlaylist *playlist = g_object_new(CRIMSON_TYPE_PLAYLIST, NULL);
	playlist->id = g_strdup(LOCAL_PLAYLIST);
	playlist->title = g_strdup(TEXT_LOCAL_LIBRARY);
	playlist->subtitle = g_strdup("");
	playlist->artwork = g_strdup(LOCAL_LIBRARY_ARTWORK);
	return playlist;
}


/* The library's count follows its songs, under an emblem of its own */
static void notice_local_library(gpointer data) {
	Window *window = data;
	CrimsonPlaylist *playlist = window->local_playlist;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(playlist->songs));
	g_free(playlist->subtitle);
	playlist->subtitle = g_strdup_printf(count == 1 ? TEXT_SONG_COUNT_ONE : TEXT_SONG_COUNT, count);
	if (playlist == window->shown) {
		set_song_page_heading(window->playlist_page, playlist->title, playlist->subtitle, playlist->artwork, FALSE);
		update_playlist_page(window);
	}
	update_dynamic_playlists(window);
}


/* Answers from the engine ================================================= */


/* Puts the player bar away and forgets the song and queue, so none of it comes back on the next launch */
static void forget_playing_song(Window *window) {
	pause_video(window->engine);
	stop_local_song(window->local_player);
	window->is_restored = FALSE;
	g_clear_pointer(&window->current_video, g_free);
	set_playing_video(NULL);
	gtk_revealer_set_reveal_child(GTK_REVEALER(window->player_revealer), FALSE);
	clear_queue(window->queue);
	save_last_song(NULL, NULL, 0, NULL, NULL);
	update_top_title(window);
}


static void notice_ready(gboolean is_signed_in, gpointer data) {
	Window *window = data;
	window->is_signed_in = is_signed_in;
	window->is_ready = TRUE;
	allow_song_info();
	refresh_lastfm_loves();
	if (window->current_video)
		notice_song_info(window->current_video, window);
	g_simple_action_set_enabled(window->sign_out_action, is_signed_in);
	g_simple_action_set_enabled(window->sign_in_action, !is_signed_in);
	update_sidebar(window);
	update_playlist_page(window);
	if (!is_signed_in)
		return;
	request_library(window->engine);
	refresh_playlist(window, window->shown);
	refresh_playlist(window, find_playlist(window, LIKED_PLAYLIST));
	update_sidebar(window);
}


static void receive_playlists(JSCValue *array, gpointer data) {
	Window *window = data;
	note_startup_moment("Playlists arrived");
	window->has_library = TRUE;
	g_autoptr(GPtrArray) playlists = read_playlists(array);
	add_local_playlists(window, playlists);
	order_playlists(playlists, (const char *const *)window->state.playlist_order);
	g_autofree char *shown_id = g_strdup(window->state.playlist_id);
	if (replace_playlists(window->playlists, playlists))
		save_cached_playlists(window->playlists);
	if (window->new_playlist_id && find_playlist(window, window->new_playlist_id)) {
		g_set_str(&shown_id, window->new_playlist_id);
		g_clear_pointer(&window->new_playlist_id, g_free);
		gtk_list_box_unselect_all(GTK_LIST_BOX(window->playlist_list));
	}
	if (!gtk_list_box_get_selected_row(GTK_LIST_BOX(window->playlist_list)))
		select_playlist_row(window, find_playlist(window, shown_id) ? shown_id : NULL);
	update_sidebar(window);
	update_playlist_page(window);
}


static void finish_songs(Window *window, CrimsonPlaylist *playlist, GPtrArray *songs) {
	apply_playlist_edits(playlist, songs, TRUE);
	if (replace_songs(playlist, songs))
		save_cached_songs(playlist);
	if (g_strcmp0(window->waiting_play_id, playlist->id) == 0)
		play_playlist_now(window, playlist, window->is_waiting_play_shuffled);
	if (g_str_equal(playlist->id, LIKED_PLAYLIST))
		update_liked(window);
	if (playlist == window->shown)
		update_playlist_page(window);
}


/* A playlist seen before keeps its old songs until the last page, while a new one fills in page by page */
static void receive_songs(const char *playlist_id, JSCValue *array, gboolean is_editable, gboolean is_first,
	gboolean is_done, gpointer data) {
	Window *window = data;
	CrimsonPlaylist *playlist = find_playlist(window, playlist_id);
	if (!playlist || !playlist->incoming)
		return;
	if (playlist->is_editable != is_editable) {
		playlist->is_editable = is_editable;
		save_cached_songs(playlist);
	}
	g_autoptr(GPtrArray) songs = read_songs(array);
	g_ptr_array_extend_and_steal(playlist->incoming, g_steal_pointer(&songs));
	if (!playlist->has_songs && playlist == window->shown) {
		GPtrArray *page = playlist->incoming;
		guint shown = g_list_model_get_n_items(G_LIST_MODEL(playlist->songs));
		g_list_store_splice(playlist->songs, shown, 0, page->pdata + shown, page->len - shown);
		update_playlist_page(window);
	}
	if (!is_done)
		return;
	g_autoptr(GPtrArray) complete = g_steal_pointer(&playlist->incoming);
	finish_songs(window, playlist, complete);
}


static void notice_failure(const char *message, const char *playlist_id, gpointer data) {
	Window *window = data;
	CrimsonPlaylist *playlist = playlist_id ? find_playlist(window, playlist_id) : NULL;
	if (playlist && playlist->incoming) {
		g_clear_pointer(&playlist->incoming, g_ptr_array_unref);
		playlist->has_songs = TRUE;
		if (playlist == window->shown)
			update_playlist_page(window);
	}
	stop_waiting(window);
	g_autofree char *text = g_strdup_printf(TEXT_LOADING_FAILED, message);
	show_message(window, text);
}


static void press_media_key(const char *action, gpointer data) {
	Window *window = data;
	if (g_strcmp0(action, "next") == 0 || g_strcmp0(action, "previous") == 0)
		g_action_group_activate_action(G_ACTION_GROUP(window->window), action, NULL);
	else if ((g_strcmp0(action, "play") == 0 && !window->is_playing)
		|| (g_strcmp0(action, "pause") == 0 && window->is_playing))
		g_action_group_activate_action(G_ACTION_GROUP(window->window), "play-pause", NULL);
}


static const EngineEvents ENGINE_EVENTS = {
	notice_ready, receive_playlists, receive_songs, receive_answer, receive_player, notice_failure, press_media_key,
};


/* Sign in and out ========================================================== */


static void finish_sign_in(gpointer data) {
	Window *window = data;
	window->is_ready = FALSE;
	reload_engine(window->engine);
}


static void start_sign_in(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	show_sign_in(GTK_WIDGET(window->window), get_engine_session(window->engine), finish_sign_in, window);
}


static void sign_out(Window *window) {
	forget_playing_song(window);
	adw_navigation_view_pop_to_tag(ADW_NAVIGATION_VIEW(window->navigation), PLAYLIST_TAG);
	window->is_signed_in = FALSE;
	window->has_library = FALSE;
	g_clear_pointer(&window->state.account_photo, g_free);
	g_simple_action_set_enabled(window->sign_out_action, FALSE);
	window->is_ready = FALSE;
	show_playlist(window, NULL);
	g_list_store_remove_all(window->playlists);
	load_local_playlists_into(window);
	clear_cache();
	g_clear_pointer(&window->home, free_home);
	forget_discover_mixes();
	update_sidebar(window);
	sign_out_engine(window->engine);
}


static void finish_sign_out_question(GObject *dialog, GAsyncResult *result, gpointer data) {
	if (g_str_equal(adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(dialog), result), "confirm"))
		sign_out(data);
}


/* Signing out is asked about first, since a click by mistake would otherwise take the library away at once */
static void ask_sign_out(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	AdwDialog *dialog = adw_alert_dialog_new(TEXT_SIGN_OUT_QUESTION, TEXT_SIGN_OUT_DETAIL);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "confirm", TEXT_SIGN_OUT, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "confirm", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(window->window), NULL, finish_sign_out_question,
		window);
}


/* Building the window ===================================================== */


GtkWidget *create_status(const char *icon, const char *title, const char *detail) {
	GtkWidget *status = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), icon);
	adw_status_page_set_title(ADW_STATUS_PAGE(status), title);
	adw_status_page_set_description(ADW_STATUS_PAGE(status), detail);
	return status;
}


GtkWidget *create_scrolled(GtkWidget *child) {
	GtkWidget *scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), child);
	add_smooth_scrolling(GTK_SCROLLED_WINDOW(scrolled));
	return scrolled;
}


GtkWidget *create_stack(void) {
	GtkWidget *stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	return stack;
}


/* Last.fm ================================================================= */


static void click_lastfm_connect(GtkButton *button, Window *window) {
	gtk_menu_button_popdown(GTK_MENU_BUTTON(window->lastfm_reminder));
	connect_lastfm(GTK_WIDGET(window->lastfm_reminder));
	show_message(window, TEXT_LASTFM_APPROVE);
}


static void click_lastfm_settings(GtkButton *button, Window *window) {
	gtk_menu_button_popdown(GTK_MENU_BUTTON(window->lastfm_reminder));
	gtk_widget_activate_action(GTK_WIDGET(window->window), "app.lastfm-settings", NULL);
}


/* A small panel that says Last.fm is not connected and what connecting gives, with the way to connect */
static GtkWidget *create_lastfm_panel(Window *window) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_add_css_class(box, "lastfm-panel");
	GtkWidget *title = gtk_label_new(TEXT_LASTFM_NOT_CONNECTED);
	gtk_widget_add_css_class(title, "heading");
	GtkWidget *detail = gtk_label_new(TEXT_LASTFM_CONNECT_DETAIL);
	gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(detail), LASTFM_PANEL_CHARACTERS);
	gtk_widget_add_css_class(detail, "dimmed");
	for (GtkWidget *label = title; label; label = label == title ? detail : NULL) {
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_box_append(GTK_BOX(box), label);
	}
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_top(buttons, 6);
	GtkWidget *connect = gtk_button_new_with_label(TEXT_CONNECT);
	gtk_widget_add_css_class(connect, "suggested-action");
	g_signal_connect(connect, "clicked", G_CALLBACK(click_lastfm_connect), window);
	GtkWidget *settings = gtk_button_new_with_label(TEXT_SETTINGS);
	g_signal_connect(settings, "clicked", G_CALLBACK(click_lastfm_settings), window);
	gtk_box_append(GTK_BOX(buttons), connect);
	gtk_box_append(GTK_BOX(buttons), settings);
	gtk_box_append(GTK_BOX(box), buttons);
	GtkWidget *popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popover), box);
	return popover;
}


static GtkWidget *create_lastfm_reminder(Window *window) {
	window->lastfm_reminder = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(window->lastfm_reminder), "crimson-lastfm-off-symbolic");
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(window->lastfm_reminder), create_lastfm_panel(window));
	gtk_widget_add_css_class(window->lastfm_reminder, "lastfm-reminder");
	gtk_widget_set_valign(window->lastfm_reminder, GTK_ALIGN_CENTER);
	return window->lastfm_reminder;
}


static void append_address(GMenu *menu, const char *label, const char *address) {
	g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
	g_menu_item_set_action_and_target_value(item, "win.open-address", g_variant_new_string(address));
	g_menu_append_item(menu, item);
}


/* Last.fm's own pages for the listener, the playing song on Last.fm, and the connection itself */
static void fill_lastfm_menu(GtkMenuButton *button, Window *window) {
	GMenu *menu = g_menu_new();
	const char *profile = get_lastfm_profile();
	g_autoptr(GMenu) pages = g_menu_new();
	if (profile) {
		append_address(pages, TEXT_LASTFM_PROFILE, profile);
		g_autofree char *recent = g_strconcat(profile, LASTFM_RECENT_PATH, NULL);
		g_autofree char *loved = g_strconcat(profile, LASTFM_LOVED_PATH, NULL);
		g_autofree char *artists = g_strconcat(profile, LASTFM_ARTISTS_PATH, NULL);
		append_address(pages, TEXT_LASTFM_RECENT, recent);
		append_address(pages, TEXT_LASTFM_LOVED, loved);
		append_address(pages, TEXT_LASTFM_TOP_ARTISTS, artists);
	}
	g_menu_append_section(menu, NULL, G_MENU_MODEL(pages));
	CrimsonSong *song = get_queue_song(window->queue);
	if (song) {
		g_autoptr(GMenu) playing = g_menu_new();
		g_autofree char *artist = get_song_lastfm_artist(song);
		g_autofree char *address = make_lastfm_address(artist, NULL, song->title);
		append_address(playing, TEXT_LASTFM_PLAYING, address);
		g_menu_append_section(menu, NULL, G_MENU_MODEL(playing));
	}
	g_autoptr(GMenu) connection = g_menu_new();
	g_menu_append(connection, TEXT_SETTINGS, "app.lastfm-settings");
	g_menu_append(connection, TEXT_DISCONNECT, "win.disconnect-lastfm");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(connection));
	gtk_menu_button_set_menu_model(button, G_MENU_MODEL(menu));
	g_object_unref(menu);
}


/* GTK only asks for the menu when it has none, so it is dropped on closing and made fresh for every opening */
static gboolean drop_lastfm_menu(gpointer data) {
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(data), NULL);
	g_object_unref(data);
	return G_SOURCE_REMOVE;
}


static void notice_lastfm_menu(GtkMenuButton *button, GParamSpec *spec, gpointer data) {
	if (!gtk_menu_button_get_active(button))
		g_idle_add(drop_lastfm_menu, g_object_ref(button));
}


static GtkWidget *create_lastfm_button(Window *window) {
	window->lastfm_avatar = adw_avatar_new(LASTFM_AVATAR_SIZE, NULL, TRUE);
	window->lastfm_button = gtk_menu_button_new();
	gtk_menu_button_set_child(GTK_MENU_BUTTON(window->lastfm_button), window->lastfm_avatar);
	gtk_widget_add_css_class(window->lastfm_button, "flat");
	gtk_widget_add_css_class(window->lastfm_button, "circular");
	gtk_widget_add_css_class(window->lastfm_button, "lastfm-avatar");
	gtk_widget_set_valign(window->lastfm_button, GTK_ALIGN_CENTER);
	gtk_widget_set_visible(window->lastfm_button, FALSE);
	gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(window->lastfm_button),
		(GtkMenuButtonCreatePopupFunc)fill_lastfm_menu, window, NULL);
	g_signal_connect(window->lastfm_button, "notify::active", G_CALLBACK(notice_lastfm_menu), NULL);
	return window->lastfm_button;
}


/* The reminder stands out in the top bar for as long as Last.fm is not connected, unless the listener hid it, and
 * the listener's picture takes its place on the other side once it is */
static void show_lastfm_reminder(Window *window) {
	const char *user = get_lastfm_user();
	gtk_widget_set_visible(window->lastfm_button, user != NULL);
	if (user) {
		adw_avatar_set_text(ADW_AVATAR(window->lastfm_avatar), user);
		adw_avatar_set_custom_image(ADW_AVATAR(window->lastfm_avatar), GDK_PAINTABLE(get_lastfm_picture()));
		g_autofree char *tooltip = g_strdup_printf(TEXT_LASTFM_CONNECTED, user);
		gtk_widget_set_tooltip_text(window->lastfm_button, tooltip);
	}
	gboolean is_shown = is_lastfm_available() && !get_lastfm_user() && get_settings()->shows_lastfm_reminder;
	gtk_widget_set_visible(window->lastfm_reminder, is_shown);
	gtk_widget_set_tooltip_text(window->lastfm_reminder, is_lastfm_connecting() ? TEXT_LASTFM_WAITING
		: TEXT_LASTFM_REMINDER);
	if (is_lastfm_connecting())
		gtk_widget_add_css_class(window->lastfm_reminder, "waiting");
	else
		gtk_widget_remove_css_class(window->lastfm_reminder, "waiting");
}


static void show_visuals(Window *window) {
	PlayerVisuals visuals = get_settings()->visuals;
	set_player_colored(window->bar, visuals != VISUALS_PLAIN);
}


void apply_player_visuals(void) {
	GList *windows = gtk_application_get_windows(GTK_APPLICATION(g_application_get_default()));
	for (GList *link = windows; link; link = link->next) {
		Window *window = g_object_get_data(link->data, "window");
		if (window)
			show_visuals(window);
	}
}


void apply_playlist_suggestions(void) {
	GList *windows = gtk_application_get_windows(GTK_APPLICATION(g_application_get_default()));
	for (GList *link = windows; link; link = link->next) {
		Window *window = g_object_get_data(link->data, "window");
		if (window)
			show_suggestions(window);
	}
}


void update_lastfm_state(void) {
	update_lastfm_settings();
	set_song_love_check(is_song_loved, get_lastfm_user() != NULL);
	GList *windows = gtk_application_get_windows(GTK_APPLICATION(g_application_get_default()));
	for (GList *link = windows; link; link = link->next) {
		Window *window = g_object_get_data(link->data, "window");
		if (window) {
			show_lastfm_reminder(window);
			show_playing_love(window);
		}
	}
}


static GtkWidget *create_menu_button(void) {
	g_autoptr(GMenu) menu = g_menu_new();
	g_menu_append(menu, TEXT_NEW_PLAYLIST, "win.new-playlist");
	g_menu_append(menu, TEXT_NEW_LOCAL_PLAYLIST, "win.new-local-playlist");
	g_menu_append(menu, TEXT_NEW_DYNAMIC, "win.new-dynamic");
	g_menu_append(menu, TEXT_SETTINGS, "app.settings");
	/* Only the one of Sign In and Sign Out that applies shows */
	const char *const ACCOUNT_ITEMS[][2] = {{TEXT_SIGN_IN, "win.sign-in"}, {TEXT_SIGN_OUT, "win.sign-out"}};
	for (guint index = 0; index < G_N_ELEMENTS(ACCOUNT_ITEMS); index++) {
		g_autoptr(GMenuItem) item = g_menu_item_new(ACCOUNT_ITEMS[index][0], ACCOUNT_ITEMS[index][1]);
		g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
		g_menu_append_item(menu, item);
	}
	g_menu_append(menu, TEXT_ABOUT, "app.about");
	g_menu_append(menu, TEXT_QUIT, "app.quit");
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
	gtk_menu_button_set_primary(GTK_MENU_BUTTON(button), TRUE);
	gtk_widget_set_tooltip_text(button, TEXT_MAIN_MENU);
	return button;
}


/* Signed out, one row takes the place of the YouTube playlists, no taller than their spinner, so nothing below moves;
 * its tooltip says what signing in adds */
GtkWidget *create_sign_in_card(void) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_add_css_class(row, "sign-in-row");
	gtk_widget_set_tooltip_text(row, TEXT_SIGN_IN_DETAIL);
	GtkWidget *icon = gtk_image_new_from_icon_name("avatar-default-symbolic");
	gtk_widget_add_css_class(icon, "dimmed");
	gtk_box_append(GTK_BOX(row), icon);
	GtkWidget *label = gtk_label_new(TEXT_SIGN_IN_TITLE);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_widget_add_css_class(label, "dimmed");
	gtk_box_append(GTK_BOX(row), label);
	GtkWidget *button = gtk_button_new_with_label(TEXT_SIGN_IN);
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(button, "pill");
	gtk_widget_add_css_class(button, "small");
	gtk_widget_add_css_class(button, "suggested-action");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.sign-in");
	gtk_box_append(GTK_BOX(row), button);
	return row;
}


static void hide_to_tray(GtkButton *button, Window *window) {
	if (is_tray_shown(window->tray))
		gtk_widget_set_visible(GTK_WIDGET(window->window), FALSE);
	else
		gtk_window_minimize(GTK_WINDOW(window->window));
}


/* Takes the place of the system minimize button, since Wayland never tells a window it was minimized, and
 * sends the window to the tray instead of the taskbar */
static GtkWidget *create_minimize_button(GtkWidget *header) {
	g_autofree char *layout = NULL;
	g_object_get(gtk_settings_get_default(), "gtk-decoration-layout", &layout, NULL);
	g_auto(GStrv) sides = g_strsplit(layout ? layout : ":close", ":", 2);
	GString *rebuilt = g_string_new(NULL);
	for (int side = 0; sides[side]; side++) {
		g_auto(GStrv) buttons = g_strsplit(sides[side], ",", -1);
		g_string_append(rebuilt, side ? ":" : "");
		for (int index = 0, written = 0; buttons[index]; index++) {
			if (g_str_equal(buttons[index], "minimize") || !*buttons[index])
				continue;
			g_string_append_printf(rebuilt, "%s%s", written++ ? "," : "", buttons[index]);
		}
	}
	g_autofree char *without_minimize = g_string_free(rebuilt, FALSE);
	adw_header_bar_set_decoration_layout(ADW_HEADER_BAR(header), without_minimize);
	GtkWidget *button = gtk_button_new_from_icon_name("window-minimize-symbolic");
	gtk_widget_set_tooltip_text(button, TEXT_MINIMIZE);
	gtk_widget_set_visible(button, layout && strstr(layout, "minimize"));
	return button;
}


static void go_back(GtkButton *button, Window *window) {
	gtk_widget_activate_action(GTK_WIDGET(window->window), "win.back", NULL);
}


/* One near black bar runs across the whole window with search in its middle, and the sidebar button only when
 * the sidebar folds away */
static GtkWidget *create_top_bar(Window *window) {
	GtkWidget *header = adw_header_bar_new();
	gtk_widget_add_css_class(header, "top-bar");
	GtkWidget *sidebar_button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(sidebar_button), "sidebar-show-symbolic");
	gtk_widget_set_tooltip_text(sidebar_button, TEXT_PLAYLISTS);
	g_object_bind_property(window->split, "show-sidebar", sidebar_button, "active",
		G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
	g_object_bind_property(window->split, "collapsed", sidebar_button, "visible", G_BINDING_SYNC_CREATE);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), sidebar_button);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), create_lastfm_button(window));
	/* The reminder takes the avatar's place while Last.fm is not connected */
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), create_lastfm_reminder(window));
	window->back_button = gtk_button_new_from_icon_name("go-previous-symbolic");
	gtk_widget_set_tooltip_text(window->back_button, TEXT_BACK);
	g_signal_connect(window->back_button, "clicked", G_CALLBACK(go_back), window);
	gtk_widget_set_visible(window->back_button, FALSE);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), window->back_button);
	window->top_spinner = adw_spinner_new();
	gtk_widget_add_css_class(window->top_spinner, "top-spinner");
	gtk_widget_set_visible(window->top_spinner, FALSE);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), window->top_spinner);
	window->search_entry = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(window->search_entry), TEXT_SEARCH);
	gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(window->search_entry), SEARCH_DELAY_MILLISECONDS);
	gtk_widget_add_css_class(window->search_entry, "top-search");
	g_signal_connect(window->search_entry, "search-changed", G_CALLBACK(start_search), window);
	g_signal_connect(window->search_entry, "activate", G_CALLBACK(start_search), window);
	GtkWidget *clamp = adw_clamp_new();
	adw_clamp_set_maximum_size(ADW_CLAMP(clamp), SEARCH_WIDTH);
	adw_clamp_set_child(ADW_CLAMP(clamp), window->search_entry);
	gtk_widget_set_hexpand(clamp, TRUE);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), clamp);
	GtkWidget *minimize = create_minimize_button(header);
	gtk_widget_add_css_class(minimize, "minimize-button");
	gtk_widget_set_valign(minimize, GTK_ALIGN_CENTER);
	g_signal_connect(minimize, "clicked", G_CALLBACK(hide_to_tray), window);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), minimize);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), create_menu_button());
	return header;
}


static void keep_sidebar_width(int width, gpointer data) {
	Window *window = data;
	window->state.sidebar_width = width;
}


static GtkWidget *create_library(Window *window) {
	window->split = adw_overlay_split_view_new();
	adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(window->split), create_sidebar(window));
	adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(window->split), create_navigation(window));
	return add_sidebar_handle(ADW_OVERLAY_SPLIT_VIEW(window->split), window->state.sidebar_width,
		SIDEBAR_MINIMUM_WIDTH, SIDEBAR_MAXIMUM_WIDTH, keep_sidebar_width, window);
}


static GtkWidget *create_frame(Window *window) {
	window->main_stack = create_stack();
	gtk_stack_add_named(GTK_STACK(window->main_stack), create_library(window), "library");
	static const PlayerBarEvents BAR_EVENTS = {seek, change_volume, open_queue_source, open_playing_artist,
		open_playing_menu, toggle_playing_like, toggle_playing_dislike, toggle_playing_love, queue_dropped_song};
	window->bar = create_player_bar(&BAR_EVENTS, window, window->state.volume);
	window->player_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(window->player_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
	gtk_revealer_set_transition_duration(GTK_REVEALER(window->player_revealer), REVEAL_MILLISECONDS);
	gtk_revealer_set_child(GTK_REVEALER(window->player_revealer), get_player_bar_widget(window->bar));
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), create_top_bar(window));
	/* The player sits under the content rather than in a bottom bar, since bars drag the whole window and that would
	 * take over dragging the playing song to a playlist */
	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_vexpand(window->main_stack, TRUE);
	gtk_box_append(GTK_BOX(content), window->main_stack);
	gtk_widget_add_css_class(window->player_revealer, "player-edge");
	gtk_box_append(GTK_BOX(content), window->player_revealer);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);
	return toolbar;
}


/* Lists take arrow keys with modifiers for their own cursor, so the player's arrow shortcuts are run before the
 * focused widget sees them, except in a text field with text in it, where they select text */
static gboolean press_arrow_shortcut(GtkEventControllerKey *controller, guint keyval, guint keycode,
	GdkModifierType state, Window *window) {
	GdkModifierType modifiers = state & gtk_accelerator_get_default_mod_mask();
	GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(window->window));
	gboolean is_typing = GTK_IS_TEXT(focus) && *gtk_editable_get_text(GTK_EDITABLE(focus));
	if ((keyval != GDK_KEY_Left && keyval != GDK_KEY_Right) || !modifiers || is_typing)
		return FALSE;
	g_autofree char *accel = gtk_accelerator_name(keyval, modifiers);
	g_auto(GStrv) actions = gtk_application_get_actions_for_accel(gtk_window_get_application(GTK_WINDOW(window->window)), accel);
	if (!actions[0] || !g_str_has_prefix(actions[0], "win."))
		return FALSE;
	g_action_group_activate_action(G_ACTION_GROUP(window->window), actions[0] + strlen("win."), NULL);
	return TRUE;
}


static void step_volume(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	step_player_volume(window->bar, g_variant_get_int32(parameter));
}


static void open_address(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(g_variant_get_string(parameter, NULL));
	gtk_uri_launcher_launch(launcher, GTK_WINDOW(window->window), NULL, NULL, NULL);
}


static void disconnect_lastfm_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	disconnect_lastfm();
}


static void add_window_actions(Window *window) {
	const GActionEntry ENTRIES[] = {
		{"play-pause", toggle_playing, NULL, NULL, NULL, {0}},
		{"next", play_next, NULL, NULL, NULL, {0}},
		{"previous", play_previous, NULL, NULL, NULL, {0}},
		{"jump-back", jump_back, NULL, NULL, NULL, {0}},
		{"jump-queue", jump_queue, "i", NULL, NULL, {0}},
		{"remove-upcoming", remove_queued_song, "i", NULL, NULL, {0}},
		{"move-queued", move_queued_song, "(ii)", NULL, NULL, {0}},
		{"jump-forward", jump_forward, NULL, NULL, NULL, {0}},
		{"shuffle", NULL, NULL, window->state.is_shuffled ? "true" : "false", change_shuffle, {0}},
		{"lyrics", NULL, NULL, "false", change_lyrics, {0}},
		{"video", NULL, NULL, "false", change_video, {0}},
		{"comments", NULL, NULL, "false", change_comments, {0}},
		{"sign-out", ask_sign_out, NULL, NULL, NULL, {0}},
		{"sign-in", start_sign_in, NULL, NULL, NULL, {0}},
		{"open-address", open_address, "s", NULL, NULL, {0}},
		{"volume-step", step_volume, "i", NULL, NULL, {0}},
		{"disconnect-lastfm", disconnect_lastfm_action, NULL, NULL, NULL, {0}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(window->window), ENTRIES, G_N_ELEMENTS(ENTRIES), window);
	add_page_actions(window);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(press_arrow_shortcut), window);
	gtk_widget_add_controller(GTK_WIDGET(window->window), keys);
	window->shuffle_action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(window->window), "shuffle"));
	window->lyrics_action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(window->window), "lyrics"));
	window->video_action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(window->window), "video"));
	window->comments_action = G_SIMPLE_ACTION(
		g_action_map_lookup_action(G_ACTION_MAP(window->window), "comments"));
	window->sign_out_action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(window->window), "sign-out"));
	window->sign_in_action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(window->window), "sign-in"));
}


/* The music stops the moment the window is asked to close, before anything else it does on the way out */
static gboolean close_window(GtkWindow *gtk_window, Window *window) {
	silence_engine(window->engine);
	stop_local_song(window->local_player);
	remember_scroll(window);
	read_window_geometry(gtk_window, &window->state.geometry);
	save_player_state(&window->state);
	return FALSE;
}


/* Starts the page after the first frame, so the cached library is on screen before WebKit begins */
/* Everything that reads files or starts a library waits until the window is on screen */
static gboolean start_engine_later(gpointer data) {
	Window *window = data;
	start_engine(window->engine);
	prepare_local_player();
	start_local_library(window->local_playlist, notice_local_library, window);
	return G_SOURCE_REMOVE;
}


Window *create_window(GtkApplication *application) {
	Window *window = g_new0(Window, 1);
	load_player_state(&window->state);
	load_scroll_places(window);
	window->playlists = g_list_store_new(CRIMSON_TYPE_PLAYLIST);
	window->queue = create_queue();
	start_song_info(notice_song_info, window);
	start_lastfm((LastfmChanged)update_lastfm_state, NULL);
	window->engine = create_engine(&ENGINE_EVENTS, window);
	window->local_player = create_local_player(receive_player, window);
	window->local_playlist = create_media_library_playlist();
	window->window = ADW_APPLICATION_WINDOW(adw_application_window_new(application));
	gtk_window_set_title(GTK_WINDOW(window->window), TEXT_APPLICATION_NAME);
	apply_window_geometry(GTK_WINDOW(window->window), &window->state.geometry);
	g_object_set_data(G_OBJECT(window->window), "window", window);
	add_window_actions(window);
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), create_frame(window));
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), create_menu_anchor(window));
	window->toasts = adw_toast_overlay_new();
	adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(window->toasts), overlay);
	adw_application_window_set_content(window->window, window->toasts);
	AdwBreakpoint *breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse(NARROW_CONDITION));
	GValue collapsed = G_VALUE_INIT;
	g_value_init(&collapsed, G_TYPE_BOOLEAN);
	g_value_set_boolean(&collapsed, TRUE);
	adw_breakpoint_add_setter(breakpoint, G_OBJECT(window->split), "collapsed", &collapsed);
	adw_application_window_add_breakpoint(window->window, breakpoint);
	g_signal_connect(window->window, "close-request", G_CALLBACK(close_window), window);
	add_side_buttons(window);
	load_cached_playlists(window->playlists, (const char *const *)window->state.playlist_order);
	load_local_playlists_into(window);
	update_sidebar(window);
	select_playlist_row(window, window->state.playlist_id);
	update_playlist_page(window);
	CrimsonPlaylist *liked = find_playlist(window, LIKED_PLAYLIST);
	if (liked && !liked->has_songs)
		load_cached_songs(liked);
	update_liked(window);
	update_top_title(window);
	/* The first entry switches between Play and Pause as the song plays */
	static const TrayEntry TRAY_ENTRIES[] = {
		{TEXT_PLAY, "win.play-pause"},
		{TEXT_NEXT, "win.next"},
		{TEXT_PREVIOUS, "win.previous"},
		{NULL, NULL},
		{TEXT_QUIT_APPLICATION, "app.quit"},
	};
	TraySetup tray = {TRAY_ENTRIES, G_N_ELEMENTS(TRAY_ENTRIES), "win.play-pause", "win.volume-step"};
	window->tray = create_tray(G_APPLICATION(application), GTK_WINDOW(window->window), &tray);
	window->mpris = create_mpris(G_APPLICATION(application), GTK_WINDOW(window->window));
	note_first_frame(GTK_WIDGET(window->window));
	restore_last_song(window);
	run_after_frame(GTK_WIDGET(window->window), start_engine_later, window);
	show_lastfm_reminder(window);
	show_visuals(window);
	set_song_love_check(is_song_loved, get_lastfm_user() != NULL);
	return window;
}


GtkWindow *get_gtk_window(Window *window) {
	return GTK_WINDOW(window->window);
}

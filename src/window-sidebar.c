#include "../lang/en.h"
#include "artwork.h"
#include "common/loading.h"
#include "pins.h"
#include "window-private.h"

#define SIDEBAR_ARTWORK_SIZE 32
#define LOCAL_PLAYLIST_ICON "computer-symbolic"


/* Claims the click, so the sidebar's own menu around the rows stays shut */
static void open_row_menu(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	open_playlist_menu(window, g_object_get_data(G_OBJECT(row), "playlist"), row, x, y);
}


static GdkContentProvider *start_playlist_drag(GtkDragSource *source, double x, double y, GtkWidget *row) {
	CrimsonPlaylist *playlist = g_object_get_data(G_OBJECT(row), "playlist");
	gtk_drag_source_set_icon(source, gtk_widget_paintable_new(row), (int)x, (int)y);
	return gdk_content_provider_new_typed(CRIMSON_TYPE_PLAYLIST, playlist);
}


/* The whole row lights up while something it takes is held over it, rather than only its contents */
static void highlight_drop(GtkWidget *contents, gboolean is_highlighted) {
	GtkWidget *row = gtk_widget_get_parent(contents);
	if (!row)
		return;
	if (is_highlighted)
		gtk_widget_add_css_class(row, "drop-target");
	else
		gtk_widget_remove_css_class(row, "drop-target");
}


/* Dropping a playlist on another puts it in that one's place and keeps the order for the next launch */
static gboolean move_playlist(Window *window, CrimsonPlaylist *moved, CrimsonPlaylist *onto) {
	guint from = 0, to = 0;
	if (!moved || moved == onto || !g_list_store_find(window->playlists, moved, &from)
		|| !g_list_store_find(window->playlists, onto, &to))
		return FALSE;
	g_object_ref(moved);
	g_list_store_remove(window->playlists, from);
	g_list_store_insert(window->playlists, to, moved);
	g_object_unref(moved);
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->playlists));
	g_strfreev(window->state.playlist_order);
	window->state.playlist_order = g_new0(char *, count + 1);
	for (guint index = 0; index < count; index++) {
		g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(G_LIST_MODEL(window->playlists), index);
		window->state.playlist_order[index] = g_strdup(playlist->id);
	}
	save_cached_playlists(window->playlists);
	select_playlist_row(window, window->state.playlist_id);
	return TRUE;
}


/* A playlist dropped on a playlist moves there, a song or song card is added to it, and an artist or album card
 * is pinned to the sidebar */
static gboolean drop_on_playlist(GtkDropTarget *target, const GValue *value, double x, double y, Window *window) {
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
	highlight_drop(row, FALSE);
	CrimsonPlaylist *onto = g_object_get_data(G_OBJECT(row), "playlist");
	if (G_VALUE_HOLDS(value, CRIMSON_TYPE_PLAYLIST))
		return move_playlist(window, g_value_get_object(value), onto);
	if (G_VALUE_HOLDS(value, CRIMSON_TYPE_SONG)) {
		add_song_to(window, onto, CRIMSON_SONG(g_value_get_object(value)));
		return TRUE;
	}
	Card *card = g_value_get_boxed(value);
	if (!card->video_id)
		return FALSE;
	g_autoptr(CrimsonSong) song = g_object_new(CRIMSON_TYPE_SONG, NULL);
	song->id = g_strdup(card->video_id);
	song->title = g_strdup(card->title);
	song->artist = g_strdup(card->subtitle);
	song->artwork = g_strdup(card->artwork);
	add_song_to(window, onto, song);
	return TRUE;
}


/* Artists and albums are pinned rather than added to a playlist, so a playlist turns them away and the whole sidebar
 * takes them instead */
static gboolean is_pinnable(const GValue *value) {
	return value && G_VALUE_HOLDS(value, CRIMSON_TYPE_CARD) && !((Card *)g_value_get_boxed(value))->video_id;
}


static void check_playlist_drop(GtkDropTarget *target, GParamSpec *spec, Window *window) {
	const GValue *value = gtk_drop_target_get_value(target);
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
	if (is_pinnable(value)) {
		gtk_drop_target_reject(target);
		highlight_drop(row, FALSE);
	} else if (value) {
		highlight_drop(row, TRUE);
	}
}


static GdkDragAction enter_playlist(GtkDropTarget *target, double x, double y, Window *window) {
	return GDK_ACTION_COPY;
}


static void leave_playlist(GtkDropTarget *target, Window *window) {
	highlight_drop(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)), FALSE);
}


/* A double click plays the playlist in random order */
static void press_playlist_row(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	if (presses != 2)
		return;
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	play_playlist_now(window, g_object_get_data(G_OBJECT(row), "playlist"), TRUE);
}


static void add_playlist_row_controllers(GtkWidget *row, Window *window) {
	GtkGesture *double_click = gtk_gesture_click_new();
	g_signal_connect(double_click, "pressed", G_CALLBACK(press_playlist_row), window);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(double_click));
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(open_row_menu), window);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
	GtkDragSource *source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_MOVE | GDK_ACTION_COPY);
	g_signal_connect(source, "prepare", G_CALLBACK(start_playlist_drag), row);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
	GtkDropTarget *target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_MOVE | GDK_ACTION_COPY);
	GType types[] = {CRIMSON_TYPE_PLAYLIST, CRIMSON_TYPE_SONG, CRIMSON_TYPE_CARD};
	gtk_drop_target_set_gtypes(target, types, G_N_ELEMENTS(types));
	gtk_drop_target_set_preload(target, TRUE);
	g_signal_connect(target, "notify::value", G_CALLBACK(check_playlist_drop), window);
	g_signal_connect(target, "drop", G_CALLBACK(drop_on_playlist), window);
	g_signal_connect(target, "enter", G_CALLBACK(enter_playlist), window);
	g_signal_connect(target, "leave", G_CALLBACK(leave_playlist), window);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(target));
}


GtkWidget *add_cover_badge(GtkWidget *cover, const char *icon) {
	GtkWidget *badge = gtk_image_new_from_icon_name(icon);
	gtk_widget_add_css_class(badge, "cover-badge");
	gtk_widget_set_halign(badge, GTK_ALIGN_END);
	gtk_widget_set_valign(badge, GTK_ALIGN_END);
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), cover);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), badge);
	gtk_widget_set_valign(overlay, GTK_ALIGN_CENTER);
	return overlay;
}


GtkWidget *create_section_gap(void) {
	GtkWidget *gap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(gap, "section-gap");
	return gap;
}


/* The line above the YouTube section shows whenever the section holds anything, playlists or the card asking to sign
 * in */
void show_youtube_gap(Window *window) {
	gboolean has_card = window->sign_in_card && gtk_widget_get_visible(window->sign_in_card);
	gtk_widget_set_visible(window->youtube_gap, g_list_model_get_n_items(window->youtube_playlists) > 0 || has_card);
}


static void notice_youtube_playlists(GListModel *playlists, guint position, guint removed, guint added,
	Window *window) {
	show_youtube_gap(window);
}


static GtkWidget *create_media_library_row(Window *window);


/* Playlists kept on this computer carry a small computer in their corner, apart from the YouTube ones */
static GtkWidget *create_playlist_row(gpointer item, gpointer data) {
	CrimsonPlaylist *playlist = item;
	Window *window = data;
	if (playlist == window->local_playlist)
		return create_media_library_row(window);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	g_object_set_data_full(G_OBJECT(box), "playlist", g_object_ref(playlist), g_object_unref);
	GtkWidget *artwork = create_cover(SIDEBAR_ARTWORK_SIZE);
	gtk_widget_set_valign(artwork, GTK_ALIGN_CENTER);
	show_artwork(artwork, playlist->artwork);
	g_object_set_data(G_OBJECT(playlist), "sidebar-cover", artwork);
	gtk_box_append(GTK_BOX(box), is_local_playlist(playlist) ? add_cover_badge(artwork, LOCAL_PLAYLIST_ICON) : artwork);
	GtkWidget *label = gtk_label_new(playlist->title);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_tooltip_text(label, playlist->title);
	gtk_box_append(GTK_BOX(box), label);
	add_playlist_row_controllers(box, data);
	return box;
}


static void choose_playlist(GtkListBox *list, GtkListBoxRow *row, Window *window) {
	if (!row)
		return;
	g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(window->youtube_playlists,
		(guint)gtk_list_box_row_get_index(row));
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->dynamic_list));
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->local_list));
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->home_list));
	if (playlist != window->shown)
		show_playlist(window, playlist);
}


/* A click goes back to the playlist from any page, takes the lyrics and the video away and closes the sidebar of a
 * narrow window, which a selection the window makes by itself, as when the library comes back after an edit, never
 * does */
static void open_playlist(GtkListBox *list, GtkListBoxRow *row, Window *window) {
	close_song_views(window);
	adw_navigation_view_pop_to_tag(ADW_NAVIGATION_VIEW(window->navigation), PLAYLIST_TAG);
	window->is_back_to_search = FALSE;
	update_back_button(window);
	if (adw_overlay_split_view_get_collapsed(ADW_OVERLAY_SPLIT_VIEW(window->split)))
		adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(window->split), FALSE);
}


/* Local media ============================================================== */


static void choose_local(GtkListBox *list, GtkListBoxRow *row, Window *window) {
	if (!row)
		return;
	g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(window->local_section,
		(guint)gtk_list_box_row_get_index(row));
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->playlist_list));
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->dynamic_list));
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->home_list));
	if (playlist != window->shown)
		show_playlist(window, playlist);
}


/* A double click plays the whole library in random order */
static void press_local_row(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	if (presses == 2)
		play_playlist_now(window, window->local_playlist, TRUE);
}


void select_local_row(Window *window) {
	gtk_list_box_select_row(GTK_LIST_BOX(window->local_list),
		gtk_list_box_get_row_at_index(GTK_LIST_BOX(window->local_list), 0));
}


static GtkWidget *create_media_library_row(Window *window) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	window->local_cover = create_cover(SIDEBAR_ARTWORK_SIZE);
	show_artwork(window->local_cover, LOCAL_LIBRARY_ARTWORK);
	gtk_widget_set_valign(window->local_cover, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), window->local_cover);
	GtkWidget *label = gtk_label_new(TEXT_LOCAL_LIBRARY);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(box), label);
	GtkGesture *double_click = gtk_gesture_click_new();
	g_signal_connect(double_click, "pressed", G_CALLBACK(press_local_row), window);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(double_click));
	return box;
}


/* Home lights its own row in place of the playlist's while its page is open */
static void open_home(GtkListBox *list, GtkListBoxRow *row, Window *window) {
	close_song_views(window);
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->playlist_list));
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->local_list));
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->dynamic_list));
	gtk_list_box_select_row(list, row);
	open_card(HOME_TAG, window);
	if (adw_overlay_split_view_get_collapsed(ADW_OVERLAY_SPLIT_VIEW(window->split)))
		adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(window->split), FALSE);
}


static GtkWidget *create_home_list(Window *window) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	/* The listener's own account picture takes the emblem's place once YouTube Music gives it */
	window->home_cover = create_cover(SIDEBAR_ARTWORK_SIZE);
	show_artwork(window->home_cover, window->state.account_photo ? window->state.account_photo : HOME_ARTWORK);
	gtk_widget_set_valign(window->home_cover, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), window->home_cover);
	GtkWidget *label = gtk_label_new(TEXT_HOME);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(box), label);
	window->home_list = gtk_list_box_new();
	gtk_widget_add_css_class(window->home_list, "navigation-sidebar");
	gtk_widget_add_css_class(window->home_list, "home-list");
	gtk_list_box_append(GTK_LIST_BOX(window->home_list), box);
	gtk_widget_set_visible(window->home_list, FALSE);
	g_signal_connect(window->home_list, "row-activated", G_CALLBACK(open_home), window);
	return window->home_list;
}


static gboolean is_local_item(gpointer item, gpointer data) {
	return is_local_playlist(item);
}


static gboolean is_youtube_item(gpointer item, gpointer data) {
	return !is_local_playlist(item);
}


/* The music on this computer is always first in the sidebar, signed in or not, with the playlists kept here under
 * it and the YouTube ones after a gap */
GtkWidget *create_local_section(Window *window) {
	GListStore *library = g_list_store_new(CRIMSON_TYPE_PLAYLIST);
	g_list_store_append(library, window->local_playlist);
	GtkFilterListModel *local_playlists = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(window->playlists)),
		GTK_FILTER(gtk_custom_filter_new(is_local_item, NULL, NULL)));
	GListStore *parts = g_list_store_new(G_TYPE_LIST_MODEL);
	g_list_store_append(parts, library);
	g_list_store_append(parts, local_playlists);
	g_object_unref(library);
	g_object_unref(local_playlists);
	window->local_section = G_LIST_MODEL(gtk_flatten_list_model_new(G_LIST_MODEL(parts)));
	window->local_list = gtk_list_box_new();
	gtk_widget_add_css_class(window->local_list, "navigation-sidebar");
	gtk_list_box_bind_model(GTK_LIST_BOX(window->local_list), window->local_section, create_playlist_row, window,
		NULL);
	g_signal_connect(window->local_list, "row-selected", G_CALLBACK(choose_local), window);
	g_signal_connect(window->local_list, "row-activated", G_CALLBACK(open_playlist), window);
	return window->local_list;
}


/* Pins =================================================================== */


static void open_pin(GtkListBox *list, GtkListBoxRow *row, Window *window) {
	Card *pin = g_ptr_array_index(window->pins, (guint)gtk_list_box_row_get_index(row));
	open_card(pin->id, window);
}


static void open_pin_row_menu(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	open_pin_menu(window, g_object_get_data(G_OBJECT(row), "pin-id"), row, x, y);
}


static GtkWidget *create_pin_row(Window *window, Card *pin) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	g_object_set_data_full(G_OBJECT(box), "pin-id", g_strdup(pin->id), g_free);
	GtkWidget *artwork = create_cover(SIDEBAR_ARTWORK_SIZE);
	if (g_str_has_prefix(pin->id, "UC"))
		gtk_widget_add_css_class(artwork, "round");
	gtk_widget_set_valign(artwork, GTK_ALIGN_CENTER);
	show_artwork(artwork, pin->artwork);
	gtk_box_append(GTK_BOX(box), artwork);
	GtkWidget *label = gtk_label_new(pin->title);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_tooltip_text(label, pin->title);
	gtk_box_append(GTK_BOX(box), label);
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(open_pin_row_menu), window);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
	return box;
}


/* Pins keep their own section under a heading, apart from the playlists */
static void show_pins(Window *window) {
	gtk_list_box_remove_all(GTK_LIST_BOX(window->pin_list));
	for (guint index = 0; index < window->pins->len; index++)
		gtk_list_box_append(GTK_LIST_BOX(window->pin_list), create_pin_row(window, g_ptr_array_index(window->pins, index)));
	gtk_widget_set_visible(window->pin_heading, window->pins->len > 0);
	gtk_widget_set_visible(window->pin_list, window->pins->len > 0);
}


void pin_card(Window *window, const Card *card) {
	for (guint index = 0; index < window->pins->len; index++) {
		if (g_str_equal(((Card *)g_ptr_array_index(window->pins, index))->id, card->id))
			return;
	}
	g_ptr_array_add(window->pins, copy_card(card));
	save_pins(window->pins);
	show_pins(window);
}


void unpin_card(Window *window, const char *id) {
	for (guint index = 0; index < window->pins->len; index++) {
		if (g_str_equal(((Card *)g_ptr_array_index(window->pins, index))->id, id)) {
			g_ptr_array_remove_index(window->pins, index);
			break;
		}
	}
	save_pins(window->pins);
	show_pins(window);
}


/* Artist and album cards dropped anywhere on the sidebar are pinned, and songs are left to the playlists */
static gboolean drop_on_sidebar(GtkDropTarget *target, const GValue *value, double x, double y, Window *window) {
	if (!is_pinnable(value))
		return FALSE;
	pin_card(window, g_value_get_boxed(value));
	return TRUE;
}


static void check_sidebar_drop(GtkDropTarget *target, GParamSpec *spec, Window *window) {
	const GValue *value = gtk_drop_target_get_value(target);
	if (value && !is_pinnable(value))
		gtk_drop_target_reject(target);
}


static GtkWidget *create_pins(Window *window) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	window->pin_heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_add_css_class(window->pin_heading, "pin-heading");
	gtk_box_append(GTK_BOX(window->pin_heading), create_section_gap());
	GtkWidget *label = gtk_label_new(TEXT_PINNED);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "heading");
	gtk_widget_add_css_class(label, "dimmed");
	gtk_box_append(GTK_BOX(window->pin_heading), label);
	window->pin_list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(window->pin_list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(window->pin_list, "navigation-sidebar");
	g_signal_connect(window->pin_list, "row-activated", G_CALLBACK(open_pin), window);
	gtk_box_append(GTK_BOX(box), window->pin_heading);
	gtk_box_append(GTK_BOX(box), window->pin_list);
	window->pins = load_pins();
	show_pins(window);
	return box;
}


static void open_empty_menu(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	open_sidebar_menu(window, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)), x, y);
}


GtkWidget *create_sidebar(Window *window) {
	window->youtube_playlists = G_LIST_MODEL(gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(window->playlists)),
		GTK_FILTER(gtk_custom_filter_new(is_youtube_item, NULL, NULL))));
	window->playlist_list = gtk_list_box_new();
	gtk_widget_add_css_class(window->playlist_list, "navigation-sidebar");
	gtk_list_box_bind_model(GTK_LIST_BOX(window->playlist_list), window->youtube_playlists, create_playlist_row,
		window, NULL);
	g_signal_connect(window->playlist_list, "row-selected", G_CALLBACK(choose_playlist), window);
	g_signal_connect(window->playlist_list, "row-activated", G_CALLBACK(open_playlist), window);
	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(content), create_local_section(window));
	window->youtube_gap = create_section_gap();
	gtk_box_append(GTK_BOX(content), window->youtube_gap);
	g_signal_connect(window->youtube_playlists, "items-changed", G_CALLBACK(notice_youtube_playlists), window);
	show_youtube_gap(window);
	window->sign_in_card = create_sign_in_card();
	gtk_widget_set_visible(window->sign_in_card, FALSE);
	gtk_box_append(GTK_BOX(content), create_home_list(window));
	gtk_box_append(GTK_BOX(content), window->playlist_list);
	gtk_box_append(GTK_BOX(content), window->sign_in_card);
	/* Only the YouTube playlists wait for YouTube, under a spinner of their own, while everything kept on this computer
	 * shows at once */
	window->playlists_spinner = adw_spinner_new();
	gtk_widget_add_css_class(window->playlists_spinner, "playlists-spinner");
	gtk_box_append(GTK_BOX(content), window->playlists_spinner);
	window->playlists_empty = create_status("view-list-symbolic", TEXT_NO_PLAYLISTS, TEXT_NO_PLAYLISTS_DETAIL);
	gtk_widget_add_css_class(window->playlists_empty, "compact");
	gtk_widget_set_visible(window->playlists_empty, FALSE);
	gtk_box_append(GTK_BOX(content), window->playlists_empty);
	gtk_box_append(GTK_BOX(content), create_dynamic_section(window));
	gtk_box_append(GTK_BOX(content), create_pins(window));
	window->sidebar_stack = create_scrolled(content);
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(open_empty_menu), window);
	gtk_widget_add_controller(window->sidebar_stack, GTK_EVENT_CONTROLLER(click));
	GtkDropTarget *target = gtk_drop_target_new(CRIMSON_TYPE_CARD, GDK_ACTION_COPY);
	gtk_drop_target_set_preload(target, TRUE);
	g_signal_connect(target, "notify::value", G_CALLBACK(check_sidebar_drop), window);
	g_signal_connect(target, "drop", G_CALLBACK(drop_on_sidebar), window);
	gtk_widget_add_controller(window->sidebar_stack, GTK_EVENT_CONTROLLER(target));
	return window->sidebar_stack;
}

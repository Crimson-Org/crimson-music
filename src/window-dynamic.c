#include "../lang/en.h"
#include "artwork.h"
#include "collage.h"
#include "dynamic-editor.h"
#include "song-info.h"
#include "window-private.h"

#define SIDEBAR_ARTWORK_SIZE 32
/* Gathered songs arrive one after another, so the playlists are filtered again a moment after the last one */
#define REFILTER_DELAY_MS 400
#define COLLAGE_COVERS 3


/* Dynamic playlists pick from the liked songs and the music on this computer, so they work signed out too */
static GListModel *get_pool(Window *window) {
	if (!window->dynamic_pool)
		window->dynamic_pool = g_list_store_new(CRIMSON_TYPE_SONG);
	g_autoptr(GPtrArray) songs = g_ptr_array_new_with_free_func(g_object_unref);
	CrimsonPlaylist *sources[] = {find_playlist(window, LIKED_PLAYLIST), window->local_playlist};
	for (guint source = 0; source < G_N_ELEMENTS(sources); source++) {
		GListModel *model = sources[source] && sources[source]->has_songs ? G_LIST_MODEL(sources[source]->songs)
			: NULL;
		for (guint index = 0; model && index < g_list_model_get_n_items(model); index++)
			g_ptr_array_add(songs, g_list_model_get_item(model, index));
	}
	g_list_store_splice(window->dynamic_pool, 0, g_list_model_get_n_items(G_LIST_MODEL(window->dynamic_pool)),
		songs->pdata, songs->len);
	return G_LIST_MODEL(window->dynamic_pool);
}


static DynamicRules *find_rules(Window *window, const char *id) {
	for (guint index = 0; index < window->dynamic_rules->len; index++) {
		DynamicRules *rules = g_ptr_array_index(window->dynamic_rules, index);
		if (g_strcmp0(rules->id, id) == 0)
			return rules;
	}
	return NULL;
}


CrimsonPlaylist *find_dynamic_playlist(Window *window, const char *id) {
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(window->dynamic_playlists)); index++) {
		g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(G_LIST_MODEL(window->dynamic_playlists), index);
		if (g_strcmp0(playlist->id, id) == 0)
			return playlist;
	}
	return NULL;
}


typedef struct {
	Window *window;
	char *id, *covers;
} CollageWait;


static void show_collage(const char *path, gpointer data) {
	CollageWait *wait = data;
	CrimsonPlaylist *playlist = find_dynamic_playlist(wait->window, wait->id);
	gboolean is_latest = playlist && g_strcmp0(g_object_get_data(G_OBJECT(playlist), "covers"), wait->covers) == 0;
	if (path && is_latest) {
		g_set_str(&playlist->artwork, path);
		GtkWidget *cover = g_object_get_data(G_OBJECT(playlist), "sidebar-cover");
		if (cover)
			show_artwork(cover, path);
		if (playlist == wait->window->shown)
			set_song_page_heading(wait->window->playlist_page, playlist->title, playlist->subtitle,
				playlist->artwork, FALSE);
	}
	g_free(wait->id);
	g_free(wait->covers);
	g_free(wait);
}


/* The cover is a collage of the first few different covers in the playlist, made again only when they change */
static void make_playlist_collage(Window *window, CrimsonPlaylist *playlist) {
	g_autoptr(GPtrArray) covers = g_ptr_array_new();
	guint count = g_list_model_get_n_items(G_LIST_MODEL(playlist->songs));
	for (guint index = 0; index < count && covers->len < COLLAGE_COVERS; index++) {
		g_autoptr(CrimsonSong) song = g_list_model_get_item(G_LIST_MODEL(playlist->songs), index);
		if (*song->artwork && !g_ptr_array_find_with_equal_func(covers, song->artwork, g_str_equal, NULL))
			g_ptr_array_add(covers, song->artwork);
	}
	g_ptr_array_add(covers, NULL);
	g_autofree char *joined = g_strjoinv("\n", (char **)covers->pdata);
	g_ptr_array_set_size(covers, covers->len - 1);
	if (g_strcmp0(g_object_get_data(G_OBJECT(playlist), "covers"), joined) == 0)
		return;
	g_object_set_data_full(G_OBJECT(playlist), "covers", g_strdup(joined), g_free);
	CollageWait *wait = g_new0(CollageWait, 1);
	wait->window = window;
	wait->id = g_strdup(playlist->id);
	wait->covers = g_strdup(joined);
	make_collage(covers, show_collage, wait);
}


static void describe_playlist(Window *window, CrimsonPlaylist *playlist) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(playlist->songs));
	g_free(playlist->subtitle);
	playlist->subtitle = g_strdup_printf(g_dngettext(NULL, TEXT_DYNAMIC_SUBTITLE_ONE, TEXT_DYNAMIC_SUBTITLE, count),
		count);
	make_playlist_collage(window, playlist);
}


/* Each dynamic playlist holds the liked songs that pass its filters right now, in the order of Liked Music */
static void refilter(Window *window) {
	GListModel *pool = get_pool(window);
	if (!pool)
		return;
	for (guint index = 0; index < window->dynamic_rules->len; index++) {
		DynamicRules *rules = g_ptr_array_index(window->dynamic_rules, index);
		CrimsonPlaylist *playlist = find_dynamic_playlist(window, rules->id);
		g_autoptr(GPtrArray) songs = filter_dynamic_songs(pool, rules);
		if (!playlist || !replace_songs(playlist, songs))
			continue;
		describe_playlist(window, playlist);
		if (playlist == window->shown) {
			set_song_page_heading(window->playlist_page, playlist->title, playlist->subtitle, playlist->artwork,
				FALSE);
			update_playlist_page(window);
		}
	}
	update_dynamic_editor();
}


static gboolean refilter_later(gpointer data) {
	Window *window = data;
	window->refilter_source = 0;
	refilter(window);
	return G_SOURCE_REMOVE;
}


void update_dynamic_playlists(Window *window) {
	if (!window->refilter_source)
		window->refilter_source = g_timeout_add(REFILTER_DELAY_MS, refilter_later, window);
	update_dynamic_editor();
}


/* Sidebar ================================================================== */


/* A double click plays the dynamic playlist in random order, as with the other playlists */
static void press_dynamic_row(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	GtkWidget *box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	CrimsonPlaylist *playlist = find_dynamic_playlist(window, g_object_get_data(G_OBJECT(box), "playlist-id"));
	if (presses == 2 && playlist)
		play_playlist_now(window, playlist, TRUE);
}


/* Dynamic playlists drag by their id, which only another dynamic playlist takes, as the place to move to */
static GdkContentProvider *start_dynamic_drag(GtkDragSource *source, double x, double y, Window *window) {
	GtkWidget *box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
	gtk_drag_source_set_icon(source, gtk_widget_paintable_new(box), (int)x, (int)y);
	return gdk_content_provider_new_typed(G_TYPE_STRING, g_object_get_data(G_OBJECT(box), "playlist-id"));
}


static void show_rows(Window *window);


/* The dropped playlist takes the place of the one it lands on, and the order is kept for the next launch */
static gboolean drop_dynamic(GtkDropTarget *target, const GValue *value, double x, double y, Window *window) {
	GtkWidget *box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
	DynamicRules *moved = find_rules(window, g_value_get_string(value));
	DynamicRules *onto = find_rules(window, g_object_get_data(G_OBJECT(box), "playlist-id"));
	guint from = 0, to = 0;
	if (!moved || !onto || moved == onto || !g_ptr_array_find(window->dynamic_rules, moved, &from)
		|| !g_ptr_array_find(window->dynamic_rules, onto, &to))
		return FALSE;
	g_ptr_array_steal_index(window->dynamic_rules, from);
	g_ptr_array_insert(window->dynamic_rules, (int)to, moved);
	CrimsonPlaylist *playlist = find_dynamic_playlist(window, moved->id);
	guint place = 0;
	if (playlist && g_list_store_find(window->dynamic_playlists, playlist, &place)) {
		g_object_ref(playlist);
		g_list_store_remove(window->dynamic_playlists, place);
		g_list_store_insert(window->dynamic_playlists, to, playlist);
		g_object_unref(playlist);
	}
	save_dynamic_playlists(window->dynamic_rules);
	show_rows(window);
	return TRUE;
}


/* The collage, with a small filter badge in its corner that tells it apart from the other playlists */
static GtkWidget *create_row(Window *window, CrimsonPlaylist *playlist) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	GtkWidget *cover = create_cover(SIDEBAR_ARTWORK_SIZE);
	show_artwork(cover, playlist->artwork);
	g_object_set_data(G_OBJECT(playlist), "sidebar-cover", cover);
	gtk_box_append(GTK_BOX(box), add_cover_badge(cover, "crimson-dynamic-symbolic"));
	GtkWidget *label = gtk_label_new(playlist->title);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_tooltip_text(label, playlist->title);
	gtk_box_append(GTK_BOX(box), label);
	g_object_set_data_full(G_OBJECT(box), "playlist-id", g_strdup(playlist->id), g_free);
	GtkGesture *double_click = gtk_gesture_click_new();
	g_signal_connect(double_click, "pressed", G_CALLBACK(press_dynamic_row), window);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(double_click));
	GtkDragSource *source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
	g_signal_connect(source, "prepare", G_CALLBACK(start_dynamic_drag), window);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(source));
	GtkDropTarget *target = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
	g_signal_connect(target, "drop", G_CALLBACK(drop_dynamic), window);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(target));
	return box;
}


static void show_rows(Window *window) {
	gtk_list_box_remove_all(GTK_LIST_BOX(window->dynamic_list));
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(window->dynamic_playlists)); index++) {
		g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(G_LIST_MODEL(window->dynamic_playlists), index);
		gtk_list_box_append(GTK_LIST_BOX(window->dynamic_list), create_row(window, playlist));
	}
	guint count = g_list_model_get_n_items(G_LIST_MODEL(window->dynamic_playlists));
	gtk_widget_set_visible(window->dynamic_gap, count > 0);
	gtk_widget_set_visible(window->dynamic_list, count > 0);
	if (window->shown && g_str_has_prefix(window->shown->id, DYNAMIC_PREFIX))
		select_dynamic_row(window, window->shown->id);
}


static CrimsonPlaylist *create_dynamic_playlist(DynamicRules *rules) {
	CrimsonPlaylist *playlist = g_object_new(CRIMSON_TYPE_PLAYLIST, NULL);
	playlist->id = g_strdup(rules->id);
	playlist->title = g_strdup(rules->name);
	playlist->subtitle = g_strdup("");
	playlist->artwork = g_strdup("");
	playlist->has_songs = TRUE;
	return playlist;
}


static void rebuild(Window *window) {
	g_list_store_remove_all(window->dynamic_playlists);
	for (guint index = 0; index < window->dynamic_rules->len; index++) {
		g_autoptr(CrimsonPlaylist) playlist = create_dynamic_playlist(g_ptr_array_index(window->dynamic_rules, index));
		g_list_store_append(window->dynamic_playlists, playlist);
	}
	refilter(window);
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(window->dynamic_playlists)); index++) {
		g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(G_LIST_MODEL(window->dynamic_playlists), index);
		describe_playlist(window, playlist);
	}
	show_rows(window);
}


void select_dynamic_row(Window *window, const char *id) {
	for (int index = 0;; index++) {
		GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(window->dynamic_list), index);
		if (!row)
			return;
		const char *row_id = g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(row)), "playlist-id");
		if (g_strcmp0(row_id, id) == 0) {
			gtk_list_box_select_row(GTK_LIST_BOX(window->dynamic_list), row);
			return;
		}
	}
}


static void finish_editing(DynamicRules *rules, gpointer data) {
	Window *window = data;
	DynamicRules *old = find_rules(window, rules->id);
	if (old) {
		guint position = 0;
		g_ptr_array_find(window->dynamic_rules, old, &position);
		g_ptr_array_index(window->dynamic_rules, position) = rules;
		free_dynamic_rules(old);
	} else {
		g_autofree char *uuid = g_uuid_string_random();
		rules->id = g_strconcat(DYNAMIC_PREFIX, uuid, NULL);
		g_ptr_array_add(window->dynamic_rules, rules);
	}
	save_dynamic_playlists(window->dynamic_rules);
	g_autofree char *id = g_strdup(rules->id);
	gboolean was_shown = window->shown && g_str_equal(window->shown->id, id);
	if (was_shown)
		g_clear_object(&window->shown);
	rebuild(window);
	select_dynamic_row(window, id);
	adw_navigation_view_pop_to_tag(ADW_NAVIGATION_VIEW(window->navigation), PLAYLIST_TAG);
}


void edit_dynamic_playlist(Window *window, const char *id) {
	show_dynamic_editor(GTK_WIDGET(window->window), id ? find_rules(window, id) : NULL, get_pool(window),
		finish_editing, window);
}


void delete_dynamic_playlist(Window *window, const char *id) {
	DynamicRules *rules = find_rules(window, id);
	if (!rules)
		return;
	g_ptr_array_remove(window->dynamic_rules, rules);
	save_dynamic_playlists(window->dynamic_rules);
	if (window->shown && g_str_equal(window->shown->id, id)) {
		select_playlist_row(window, LIKED_PLAYLIST);
	}
	rebuild(window);
}


static void choose_dynamic(GtkListBox *list, GtkListBoxRow *row, Window *window) {
	if (!row)
		return;
	const char *id = g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(row)), "playlist-id");
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->playlist_list));
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->local_list));
	gtk_list_box_unselect_all(GTK_LIST_BOX(window->home_list));
	CrimsonPlaylist *playlist = find_dynamic_playlist(window, id);
	if (playlist && playlist != window->shown)
		show_playlist(window, playlist);
}


static void open_dynamic(GtkListBox *list, GtkListBoxRow *row, Window *window) {
	close_song_views(window);
	adw_navigation_view_pop_to_tag(ADW_NAVIGATION_VIEW(window->navigation), PLAYLIST_TAG);
	window->is_back_to_search = FALSE;
	update_back_button(window);
	if (adw_overlay_split_view_get_collapsed(ADW_OVERLAY_SPLIT_VIEW(window->split)))
		adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(window->split), FALSE);
}


static void open_row_menu(GtkGestureClick *gesture, int presses, double x, double y, Window *window) {
	GtkWidget *list = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), (int)y);
	const char *id = row ? g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(row)), "playlist-id") : NULL;
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	if (id)
		open_dynamic_menu(window, id, list, x, y);
}


GtkWidget *create_dynamic_section(Window *window) {
	window->dynamic_rules = load_dynamic_playlists();
	window->dynamic_playlists = g_list_store_new(CRIMSON_TYPE_PLAYLIST);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	window->dynamic_gap = create_section_gap();
	gtk_box_append(GTK_BOX(box), window->dynamic_gap);
	window->dynamic_list = gtk_list_box_new();
	gtk_widget_add_css_class(window->dynamic_list, "navigation-sidebar");
	g_signal_connect(window->dynamic_list, "row-selected", G_CALLBACK(choose_dynamic), window);
	g_signal_connect(window->dynamic_list, "row-activated", G_CALLBACK(open_dynamic), window);
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
	g_signal_connect(click, "pressed", G_CALLBACK(open_row_menu), window);
	gtk_widget_add_controller(window->dynamic_list, GTK_EVENT_CONTROLLER(click));
	gtk_box_append(GTK_BOX(box), window->dynamic_list);
	rebuild(window);
	return box;
}

#include "../lang/en.h"
#include "artwork.h"
#include "common/startup.h"
#include "library.h"
#include "local-library.h"
#include "song-list.h"

#define ARTWORK_SIZE 40
#define LENGTH_CHARACTERS 8
#define PLAYING_CLASS "playing"
#define SPOTLIGHT_CLASS "spotlight"
#define SPOTLIGHT_MILLISECONDS 2200
#define DROP_CLASS "drop-target"
#define SECONDARY_BUTTON 3
#define FOLD_MILLISECONDS 220
/* A song added longer ago than this shows plainly once scrolled to, instead of sliding open then */
#define ARRIVAL_MICROSECONDS (2 * G_USEC_PER_SEC)
#define ARRIVING_KEY "arriving"

/* Widgets of one row, looked up again when the row shows another song */
typedef struct {
	GtkWidget *revealer, *box, *artwork, *title, *details, *length, *like, *dislike, *love, *add;
	GtkListItem *item;
	GtkWidget *list;
} SongRow;

/* What a list needs to answer its rows */
typedef struct {
	const SongListEvents *events;
	gpointer data;
	gboolean is_reorderable;
	GListModel *songs, *suggestions;
	/* The songs and the suggestions as the two sections the list shows */
	GListStore *parts;
} SongListSetup;

/* A row's small round button, named apart from buttons so the theme's many button rules are not matched again
 * each time the pointer crosses into another row */
#define CRIMSON_TYPE_ROW_ACTION (crimson_row_action_get_type())
G_DECLARE_FINAL_TYPE(CrimsonRowAction, crimson_row_action, CRIMSON, ROW_ACTION, GtkButton)

struct _CrimsonRowAction {
	GtkButton parent;
};

G_DEFINE_FINAL_TYPE(CrimsonRowAction, crimson_row_action, GTK_TYPE_BUTTON)


static void crimson_row_action_class_init(CrimsonRowActionClass *type) {
	gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(type), "row-action");
}


static void crimson_row_action_init(CrimsonRowAction *action) {
}


static GtkWidget *create_row_action(const char *icon, const char *style) {
	GtkWidget *action = g_object_new(CRIMSON_TYPE_ROW_ACTION, "icon-name", icon, NULL);
	if (style)
		gtk_widget_add_css_class(action, style);
	gtk_widget_set_valign(action, GTK_ALIGN_CENTER);
	return action;
}


static char *playing_video;
/* The song just shown to the listener, lit for a moment so the eye finds it */
static char *spotlit_video;
static guint spotlight_source;
static GPtrArray *bound_rows;
static GHashTable *liked_videos;
static SongLoveCheck love_check;
static gboolean is_love_shown;
static GPtrArray *followers;

#define FOLLOWED_VIDEO_KEY "followed-video"
#define SUGGESTED_KEY "suggested"


gboolean is_suggested_song(CrimsonSong *song) {
	return song && g_object_get_data(G_OBJECT(song), SUGGESTED_KEY) != NULL;
}


static void show_playing(SongRow *row) {
	CrimsonSong *song = gtk_list_item_get_item(row->item);
	if (song && g_strcmp0(song->id, playing_video) == 0)
		gtk_widget_add_css_class(row->box, PLAYING_CLASS);
	else
		gtk_widget_remove_css_class(row->box, PLAYING_CLASS);
	if (song && g_strcmp0(song->id, spotlit_video) == 0)
		gtk_widget_add_css_class(row->box, SPOTLIGHT_CLASS);
	else
		gtk_widget_remove_css_class(row->box, SPOTLIGHT_CLASS);
}


static gboolean end_spotlight(gpointer data) {
	spotlight_source = 0;
	g_clear_pointer(&spotlit_video, g_free);
	for (guint index = 0; bound_rows && index < bound_rows->len; index++)
		show_playing(g_ptr_array_index(bound_rows, index));
	return G_SOURCE_REMOVE;
}


typedef struct {
	GtkWidget *list;
	guint position;
} Reveal;


static gboolean scroll_to_song(gpointer data) {
	Reveal *reveal = data;
	gtk_list_view_scroll_to(GTK_LIST_VIEW(reveal->list), reveal->position, GTK_LIST_SCROLL_FOCUS, NULL);
	g_object_unref(reveal->list);
	g_free(reveal);
	return G_SOURCE_REMOVE;
}


void reveal_song_in_list(GtkWidget *list, const char *video_id) {
	GListModel *songs = G_LIST_MODEL(gtk_list_view_get_model(GTK_LIST_VIEW(list)));
	guint count = songs ? g_list_model_get_n_items(songs) : 0;
	for (guint position = 0; position < count; position++) {
		g_autoptr(CrimsonSong) song = g_list_model_get_item(songs, position);
		if (!g_str_equal(song->id, video_id))
			continue;
		g_set_str(&spotlit_video, video_id);
		g_clear_handle_id(&spotlight_source, g_source_remove);
		spotlight_source = g_timeout_add(SPOTLIGHT_MILLISECONDS, end_spotlight, NULL);
		for (guint index = 0; bound_rows && index < bound_rows->len; index++)
			show_playing(g_ptr_array_index(bound_rows, index));
		Reveal *reveal = g_new0(Reveal, 1);
		reveal->list = g_object_ref(list);
		reveal->position = position;
		/* The page may only now be sliding in, so the list scrolls once it has its size */
		run_after_frame(list, scroll_to_song, reveal);
		return;
	}
}


gboolean is_video_liked(const char *video_id) {
	return liked_videos && video_id && g_hash_table_contains(liked_videos, video_id);
}


/* A loved song keeps its filled heart showing, while others show an outline only under the pointer */
static void show_loved(SongRow *row) {
	CrimsonSong *song = gtk_list_item_get_item(row->item);
	gboolean is_loved = song && love_check && love_check(song);
	gtk_widget_set_visible(row->love, is_love_shown && !is_suggested_song(song));
	gtk_button_set_icon_name(GTK_BUTTON(row->love), is_loved ? "crimson-heart-filled-symbolic" : "crimson-heart-symbolic");
	gtk_widget_set_tooltip_text(row->love, is_loved ? TEXT_UNLOVE : TEXT_LOVE);
	if (is_loved)
		gtk_widget_add_css_class(row->love, "loved");
	else
		gtk_widget_remove_css_class(row->love, "loved");
}


void set_song_love_check(SongLoveCheck check, gboolean is_shown) {
	love_check = check;
	is_love_shown = is_shown;
	for (guint index = 0; bound_rows && index < bound_rows->len; index++)
		show_loved(g_ptr_array_index(bound_rows, index));
}


/* A liked song keeps its filled thumb showing, while others show an outline only under the pointer */
static void show_liked(SongRow *row) {
	CrimsonSong *song = gtk_list_item_get_item(row->item);
	gboolean is_liked = song && is_video_liked(song->id);
	/* Local songs are not on YouTube, so there is nothing to like */
	gtk_widget_set_visible(row->like, !is_suggested_song(song) && !(song && is_local_song(song->id)));
	gtk_button_set_icon_name(GTK_BUTTON(row->like), is_liked ? "crimson-like-filled-symbolic" : "crimson-like-symbolic");
	gtk_widget_set_tooltip_text(row->like, is_liked ? TEXT_UNLIKE : TEXT_LIKE);
	if (is_liked)
		gtk_widget_add_css_class(row->like, "liked");
	else
		gtk_widget_remove_css_class(row->like, "liked");
}


/* A disliked song keeps its filled thumb showing, as a liked one does */
static void show_disliked(SongRow *row) {
	CrimsonSong *song = gtk_list_item_get_item(row->item);
	gboolean is_disliked = song && is_video_disliked(song->id);
	gtk_widget_set_visible(row->dislike, gtk_widget_get_visible(row->like));
	gtk_button_set_icon_name(GTK_BUTTON(row->dislike),
		is_disliked ? "crimson-dislike-filled-symbolic" : "crimson-dislike-symbolic");
	gtk_widget_set_tooltip_text(row->dislike, is_disliked ? TEXT_UNDISLIKE : TEXT_DISLIKE);
	if (is_disliked)
		gtk_widget_add_css_class(row->dislike, "disliked");
	else
		gtk_widget_remove_css_class(row->dislike, "disliked");
}


void update_disliked_rows(void) {
	for (guint index = 0; bound_rows && index < bound_rows->len; index++)
		show_disliked(g_ptr_array_index(bound_rows, index));
}


void set_liked_videos(GHashTable *video_ids) {
	if (liked_videos != video_ids) {
		g_clear_pointer(&liked_videos, g_hash_table_unref);
		liked_videos = video_ids ? g_hash_table_ref(video_ids) : NULL;
	}
	for (guint index = 0; bound_rows && index < bound_rows->len; index++)
		show_liked(g_ptr_array_index(bound_rows, index));
}


static void show_follower_playing(GtkWidget *widget) {
	if (g_strcmp0(g_object_get_data(G_OBJECT(widget), FOLLOWED_VIDEO_KEY), playing_video) == 0)
		gtk_widget_add_css_class(widget, PLAYING_CLASS);
	else
		gtk_widget_remove_css_class(widget, PLAYING_CLASS);
}


void set_playing_video(const char *video_id) {
	g_set_str(&playing_video, video_id);
	for (guint index = 0; bound_rows && index < bound_rows->len; index++)
		show_playing(g_ptr_array_index(bound_rows, index));
	for (guint index = 0; followers && index < followers->len; index++)
		show_follower_playing(g_ptr_array_index(followers, index));
}


static void forget_follower(gpointer data, GObject *widget) {
	g_ptr_array_remove_fast(followers, widget);
}


void follow_playing_video(GtkWidget *widget, const char *video_id) {
	if (!followers)
		followers = g_ptr_array_new();
	g_object_set_data_full(G_OBJECT(widget), FOLLOWED_VIDEO_KEY, g_strdup(video_id), g_free);
	g_ptr_array_add(followers, widget);
	g_object_weak_ref(G_OBJECT(widget), forget_follower, NULL);
	show_follower_playing(widget);
}


static GtkWidget *create_label(const char *style) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	if (style)
		gtk_widget_add_css_class(label, style);
	return label;
}


static void open_menu(GtkGestureClick *gesture, int presses, double x, double y, SongRow *row) {
	SongListSetup *setup = g_object_get_data(G_OBJECT(row->list), "setup");
	CrimsonSong *song = gtk_list_item_get_item(row->item);
	if (song && setup->events->open_menu)
		setup->events->open_menu(song, gtk_list_item_get_position(row->item), row->box, x, y, setup->data);
}


/* A song drags as itself, for dropping on a playlist, and as its place, for moving it within its own list, which
 * suggestions are no part of */
static GdkContentProvider *start_drag(GtkDragSource *source, double x, double y, SongRow *row) {
	CrimsonSong *song = gtk_list_item_get_item(row->item);
	if (!song)
		return NULL;
	gtk_drag_source_set_icon(source, gtk_widget_paintable_new(row->box), (int)x, (int)y);
	if (is_suggested_song(song))
		return gdk_content_provider_new_typed(CRIMSON_TYPE_SONG, song);
	GdkContentProvider *contents[] = {
		gdk_content_provider_new_typed(CRIMSON_TYPE_SONG, song),
		gdk_content_provider_new_typed(G_TYPE_UINT, gtk_list_item_get_position(row->item)),
	};
	return gdk_content_provider_new_union(contents, G_N_ELEMENTS(contents));
}


static gboolean drop_song(GtkDropTarget *target, const GValue *value, double x, double y, SongRow *row) {
	SongListSetup *setup = g_object_get_data(G_OBJECT(row->list), "setup");
	gtk_widget_remove_css_class(row->box, DROP_CLASS);
	guint from = g_value_get_uint(value), to = gtk_list_item_get_position(row->item);
	if (!setup->is_reorderable || from == to || is_suggested_song(gtk_list_item_get_item(row->item)))
		return FALSE;
	setup->events->move_song(from, to, setup->data);
	return TRUE;
}


static GdkDragAction enter_drop(GtkDropTarget *target, double x, double y, SongRow *row) {
	SongListSetup *setup = g_object_get_data(G_OBJECT(row->list), "setup");
	if (!setup->is_reorderable || is_suggested_song(gtk_list_item_get_item(row->item)))
		return 0;
	gtk_widget_add_css_class(row->box, DROP_CLASS);
	return GDK_ACTION_MOVE;
}


static void leave_drop(GtkDropTarget *target, SongRow *row) {
	gtk_widget_remove_css_class(row->box, DROP_CLASS);
}


static void toggle_like(GtkButton *button, SongRow *row) {
	SongListSetup *setup = g_object_get_data(G_OBJECT(row->list), "setup");
	CrimsonSong *song = gtk_list_item_get_item(row->item);
	if (song)
		setup->events->toggle_like(song, setup->data);
}


static void toggle_dislike(GtkButton *button, SongRow *row) {
	SongListSetup *setup = g_object_get_data(G_OBJECT(row->list), "setup");
	CrimsonSong *song = gtk_list_item_get_item(row->item);
	if (song)
		setup->events->toggle_dislike(song, setup->data);
}


static void toggle_love(GtkButton *button, SongRow *row) {
	SongListSetup *setup = g_object_get_data(G_OBJECT(row->list), "setup");
	CrimsonSong *song = gtk_list_item_get_item(row->item);
	if (song)
		setup->events->toggle_love(song, setup->data);
}


static void add_suggestion(GtkButton *button, SongRow *row) {
	SongListSetup *setup = g_object_get_data(G_OBJECT(row->list), "setup");
	CrimsonSong *song = gtk_list_item_get_item(row->item);
	if (song && setup->events->add_suggestion)
		setup->events->add_suggestion(song, setup->data);
}


static void add_row_controllers(SongRow *row) {
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), SECONDARY_BUTTON);
	g_signal_connect(click, "pressed", G_CALLBACK(open_menu), row);
	gtk_widget_add_controller(row->box, GTK_EVENT_CONTROLLER(click));
	GtkDragSource *source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_MOVE | GDK_ACTION_COPY);
	g_signal_connect(source, "prepare", G_CALLBACK(start_drag), row);
	gtk_widget_add_controller(row->box, GTK_EVENT_CONTROLLER(source));
	GtkDropTarget *target = gtk_drop_target_new(G_TYPE_UINT, GDK_ACTION_MOVE);
	g_signal_connect(target, "drop", G_CALLBACK(drop_song), row);
	g_signal_connect(target, "enter", G_CALLBACK(enter_drop), row);
	g_signal_connect(target, "leave", G_CALLBACK(leave_drop), row);
	gtk_widget_add_controller(row->box, GTK_EVENT_CONTROLLER(target));
}


static void set_up_row(GtkSignalListItemFactory *factory, GtkListItem *item, GtkWidget *list) {
	SongRow *row = g_new0(SongRow, 1);
	row->item = item;
	row->list = list;
	row->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_add_css_class(row->box, "song");
	GtkWidget *cover = gtk_overlay_new();
	row->artwork = create_cover(ARTWORK_SIZE);
	gtk_overlay_set_child(GTK_OVERLAY(cover), row->artwork);
	GtkWidget *playing = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
	gtk_widget_add_css_class(playing, "playing-icon");
	gtk_overlay_add_overlay(GTK_OVERLAY(cover), playing);
	gtk_widget_set_valign(cover, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(row->box), cover);
	GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_hexpand(labels, TRUE);
	gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
	row->title = create_label("song-title");
	row->details = create_label("dimmed");
	gtk_box_append(GTK_BOX(labels), row->title);
	gtk_box_append(GTK_BOX(labels), row->details);
	gtk_box_append(GTK_BOX(row->box), labels);
	row->love = create_row_action("crimson-heart-symbolic", "love-button");
	g_signal_connect(row->love, "clicked", G_CALLBACK(toggle_love), row);
	gtk_box_append(GTK_BOX(row->box), row->love);
	row->like = create_row_action("crimson-like-symbolic", "like-button");
	g_signal_connect(row->like, "clicked", G_CALLBACK(toggle_like), row);
	gtk_box_append(GTK_BOX(row->box), row->like);
	row->dislike = create_row_action("crimson-dislike-symbolic", "like-button");
	g_signal_connect(row->dislike, "clicked", G_CALLBACK(toggle_dislike), row);
	gtk_box_append(GTK_BOX(row->box), row->dislike);
	row->add = create_row_action("list-add-symbolic", NULL);
	gtk_widget_set_tooltip_text(row->add, TEXT_ADD_SUGGESTION);
	g_signal_connect(row->add, "clicked", G_CALLBACK(add_suggestion), row);
	gtk_box_append(GTK_BOX(row->box), row->add);
	row->length = create_label("dimmed");
	gtk_widget_add_css_class(row->length, "numeric");
	/* Room for lengths up to 99:59:59, so the hearts beside them line up down the list */
	gtk_label_set_width_chars(GTK_LABEL(row->length), LENGTH_CHARACTERS);
	gtk_label_set_xalign(GTK_LABEL(row->length), 1);
	gtk_box_append(GTK_BOX(row->box), row->length);
	add_row_controllers(row);
	g_object_set_data_full(G_OBJECT(item), "row", row, g_free);
	/* Rows open and fold through a revealer, so songs coming and going push the rows below smoothly */
	row->revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(row->revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_child(GTK_REVEALER(row->revealer), row->box);
	gtk_list_item_set_child(item, row->revealer);
}


static void bind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	SongRow *row = g_object_get_data(G_OBJECT(item), "row");
	CrimsonSong *song = gtk_list_item_get_item(item);
	gtk_label_set_text(GTK_LABEL(row->title), song->title);
	g_autofree char *details = *song->album ? g_strdup_printf("%s · %s", song->artist, song->album)
		: g_strdup(song->artist);
	gtk_label_set_text(GTK_LABEL(row->details), details);
	gtk_widget_set_visible(row->details, *details != '\0');
	gtk_label_set_text(GTK_LABEL(row->length), song->duration);
	show_artwork(row->artwork, song->artwork);
	gtk_widget_set_visible(row->add, is_suggested_song(song));
	gint64 *arrived = g_object_get_data(G_OBJECT(song), ARRIVING_KEY);
	gboolean is_arriving = arrived && g_get_monotonic_time() - *arrived < ARRIVAL_MICROSECONDS;
	g_object_set_data(G_OBJECT(song), ARRIVING_KEY, NULL);
	gtk_revealer_set_transition_duration(GTK_REVEALER(row->revealer), 0);
	gtk_revealer_set_reveal_child(GTK_REVEALER(row->revealer), !is_arriving);
	gtk_revealer_set_transition_duration(GTK_REVEALER(row->revealer), FOLD_MILLISECONDS);
	if (is_arriving)
		gtk_revealer_set_reveal_child(GTK_REVEALER(row->revealer), TRUE);
	if (!bound_rows)
		bound_rows = g_ptr_array_new();
	g_ptr_array_add(bound_rows, row);
	show_playing(row);
	show_liked(row);
	show_disliked(row);
	show_loved(row);
}


static void unbind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	g_ptr_array_remove_fast(bound_rows, g_object_get_data(G_OBJECT(item), "row"));
}


/* A list closed while its rows are bound must not leave them behind for the playing mark */
static void tear_down_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
	if (bound_rows)
		g_ptr_array_remove_fast(bound_rows, g_object_get_data(G_OBJECT(item), "row"));
}


void mark_song_arriving(CrimsonSong *song) {
	gint64 *now = g_new(gint64, 1);
	*now = g_get_monotonic_time();
	g_object_set_data_full(G_OBJECT(song), ARRIVING_KEY, now, g_free);
}


typedef struct {
	CrimsonSong *song;
	SongFolded done;
	gpointer data;
	gulong handler;
} Fold;


static void finish_fold(Fold *fold) {
	fold->done(fold->song, fold->data);
	g_object_unref(fold->song);
	g_free(fold);
}


static void notice_folded(GtkRevealer *revealer, GParamSpec *spec, Fold *fold) {
	if (gtk_revealer_get_child_revealed(revealer))
		return;
	g_signal_handler_disconnect(revealer, fold->handler);
	finish_fold(fold);
}


void fold_song_away(CrimsonSong *song, SongFolded done, gpointer data) {
	Fold *fold = g_new0(Fold, 1);
	fold->song = g_object_ref(song);
	fold->done = done;
	fold->data = data;
	for (guint index = 0; bound_rows && index < bound_rows->len; index++) {
		SongRow *row = g_ptr_array_index(bound_rows, index);
		if (gtk_list_item_get_item(row->item) != (gpointer)song || !gtk_widget_get_mapped(row->revealer))
			continue;
		fold->handler = g_signal_connect(row->revealer, "notify::child-revealed", G_CALLBACK(notice_folded), fold);
		gtk_revealer_set_reveal_child(GTK_REVEALER(row->revealer), FALSE);
		return;
	}
	finish_fold(fold);
}


/* Every section of the list gets a header, and only the suggestions show theirs */
static void set_up_header(GtkSignalListItemFactory *factory, GtkListHeader *header, gpointer data) {
	GtkWidget *label = gtk_label_new(TEXT_SUGGESTIONS);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "title-4");
	gtk_widget_add_css_class(label, "suggestions-heading");
	gtk_list_header_set_child(header, label);
}


static void bind_header(GtkSignalListItemFactory *factory, GtkListHeader *header, gpointer data) {
	gtk_widget_set_visible(gtk_list_header_get_child(header), is_suggested_song(gtk_list_header_get_item(header)));
}


static void free_setup(gpointer data) {
	SongListSetup *setup = data;
	g_clear_object(&setup->songs);
	g_clear_object(&setup->suggestions);
	g_clear_object(&setup->parts);
	g_free(setup);
}


GtkWidget *create_song_list(const SongListEvents *events, gpointer data) {
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	GtkWidget *list = gtk_list_view_new(NULL, factory);
	SongListSetup *setup = g_new0(SongListSetup, 1);
	*setup = (SongListSetup){events, data, FALSE, NULL, NULL, NULL};
	g_object_set_data_full(G_OBJECT(list), "setup", setup, free_setup);
	GtkListItemFactory *headers = gtk_signal_list_item_factory_new();
	g_signal_connect(headers, "setup", G_CALLBACK(set_up_header), NULL);
	g_signal_connect(headers, "bind", G_CALLBACK(bind_header), NULL);
	gtk_list_view_set_header_factory(GTK_LIST_VIEW(list), headers);
	g_object_unref(headers);
	g_signal_connect(factory, "setup", G_CALLBACK(set_up_row), list);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_row), NULL);
	g_signal_connect(factory, "unbind", G_CALLBACK(unbind_row), NULL);
	g_signal_connect(factory, "teardown", G_CALLBACK(tear_down_row), NULL);
	gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(list), TRUE);
	gtk_widget_add_css_class(list, "song-list");
	return list;
}


typedef struct {
	GtkAdjustment *adjustment;
	double value;
	gint64 end;
} ScrollHold;


static gboolean keep_scroll_position(GtkWidget *list, GdkFrameClock *clock, gpointer data) {
	ScrollHold *hold = data;
	gtk_adjustment_set_value(hold->adjustment, hold->value);
	return gdk_frame_clock_get_frame_time(clock) < hold->end ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}


static void free_scroll_hold(gpointer data) {
	ScrollHold *hold = data;
	g_object_unref(hold->adjustment);
	g_free(hold);
}


/* The list view moves its view when a section is added after the songs, so the view is held where it was while the
 * new rows open */
static void hold_scroll_position(GtkWidget *list) {
	GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(list));
	if (!adjustment)
		return;
	ScrollHold *hold = g_new(ScrollHold, 1);
	hold->adjustment = g_object_ref(adjustment);
	hold->value = gtk_adjustment_get_value(adjustment);
	hold->end = g_get_monotonic_time() + (FOLD_MILLISECONDS + 100) * 1000;
	gtk_widget_add_tick_callback(list, keep_scroll_position, hold, free_scroll_hold);
}


/* The list's own songs and the suggestions after them are the two sections of one model */
static void show_list_model(GtkWidget *list, SongListSetup *setup) {
	g_clear_object(&setup->parts);
	setup->parts = g_list_store_new(G_TYPE_LIST_MODEL);
	g_list_store_append(setup->parts, setup->songs);
	if (setup->suggestions)
		g_list_store_append(setup->parts, setup->suggestions);
	g_autoptr(GtkNoSelection) selection = gtk_no_selection_new(G_LIST_MODEL(gtk_flatten_list_model_new(
		G_LIST_MODEL(g_object_ref(setup->parts)))));
	gtk_list_view_set_model(GTK_LIST_VIEW(list), GTK_SELECTION_MODEL(selection));
}


/* Songs added to a list hold the view too, so an added song opens without moving the rows on screen */
static void hold_for_added(GListModel *songs, guint position, guint removed, guint added, GtkWidget *list) {
	gboolean was_empty = g_list_model_get_n_items(songs) + removed == added;
	if (added > removed && !was_empty)
		hold_scroll_position(list);
}


void set_song_list_songs(GtkWidget *list, GListModel *songs, gboolean is_reorderable) {
	SongListSetup *setup = g_object_get_data(G_OBJECT(list), "setup");
	setup->is_reorderable = is_reorderable;
	if (setup->songs)
		g_signal_handlers_disconnect_by_data(setup->songs, list);
	g_set_object(&setup->songs, songs);
	g_signal_connect_object(songs, "items-changed", G_CALLBACK(hold_for_added), list, 0);
	show_list_model(list, setup);
}


void set_song_list_suggestions(GtkWidget *list, GListModel *suggestions) {
	SongListSetup *setup = g_object_get_data(G_OBJECT(list), "setup");
	if (setup->suggestions == suggestions)
		return;
	g_set_object(&setup->suggestions, suggestions);
	for (guint index = 0; suggestions && index < g_list_model_get_n_items(suggestions); index++) {
		g_autoptr(CrimsonSong) song = g_list_model_get_item(suggestions, index);
		g_object_set_data(G_OBJECT(song), SUGGESTED_KEY, GINT_TO_POINTER(TRUE));
		mark_song_arriving(song);
	}
	/* Only the suggestions section changes, so the songs above keep their place and the new rows slide open */
	if (!setup->parts)
		return;
	guint count = g_list_model_get_n_items(G_LIST_MODEL(setup->parts));
	gpointer added[] = {suggestions};
	g_list_store_splice(setup->parts, 1, count - 1, added, suggestions ? 1 : 0);
	hold_scroll_position(list);
}


void set_song_list_reorderable(GtkWidget *list, gboolean is_reorderable) {
	SongListSetup *setup = g_object_get_data(G_OBJECT(list), "setup");
	setup->is_reorderable = is_reorderable;
}

#include "../lang/en.h"
#include "ambience.h"
#include "artwork.h"
#include "motion.h"
#include "player-bar.h"

#define ARTWORK_SIZE 56
/* Where each artist's name keeps the artist's id and its click */
#define ARTIST_ID "artist-id"
#define ARTIST_CLICK "artist-click"
/* Reports arrive four times a second, so the slider glides across each gap instead of jumping */
#define GLIDE_MILLISECONDS 250
/* Buttons that come and go with the song take this long to slide in or out */
#define BUTTON_SLIDE_MILLISECONDS 250
/* Larger moves are seeks or new songs, which jump straight there */
#define GLIDE_LIMIT_SECONDS 2
/* One wheel step changes the volume by this much, out of 100 */
#define VOLUME_STEP 5
/* Room for times up to 999:59, so the slider never shifts as the numbers grow */
#define TIME_CHARACTERS 6
/* Reports from the page lag behind a drag, so they wait this long before moving the slider again */
#define SEEK_SETTLE_MICROSECONDS (700 * G_TIME_SPAN_MILLISECOND)
/* The volume bar that scrolling opens closes again after this long without another scroll */
#define VOLUME_POPUP_MILLISECONDS 1200
#define CHAPTER_LIST_HEIGHT 420
/* The next few covers beside the Next button, fanned like a hand of cards, each this far right of the one before */
#define UPCOMING_COVERS 3
#define UPCOMING_COVER 46
#define UPCOMING_STEP 14
#define UPCOMING_ROW_COVER 44
/* Space around the fan, so tilted and lifted cards are not cut off */
#define UPCOMING_ROOM 12
/* The fan keeps this far from the next button */
#define UPCOMING_GAP 20
/* One notch of a scroll wheel on the panel icon turns the volume this much */
#define VOLUME_STEP 5
#define CHAPTER_TITLE_CHARACTERS 40
#define CHAPTER_TITLE_MINIMUM 30
#define CHAPTER_MARKER_WIDTH 22
#define CONTROL_SPACING 6
/* The groups of buttons at the right of the bar keep this far apart, so each reads as its own */
#define GROUP_SPACING 18

struct PlayerBar {
	const PlayerBarEvents *events;
	gpointer data;
	GtkWidget *root, *ambience, *details, *artwork, *title, *artist, *play_stack, *position, *elapsed, *length;
	AdwAnimation *glide;
	/* The songs the fan showed last, to tell how it changed */
	GPtrArray *fan_ids;
	GtkWidget *upcoming, *upcoming_covers, *upcoming_queued, *upcoming_queued_heading, *upcoming_list;
	GtkWidget *sound, *like, *dislike, *love, *lyrics, *video, *comments, *chapters, *chapter_list, *chapter_heading;
	int current_chapter;
	GPtrArray *chapter_starts;
	double time;
	gint64 last_seek_time;
	double duration;
	/* The position is held down, so the slider follows the pointer and the song only moves on release */
	gboolean is_holding_position;
	gboolean is_updating;
	guint volume_popup_source;
	/* The song on show, which drags onto playlists like a song from a list */
	CrimsonSong *song;
	/* The title click, called off when a drag starts so it never waits for its release */
	GtkEventController *title_click;
};


static GtkWidget *create_action_button(const char *icon, const char *action, const char *tooltip) {
	GtkWidget *button = gtk_button_new_from_icon_name(icon);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_add_css_class(button, "circular");
	gtk_widget_add_css_class(button, "control");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	return button;
}


static char *format_time(double seconds) {
	return format_song_length(seconds);
}


static void show_time_labels(PlayerBar *bar, double time) {
	g_autofree char *elapsed = format_time(time);
	g_autofree char *length = format_time(bar->duration);
	gtk_label_set_text(GTK_LABEL(bar->elapsed), elapsed);
	gtk_label_set_text(GTK_LABEL(bar->length), length);
}


/* A click or drag moves the slider at once and stops any glide, which would otherwise pull it back */
static gboolean move_position(GtkRange *range, GtkScrollType scroll, double value, PlayerBar *bar) {
	adw_animation_reset(bar->glide);
	bar->last_seek_time = g_get_monotonic_time();
	show_time_labels(bar, CLAMP(value, 0, bar->duration));
	if (!bar->is_holding_position)
		bar->events->seek(value, bar->data);
	return FALSE;
}


/* Watches presses before the slider claims them, so a drag only seeks once the button is let go */
static gboolean notice_position_button(GtkEventControllerLegacy *controller, GdkEvent *event, PlayerBar *bar) {
	GdkEventType type = gdk_event_get_event_type(event);
	if (type == GDK_BUTTON_PRESS) {
		bar->is_holding_position = TRUE;
	} else if (type == GDK_BUTTON_RELEASE && bar->is_holding_position) {
		bar->is_holding_position = FALSE;
		bar->last_seek_time = g_get_monotonic_time();
		bar->events->seek(gtk_range_get_value(GTK_RANGE(bar->position)), bar->data);
	}
	return FALSE;
}


static void change_volume(GtkScaleButton *button, double value, PlayerBar *bar) {
	if (!bar->is_updating)
		bar->events->change_volume((int)value, bar->data);
}


static gboolean close_volume_popup(gpointer data) {
	PlayerBar *bar = data;
	bar->volume_popup_source = 0;
	GtkWidget *popup = gtk_scale_button_get_popup(GTK_SCALE_BUTTON(bar->sound));
	gtk_popover_popdown(GTK_POPOVER(popup));
	gtk_popover_set_autohide(GTK_POPOVER(popup), TRUE);
	return G_SOURCE_REMOVE;
}


/* Scrolling over the icon opens the volume bar without taking the pointer, so the scroll keeps changing it */
static gboolean show_volume_popup(GtkEventControllerScroll *controller, double x, double y, PlayerBar *bar) {
	GtkWidget *popup = gtk_scale_button_get_popup(GTK_SCALE_BUTTON(bar->sound));
	if (!gtk_widget_get_visible(popup)) {
		gtk_popover_set_autohide(GTK_POPOVER(popup), FALSE);
		gtk_popover_popup(GTK_POPOVER(popup));
	}
	if (bar->volume_popup_source)
		g_source_remove(bar->volume_popup_source);
	if (!gtk_popover_get_autohide(GTK_POPOVER(popup)))
		bar->volume_popup_source = g_timeout_add(VOLUME_POPUP_MILLISECONDS, close_volume_popup, bar);
	return FALSE;
}


static void click_title(GtkGestureClick *gesture, int presses, double x, double y, PlayerBar *bar) {
	bar->events->open_source(bar->data);
}


/* Each artist's name holds its id, which is empty for a song that names its artist only as text */
static void click_artist(GtkGestureClick *gesture, int presses, double x, double y, PlayerBar *bar) {
	GtkWidget *name = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	bar->events->open_artist(g_object_get_data(G_OBJECT(name), ARTIST_ID), bar->data);
}


/* Claims the press, since the bar is also a window handle that would otherwise open the window menu */
static void click_details(GtkGestureClick *gesture, int presses, double x, double y, PlayerBar *bar) {
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	bar->events->open_menu(bar->details, x, y, bar->data);
}


static GtkEventController *add_click(GtkWidget *widget, guint button, GCallback clicked, PlayerBar *bar) {
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), button);
	g_signal_connect(click, button == GDK_BUTTON_SECONDARY ? "pressed" : "released", clicked, bar);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));
	return GTK_EVENT_CONTROLLER(click);
}


static GdkContentProvider *start_song_drag(GtkDragSource *source, double x, double y, PlayerBar *bar) {
	if (!bar->song)
		return NULL;
	gtk_drag_source_set_icon(source, gtk_widget_paintable_new(bar->details), (int)x, (int)y);
	return gdk_content_provider_new_typed(CRIMSON_TYPE_SONG, bar->song);
}


static void begin_song_drag(GtkDragSource *source, GdkDrag *drag, PlayerBar *bar) {
	gtk_event_controller_reset(bar->title_click);
	for (GtkWidget *name = gtk_widget_get_first_child(bar->artist); name; name = gtk_widget_get_next_sibling(name)) {
		GtkEventController *click = g_object_get_data(G_OBJECT(name), ARTIST_CLICK);
		if (click)
			gtk_event_controller_reset(click);
	}
}


/* The names ask for no width, like the title, so the controls stay centred and the names fill what is left */
static void measure_artist_names(GtkWidget *line, GtkOrientation orientation, int for_size, int *minimum,
	int *natural, int *minimum_baseline, int *natural_baseline) {
	*minimum = *natural = 0;
	for (GtkWidget *name = gtk_widget_get_first_child(line); name; name = gtk_widget_get_next_sibling(name)) {
		int name_minimum, name_natural;
		gtk_widget_measure(name, orientation, -1, &name_minimum, &name_natural, NULL, NULL);
		if (orientation == GTK_ORIENTATION_VERTICAL) {
			*minimum = MAX(*minimum, name_minimum);
			*natural = MAX(*natural, name_natural);
		}
	}
}


static void open_information(GtkWidget *artwork) {
	gtk_widget_activate_action(artwork, "win.playing-information", NULL);
}


/* The names fill the line from the left at full width, and the one reaching the edge ends in an ellipsis,
 * so a narrow bar cuts the line like a single label instead of shortening every name */
static void place_artist_names(GtkWidget *line, int width, int height, int baseline) {
	int left = 0;
	for (GtkWidget *name = gtk_widget_get_first_child(line); name; name = gtk_widget_get_next_sibling(name)) {
		int minimum, natural;
		gtk_widget_measure(name, GTK_ORIENTATION_HORIZONTAL, height, &minimum, &natural, NULL, NULL);
		int room = MIN(natural, width - left);
		gboolean is_fitting = room >= minimum;
		gtk_widget_set_child_visible(name, is_fitting);
		if (is_fitting)
			gtk_widget_size_allocate(name, &(GtkAllocation){left, 0, room, height}, baseline);
		/* A comma stays only with a name after it */
		GtkWidget *before = gtk_widget_get_prev_sibling(name);
		if (!is_fitting && left < width && before && !g_object_get_data(G_OBJECT(before), ARTIST_ID))
			gtk_widget_set_child_visible(before, FALSE);
		left = is_fitting ? left + room : width;
	}
}


static GtkWidget *create_song_details(PlayerBar *bar) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	bar->details = box;
	gtk_widget_add_css_class(box, "song-details");
	bar->artwork = create_cover(ARTWORK_SIZE);
	gtk_widget_set_valign(bar->artwork, GTK_ALIGN_CENTER);
	gtk_widget_set_cursor_from_name(bar->artwork, "pointer");
	gtk_widget_set_tooltip_text(bar->artwork, TEXT_INFORMATION);
	GtkGesture *information = gtk_gesture_click_new();
	g_signal_connect_swapped(information, "released", G_CALLBACK(open_information), bar->artwork);
	gtk_widget_add_controller(bar->artwork, GTK_EVENT_CONTROLLER(information));
	gtk_box_append(GTK_BOX(box), bar->artwork);
	GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
	bar->title = gtk_label_new(NULL);
	gtk_widget_add_css_class(bar->title, "heading");
	gtk_label_set_xalign(GTK_LABEL(bar->title), 0);
	gtk_label_set_ellipsize(GTK_LABEL(bar->title), PANGO_ELLIPSIZE_END);
	/* Asking for almost no width keeps the controls centred, and the label still fills its side */
	gtk_label_set_max_width_chars(GTK_LABEL(bar->title), 1);
	gtk_widget_set_hexpand(bar->title, TRUE);
	gtk_box_append(GTK_BOX(labels), bar->title);
	/* The artists' names sit side by side, each one a link of its own */
	bar->artist = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(bar->artist, "dimmed");
	gtk_widget_set_layout_manager(bar->artist, gtk_custom_layout_new(NULL, measure_artist_names, place_artist_names));
	gtk_box_append(GTK_BOX(labels), bar->artist);
	gtk_widget_set_hexpand(labels, TRUE);
	gtk_box_append(GTK_BOX(box), labels);
	gtk_widget_add_css_class(bar->title, "link-label");
	gtk_widget_set_cursor_from_name(bar->title, "pointer");
	bar->title_click = add_click(bar->title, GDK_BUTTON_PRIMARY, G_CALLBACK(click_title), bar);
	add_click(box, GDK_BUTTON_SECONDARY, G_CALLBACK(click_details), bar);
	GtkDragSource *source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_COPY);
	g_signal_connect(source, "prepare", G_CALLBACK(start_song_drag), bar);
	g_signal_connect(source, "drag-begin", G_CALLBACK(begin_song_drag), bar);
	gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(source));
	return box;
}


/* The play button crossfades between play, pause and a spinner while a song is on its way */
/* Upcoming ================================================================= */


/* Rows carry how many places after the playing song they are, for jumping there or taking them out */
static void jump_to_upcoming(GtkListBox *list, GtkListBoxRow *row, PlayerBar *bar) {
	int offset = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(row)), "offset"));
	gtk_menu_button_popdown(GTK_MENU_BUTTON(bar->upcoming));
	gtk_widget_activate_action(bar->root, "win.jump-queue", "i", offset);
}


/* Queued songs drag by their place, and dropping one on another queued song moves it there */
static GdkContentProvider *start_queued_drag(GtkDragSource *source, double x, double y, PlayerBar *bar) {
	GtkWidget *box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
	gtk_drag_source_set_icon(source, gtk_widget_paintable_new(box), (int)x, (int)y);
	return gdk_content_provider_new_typed(G_TYPE_INT, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(box), "offset")));
}


static gboolean drop_queued(GtkDropTarget *target, const GValue *value, double x, double y, PlayerBar *bar) {
	GtkWidget *box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
	gtk_widget_remove_css_class(box, "drop-target");
	int from = g_value_get_int(value), to = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(box), "offset"));
	if (from == to)
		return FALSE;
	gtk_widget_activate_action(bar->root, "win.move-queued", "(ii)", from, to);
	return TRUE;
}


static GdkDragAction enter_queued(GtkDropTarget *target, double x, double y, PlayerBar *bar) {
	gtk_widget_add_css_class(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)), "drop-target");
	return GDK_ACTION_MOVE;
}


static void leave_queued(GtkDropTarget *target, PlayerBar *bar) {
	gtk_widget_remove_css_class(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)), "drop-target");
}


static void remove_queued(GtkButton *button, PlayerBar *bar) {
	int offset = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "offset"));
	gtk_widget_activate_action(bar->root, "win.remove-upcoming", "i", offset);
}


/* The song that plays next is chosen when the queue opens, so Enter plays it and the arrows move from it */
static void open_upcoming(GtkPopover *popover, PlayerBar *bar) {
	GtkWidget *list = gtk_widget_get_visible(bar->upcoming_queued) ? bar->upcoming_queued : bar->upcoming_list;
	GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0);
	if (!row)
		return;
	gtk_list_box_select_row(GTK_LIST_BOX(list), row);
	gtk_widget_grab_focus(GTK_WIDGET(row));
}


static GtkWidget *create_queue_list(PlayerBar *bar) {
	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
	gtk_widget_add_css_class(list, "chapter-list");
	g_signal_connect(list, "row-activated", G_CALLBACK(jump_to_upcoming), bar);
	return list;
}


static GtkWidget *create_queue_heading(const char *text) {
	GtkWidget *heading = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(heading), 0);
	gtk_widget_add_css_class(heading, "chapter-heading");
	return heading;
}


/* A song dropped on the fan is queued to play next, as Play Next does */
static gboolean drop_on_upcoming(GtkDropTarget *target, const GValue *value, double x, double y, PlayerBar *bar) {
	gtk_widget_remove_css_class(bar->upcoming, "drop-target");
	if (G_VALUE_HOLDS(value, CRIMSON_TYPE_SONG)) {
		bar->events->queue_song(CRIMSON_SONG(g_value_get_object(value)), bar->data);
		return TRUE;
	}
	Card *card = g_value_get_boxed(value);
	if (!card->video_id)
		return FALSE;
	g_autoptr(CrimsonSong) song = g_object_new(CRIMSON_TYPE_SONG, NULL);
	song->id = g_strdup(card->video_id);
	song->title = g_strdup(card->title);
	song->artist = g_strdup(card->subtitle ? card->subtitle : "");
	song->artist_id = g_strdup("");
	song->album = g_strdup("");
	song->album_id = g_strdup("");
	song->duration = g_strdup("");
	song->artwork = g_strdup(card->artwork ? card->artwork : "");
	song->set_id = g_strdup("");
	bar->events->queue_song(song, bar->data);
	return TRUE;
}


static GdkDragAction enter_upcoming(GtkDropTarget *target, double x, double y, PlayerBar *bar) {
	gtk_widget_add_css_class(bar->upcoming, "drop-target");
	return GDK_ACTION_COPY;
}


static void leave_upcoming(GtkDropTarget *target, PlayerBar *bar) {
	gtk_widget_remove_css_class(bar->upcoming, "drop-target");
}


/* The next few songs as covers fanned out from the Next button, the soonest in front, opening the queue */
static GtkWidget *create_upcoming(PlayerBar *bar) {
	bar->upcoming_covers = gtk_overlay_new();
	GtkWidget *room = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_size_request(room, UPCOMING_COVER + UPCOMING_STEP * (UPCOMING_COVERS - 1) + UPCOMING_ROOM,
		UPCOMING_COVER + UPCOMING_ROOM);
	gtk_overlay_set_child(GTK_OVERLAY(bar->upcoming_covers), room);
	bar->upcoming = gtk_menu_button_new();
	gtk_menu_button_set_child(GTK_MENU_BUTTON(bar->upcoming), bar->upcoming_covers);
	gtk_widget_set_tooltip_text(bar->upcoming, TEXT_UP_NEXT);
	gtk_widget_add_css_class(bar->upcoming, "flat");
	gtk_widget_add_css_class(bar->upcoming, "upcoming");
	gtk_widget_set_valign(bar->upcoming, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start(bar->upcoming, UPCOMING_GAP);
	/* An empty queue only stops drawing the covers, so the bar keeps one height and the buttons stay in place */
	gtk_widget_set_child_visible(bar->upcoming, FALSE);
	GtkDropTarget *target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
	GType types[] = {CRIMSON_TYPE_SONG, CRIMSON_TYPE_CARD};
	gtk_drop_target_set_gtypes(target, types, G_N_ELEMENTS(types));
	g_signal_connect(target, "drop", G_CALLBACK(drop_on_upcoming), bar);
	g_signal_connect(target, "enter", G_CALLBACK(enter_upcoming), bar);
	g_signal_connect(target, "leave", G_CALLBACK(leave_upcoming), bar);
	gtk_widget_add_controller(bar->upcoming, GTK_EVENT_CONTROLLER(target));
	bar->upcoming_queued = create_queue_list(bar);
	bar->upcoming_queued_heading = create_queue_heading(TEXT_QUEUED);
	bar->upcoming_list = create_queue_list(bar);
	GtkWidget *lists = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(lists), bar->upcoming_queued_heading);
	gtk_box_append(GTK_BOX(lists), bar->upcoming_queued);
	GtkWidget *divide = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_add_css_class(divide, "queue-divide");
	g_object_bind_property(bar->upcoming_queued, "visible", divide, "visible", G_BINDING_SYNC_CREATE);
	g_object_bind_property(bar->upcoming_queued, "visible", bar->upcoming_queued_heading, "visible",
		G_BINDING_SYNC_CREATE);
	gtk_box_append(GTK_BOX(lists), divide);
	gtk_box_append(GTK_BOX(lists), create_queue_heading(TEXT_UP_NEXT));
	gtk_box_append(GTK_BOX(lists), bar->upcoming_list);
	GtkWidget *scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scrolled), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), CHAPTER_LIST_HEIGHT);
	/* The scrollbar takes a strip of its own instead of covering the rows */
	gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scrolled), FALSE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), lists);
	GtkWidget *popover = gtk_popover_new();
	gtk_widget_add_css_class(popover, "chapters");
	gtk_popover_set_child(GTK_POPOVER(popover), scrolled);
	g_signal_connect(popover, "show", G_CALLBACK(open_upcoming), bar);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(bar->upcoming), popover);
	return bar->upcoming;
}


static GtkWidget *create_upcoming_row(PlayerBar *bar, CrimsonSong *song, int offset, gboolean is_queued) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_add_css_class(box, "chapter");
	g_object_set_data(G_OBJECT(box), "offset", GINT_TO_POINTER(offset));
	GtkWidget *cover = create_cover(UPCOMING_ROW_COVER);
	show_artwork(cover, song->artwork);
	gtk_box_append(GTK_BOX(box), cover);
	GtkWidget *words = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_hexpand(words, TRUE);
	gtk_widget_set_valign(words, GTK_ALIGN_CENTER);
	GtkWidget *title = gtk_label_new(song->title);
	GtkWidget *artist = gtk_label_new(song->artist);
	gtk_widget_add_css_class(artist, "dimmed");
	gtk_widget_add_css_class(artist, "caption");
	for (GtkWidget *label = title; label; label = label == title ? artist : NULL) {
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_label_set_max_width_chars(GTK_LABEL(label), CHAPTER_TITLE_CHARACTERS);
		gtk_label_set_width_chars(GTK_LABEL(label), CHAPTER_TITLE_MINIMUM);
		gtk_box_append(GTK_BOX(words), label);
	}
	gtk_box_append(GTK_BOX(box), words);
	GtkWidget *length = gtk_label_new(song->duration);
	gtk_widget_add_css_class(length, "chapter-time");
	gtk_widget_set_valign(length, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), length);
	if (is_queued) {
		GtkWidget *remove = gtk_button_new_from_icon_name("window-close-symbolic");
		gtk_widget_set_tooltip_text(remove, TEXT_REMOVE_FROM_QUEUE);
		gtk_widget_add_css_class(remove, "flat");
		gtk_widget_add_css_class(remove, "circular");
		gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
		g_object_set_data(G_OBJECT(remove), "offset", GINT_TO_POINTER(offset));
		g_signal_connect(remove, "clicked", G_CALLBACK(remove_queued), bar);
		gtk_box_append(GTK_BOX(box), remove);
		GtkDragSource *source = gtk_drag_source_new();
		gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
		g_signal_connect(source, "prepare", G_CALLBACK(start_queued_drag), bar);
		gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(source));
		GtkDropTarget *target = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE);
		g_signal_connect(target, "drop", G_CALLBACK(drop_queued), bar);
		g_signal_connect(target, "enter", G_CALLBACK(enter_queued), bar);
		g_signal_connect(target, "leave", G_CALLBACK(leave_queued), bar);
		gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(target));
	}
	return box;
}


/* How the fan moves to its new cards: forward when the front song started playing, a card dealt on when a song
 * was queued in front, a fresh fan otherwise, and not at all when nothing changed */
static const char *pick_fan_motion(PlayerBar *bar, GPtrArray *songs) {
	if (!bar->fan_ids)
		bar->fan_ids = g_ptr_array_new_with_free_func(g_free);
	const char *new_front = songs->len ? ((CrimsonSong *)g_ptr_array_index(songs, 0))->id : NULL;
	const char *new_second = songs->len > 1 ? ((CrimsonSong *)g_ptr_array_index(songs, 1))->id : NULL;
	const char *old_front = bar->fan_ids->len ? g_ptr_array_index(bar->fan_ids, 0) : NULL;
	const char *old_second = bar->fan_ids->len > 1 ? g_ptr_array_index(bar->fan_ids, 1) : NULL;
	if (!new_front || g_strcmp0(new_front, old_front) == 0)
		return NULL;
	if (old_second && g_strcmp0(new_front, old_second) == 0)
		return "fan-forward";
	if (old_front && g_strcmp0(new_second, old_front) == 0)
		return "fan-deal";
	return "fan-fresh";
}


void show_player_upcoming(PlayerBar *bar, GPtrArray *songs, guint queued_count) {
	GtkWidget *child = gtk_widget_get_first_child(bar->upcoming_covers);
	while (child) {
		GtkWidget *next = gtk_widget_get_next_sibling(child);
		if (child != gtk_overlay_get_child(GTK_OVERLAY(bar->upcoming_covers)))
			gtk_overlay_remove_overlay(GTK_OVERLAY(bar->upcoming_covers), child);
		child = next;
	}
	guint shown = MIN(songs->len, UPCOMING_COVERS);
	const char *motion = pick_fan_motion(bar, songs);
	for (guint index = shown; index-- > 0;) {
		CrimsonSong *song = g_ptr_array_index(songs, index);
		GtkWidget *cover = create_cover(UPCOMING_COVER);
		show_artwork(cover, song->artwork);
		gtk_widget_add_css_class(cover, "upcoming-cover");
		gtk_widget_add_css_class(cover, "tilted");
		g_autofree char *place = g_strdup_printf("card-%u", index);
		gtk_widget_add_css_class(cover, place);
		/* The holder moves while the cover inside keeps its tilt */
		GtkWidget *holder = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_box_append(GTK_BOX(holder), cover);
		gtk_widget_set_halign(holder, GTK_ALIGN_START);
		gtk_widget_set_valign(holder, GTK_ALIGN_CENTER);
		gtk_widget_set_margin_start(holder, (int)(UPCOMING_STEP * index));
		if (motion) {
			gtk_widget_add_css_class(holder, motion);
			g_autofree char *delay = g_strdup_printf("fan-delay-%u", index);
			gtk_widget_add_css_class(holder, delay);
			replay_entrance(holder);
		}
		gtk_overlay_add_overlay(GTK_OVERLAY(bar->upcoming_covers), holder);
	}
	g_ptr_array_set_size(bar->fan_ids, 0);
	for (guint index = 0; index < shown; index++)
		g_ptr_array_add(bar->fan_ids, g_strdup(((CrimsonSong *)g_ptr_array_index(songs, index))->id));
	/* How many songs the listener queued with Play Next, which the badge counts */
	if (queued_count) {
		g_autofree char *more = g_strdup_printf("%u", queued_count);
		GtkWidget *badge = gtk_label_new(more);
		gtk_widget_add_css_class(badge, "upcoming-count");
		gtk_widget_set_halign(badge, GTK_ALIGN_END);
		gtk_widget_set_valign(badge, GTK_ALIGN_END);
		gtk_overlay_add_overlay(GTK_OVERLAY(bar->upcoming_covers), badge);
	}
	gtk_list_box_remove_all(GTK_LIST_BOX(bar->upcoming_queued));
	gtk_list_box_remove_all(GTK_LIST_BOX(bar->upcoming_list));
	for (guint index = 0; index < songs->len; index++) {
		gboolean is_queued = index < queued_count;
		gtk_list_box_append(GTK_LIST_BOX(is_queued ? bar->upcoming_queued : bar->upcoming_list),
			create_upcoming_row(bar, g_ptr_array_index(songs, index), (int)index + 1, is_queued));
	}
	gtk_widget_set_visible(bar->upcoming_queued, queued_count > 0);
	gtk_widget_set_child_visible(bar->upcoming, songs->len > 0);
}


static GtkWidget *create_shuffle_button(void) {
	GtkWidget *shuffle = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(shuffle), "media-playlist-shuffle-symbolic");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(shuffle), "win.shuffle");
	gtk_widget_set_tooltip_text(shuffle, TEXT_SHUFFLE);
	gtk_widget_add_css_class(shuffle, "flat");
	gtk_widget_add_css_class(shuffle, "circular");
	gtk_widget_add_css_class(shuffle, "control");
	gtk_widget_set_valign(shuffle, GTK_ALIGN_CENTER);
	return shuffle;
}


static GtkWidget *create_controls(PlayerBar *bar) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, CONTROL_SPACING);
	/* Both sides of the play button take the same width, so it stays in the middle with Shuffle and the upcoming
	 * covers on its right */
	GtkWidget *before = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, CONTROL_SPACING);
	GtkWidget *after = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, CONTROL_SPACING);
	g_autoptr(GtkSizeGroup) sizes = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
	gtk_size_group_add_widget(sizes, before);
	gtk_size_group_add_widget(sizes, after);
	GtkWidget *previous = create_action_button("media-skip-backward-symbolic", "win.previous", TEXT_PREVIOUS);
	/* Previous sits against the play button, and its stretch stays inside its side rather than widening the row */
	gtk_widget_set_hexpand(previous, TRUE);
	gtk_widget_set_halign(previous, GTK_ALIGN_END);
	gtk_widget_set_hexpand(before, FALSE);
	gtk_box_append(GTK_BOX(before), previous);
	gtk_box_append(GTK_BOX(box), before);
	GtkWidget *play = create_action_button("media-playback-start-symbolic", "win.play-pause", TEXT_PLAY_PAUSE);
	gtk_widget_remove_css_class(play, "flat");
	gtk_widget_add_css_class(play, "suggested-action");
	gtk_widget_add_css_class(play, "play-button");
	bar->play_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(bar->play_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(bar->play_stack), gtk_image_new_from_icon_name("media-playback-start-symbolic"),
		"play");
	gtk_stack_add_named(GTK_STACK(bar->play_stack), gtk_image_new_from_icon_name("media-playback-pause-symbolic"),
		"pause");
	gtk_stack_add_named(GTK_STACK(bar->play_stack), adw_spinner_new(), "spinner");
	gtk_button_set_child(GTK_BUTTON(play), bar->play_stack);
	gtk_box_append(GTK_BOX(box), play);
	gtk_box_append(GTK_BOX(after), create_action_button("media-skip-forward-symbolic", "win.next", TEXT_NEXT));
	gtk_box_append(GTK_BOX(after), create_shuffle_button());
	gtk_box_append(GTK_BOX(after), create_upcoming(bar));
	gtk_box_append(GTK_BOX(box), after);
	return box;
}


static void click_love(GtkButton *button, PlayerBar *bar) {
	bar->events->toggle_love(bar->data);
}


static void click_like(GtkButton *button, PlayerBar *bar) {
	bar->events->toggle_like(bar->data);
}


static void click_dislike(GtkButton *button, PlayerBar *bar) {
	bar->events->toggle_dislike(bar->data);
}


static void open_chapter(GtkListBox *list, GtkListBoxRow *row, PlayerBar *bar) {
	int start = GPOINTER_TO_INT(g_ptr_array_index(bar->chapter_starts, (guint)gtk_list_box_row_get_index(row)));
	bar->events->seek(start, bar->data);
}


/* The chapter playing now shows the playing icon in place of its number, and follows the song while the list is
 * open */
static void mark_current_chapter(PlayerBar *bar) {
	int current = -1;
	for (guint index = 0; bar->chapter_starts && index < bar->chapter_starts->len; index++) {
		if (GPOINTER_TO_INT(g_ptr_array_index(bar->chapter_starts, index)) <= bar->time)
			current = (int)index;
	}
	if (current == bar->current_chapter)
		return;
	bar->current_chapter = current;
	for (int index = 0;; index++) {
		GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(bar->chapter_list), index);
		if (!row)
			break;
		GtkWidget *box = gtk_list_box_row_get_child(row);
		gtk_stack_set_visible_child_name(GTK_STACK(g_object_get_data(G_OBJECT(box), "marker")),
			index == current ? "playing" : "number");
		if (index == current)
			gtk_widget_add_css_class(GTK_WIDGET(row), "current");
		else
			gtk_widget_remove_css_class(GTK_WIDGET(row), "current");
	}
}


static void open_chapters(GtkPopover *popover, PlayerBar *bar) {
	bar->current_chapter = -2;
	mark_current_chapter(bar);
	GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(bar->chapter_list), MAX(bar->current_chapter, 0));
	if (row)
		gtk_widget_grab_focus(GTK_WIDGET(row));
}


static GtkWidget *create_chapters(PlayerBar *bar) {
	bar->chapters = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(bar->chapters), "view-list-bullet-symbolic");
	gtk_widget_set_tooltip_text(bar->chapters, TEXT_CHAPTERS);
	gtk_widget_add_css_class(bar->chapters, "flat");
	gtk_widget_add_css_class(bar->chapters, "control");
	gtk_widget_set_valign(bar->chapters, GTK_ALIGN_CENTER);
	gtk_widget_set_visible(bar->chapters, FALSE);
	bar->chapter_list = gtk_list_box_new();
	gtk_widget_add_css_class(bar->chapter_list, "chapter-list");
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(bar->chapter_list), GTK_SELECTION_NONE);
	g_signal_connect(bar->chapter_list, "row-activated", G_CALLBACK(open_chapter), bar);
	GtkWidget *scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scrolled), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), CHAPTER_LIST_HEIGHT);
	/* The scrollbar takes a strip of its own instead of covering the rows */
	gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scrolled), FALSE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), bar->chapter_list);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	bar->chapter_heading = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(bar->chapter_heading), 0);
	gtk_widget_add_css_class(bar->chapter_heading, "chapter-heading");
	gtk_box_append(GTK_BOX(box), bar->chapter_heading);
	gtk_box_append(GTK_BOX(box), scrolled);
	GtkWidget *popover = gtk_popover_new();
	gtk_widget_add_css_class(popover, "chapters");
	gtk_popover_set_child(GTK_POPOVER(popover), box);
	g_signal_connect(popover, "show", G_CALLBACK(open_chapters), bar);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(bar->chapters), popover);
	return bar->chapters;
}


static GtkWidget *create_chapter_row(Chapter *chapter, guint number) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_add_css_class(box, "chapter");
	GtkWidget *marker = gtk_stack_new();
	g_autofree char *count = g_strdup_printf("%u", number);
	GtkWidget *number_label = gtk_label_new(count);
	gtk_widget_add_css_class(number_label, "chapter-number");
	gtk_stack_add_named(GTK_STACK(marker), number_label, "number");
	GtkWidget *playing = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
	gtk_widget_add_css_class(playing, "chapter-playing");
	gtk_stack_add_named(GTK_STACK(marker), playing, "playing");
	gtk_widget_set_size_request(marker, CHAPTER_MARKER_WIDTH, -1);
	gtk_widget_set_valign(marker, GTK_ALIGN_CENTER);
	g_object_set_data(G_OBJECT(box), "marker", marker);
	gtk_box_append(GTK_BOX(box), marker);
	GtkWidget *title = gtk_label_new(chapter->title);
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	gtk_label_set_wrap(GTK_LABEL(title), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(title), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_lines(GTK_LABEL(title), 2);
	gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(title), CHAPTER_TITLE_CHARACTERS);
	gtk_label_set_width_chars(GTK_LABEL(title), CHAPTER_TITLE_MINIMUM);
	gtk_widget_add_css_class(title, "chapter-title");
	gtk_widget_set_hexpand(title, TRUE);
	gtk_box_append(GTK_BOX(box), title);
	g_autofree char *start = format_time(chapter->start);
	GtkWidget *time = gtk_label_new(start);
	gtk_widget_add_css_class(time, "chapter-time");
	gtk_widget_set_valign(time, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), time);
	return box;
}


/* A button comes in two steps, the bar first making room for it sideways and the button then fading into that room,
 * and goes the opposite way, so it never shows cut off by the buttons beside it */
static void finish_button_room(GtkRevealer *room) {
	GtkRevealer *drop = GTK_REVEALER(gtk_revealer_get_child(room));
	if (gtk_revealer_get_child_revealed(room) && gtk_revealer_get_reveal_child(room))
		gtk_revealer_set_reveal_child(drop, TRUE);
	else if (!gtk_revealer_get_reveal_child(room))
		gtk_widget_set_visible(GTK_WIDGET(room), FALSE);
}


/* The room closes only once the button has faded out of it */
static void finish_button_drop(GtkRevealer *drop) {
	GtkRevealer *room = GTK_REVEALER(gtk_widget_get_parent(GTK_WIDGET(drop)));
	if (!gtk_revealer_get_reveal_child(drop))
		gtk_revealer_set_reveal_child(room, FALSE);
}


static GtkWidget *create_button_slide(GtkWidget *button) {
	GtkWidget *drop = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(drop), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
	gtk_revealer_set_transition_duration(GTK_REVEALER(drop), BUTTON_SLIDE_MILLISECONDS);
	gtk_revealer_set_child(GTK_REVEALER(drop), button);
	gtk_widget_set_valign(drop, GTK_ALIGN_CENTER);
	g_signal_connect(drop, "notify::child-revealed", G_CALLBACK(finish_button_drop), NULL);
	GtkWidget *room = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(room), GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
	gtk_revealer_set_transition_duration(GTK_REVEALER(room), BUTTON_SLIDE_MILLISECONDS);
	gtk_revealer_set_child(GTK_REVEALER(room), drop);
	gtk_widget_set_visible(room, FALSE);
	g_signal_connect(room, "notify::child-revealed", G_CALLBACK(finish_button_room), NULL);
	return room;
}


static void slide_button(GtkWidget *button, gboolean is_shown) {
	GtkRevealer *drop = GTK_REVEALER(gtk_widget_get_parent(button));
	GtkRevealer *room = GTK_REVEALER(gtk_widget_get_parent(GTK_WIDGET(drop)));
	/* A room still closing counts as revealed, so only a room that stays open takes the button straight away */
	if (is_shown) {
		gtk_widget_set_visible(GTK_WIDGET(room), TRUE);
		if (gtk_revealer_get_reveal_child(room) && gtk_revealer_get_child_revealed(room))
			gtk_revealer_set_reveal_child(drop, TRUE);
		else
			gtk_revealer_set_reveal_child(room, TRUE);
	} else if (gtk_revealer_get_child_revealed(drop) || gtk_revealer_get_reveal_child(drop)) {
		gtk_revealer_set_reveal_child(drop, FALSE);
	} else {
		gtk_revealer_set_reveal_child(room, FALSE);
	}
}


void show_player_chapters(PlayerBar *bar, GPtrArray *chapters) {
	gtk_list_box_remove_all(GTK_LIST_BOX(bar->chapter_list));
	g_clear_pointer(&bar->chapter_starts, g_ptr_array_unref);
	bar->chapter_starts = g_ptr_array_new();
	bar->current_chapter = -2;
	guint count = chapters ? chapters->len : 0;
	for (guint index = 0; index < count; index++) {
		Chapter *chapter = g_ptr_array_index(chapters, index);
		g_ptr_array_add(bar->chapter_starts, GINT_TO_POINTER(chapter->start));
		gtk_list_box_append(GTK_LIST_BOX(bar->chapter_list), create_chapter_row(chapter, index + 1));
	}
	g_autofree char *heading = g_strdup_printf(TEXT_CHAPTER_COUNT, count);
	gtk_label_set_text(GTK_LABEL(bar->chapter_heading), heading);
	gtk_widget_set_visible(bar->chapters, count > 0);
}


static GtkWidget *create_toggle_slide(GtkWidget **button, const char *icon, const char *action, const char *tooltip) {
	*button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(*button), icon);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(*button), action);
	gtk_widget_set_tooltip_text(*button, tooltip);
	gtk_widget_add_css_class(*button, "flat");
	gtk_widget_add_css_class(*button, "circular");
	gtk_widget_add_css_class(*button, "control");
	gtk_widget_set_valign(*button, GTK_ALIGN_CENTER);
	return create_button_slide(*button);
}


static GtkWidget *create_rating_button(const char *icon, const char *tooltip, GCallback clicked, PlayerBar *bar) {
	GtkWidget *button = gtk_button_new_from_icon_name(icon);
	if (tooltip)
		gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_add_css_class(button, "control");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	g_signal_connect(button, "clicked", clicked, bar);
	return button;
}


/* Comments, video, lyrics and chapters, then love, like and dislike, then the volume, each group apart */
static GtkWidget *create_extras(PlayerBar *bar, int volume) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, GROUP_SPACING);
	GtkWidget *views = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, CONTROL_SPACING);
	gtk_box_append(GTK_BOX(views),
		create_toggle_slide(&bar->comments, "crimson-comments-symbolic", "win.comments", TEXT_COMMENTS));
	gtk_box_append(GTK_BOX(views),
		create_toggle_slide(&bar->video, "crimson-video-symbolic", "win.video", TEXT_MUSIC_VIDEO));
	gtk_box_append(GTK_BOX(views),
		create_toggle_slide(&bar->lyrics, "audio-input-microphone-symbolic", "win.lyrics", TEXT_LYRICS));
	gtk_box_append(GTK_BOX(views), create_chapters(bar));
	gtk_box_append(GTK_BOX(box), views);
	GtkWidget *ratings = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, CONTROL_SPACING);
	bar->love = create_rating_button("crimson-heart-symbolic", NULL, G_CALLBACK(click_love), bar);
	gtk_widget_set_visible(bar->love, FALSE);
	gtk_box_append(GTK_BOX(ratings), bar->love);
	bar->like = create_rating_button("crimson-like-symbolic", TEXT_LIKE, G_CALLBACK(click_like), bar);
	gtk_box_append(GTK_BOX(ratings), bar->like);
	bar->dislike = create_rating_button("crimson-dislike-symbolic", TEXT_DISLIKE, G_CALLBACK(click_dislike), bar);
	gtk_box_append(GTK_BOX(ratings), bar->dislike);
	gtk_box_append(GTK_BOX(box), ratings);
	static const char *VOLUME_ICONS[] = {"audio-volume-muted-symbolic", "audio-volume-high-symbolic",
		"audio-volume-low-symbolic", "audio-volume-medium-symbolic", NULL};
	GtkWidget *sound = gtk_scale_button_new(0, 100, VOLUME_STEP, VOLUME_ICONS);
	/* Scrolling moves by the page step, which GTK makes ten times the small one unless told otherwise */
	gtk_adjustment_set_page_increment(gtk_scale_button_get_adjustment(GTK_SCALE_BUTTON(sound)), VOLUME_STEP);
	gtk_widget_set_tooltip_text(sound, TEXT_VOLUME);
	gtk_widget_add_css_class(sound, "flat");
	gtk_widget_add_css_class(sound, "control");
	gtk_widget_set_valign(sound, GTK_ALIGN_CENTER);
	bar->is_updating = TRUE;
	gtk_scale_button_set_value(GTK_SCALE_BUTTON(sound), volume);
	bar->is_updating = FALSE;
	g_signal_connect(sound, "value-changed", G_CALLBACK(change_volume), bar);
	bar->sound = sound;
	GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(show_volume_popup), bar);
	gtk_widget_add_controller(sound, scroll);
	gtk_box_append(GTK_BOX(box), sound);
	return box;
}


static GtkWidget *create_position(PlayerBar *bar) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	bar->elapsed = gtk_label_new("0:00");
	bar->length = gtk_label_new("0:00");
	bar->position = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
	gtk_widget_set_hexpand(bar->position, TRUE);
	gtk_accessible_update_property(GTK_ACCESSIBLE(bar->position), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_POSITION, -1);
	g_signal_connect(bar->position, "change-value", G_CALLBACK(move_position), bar);
	GtkEventController *buttons = gtk_event_controller_legacy_new();
	gtk_event_controller_set_propagation_phase(buttons, GTK_PHASE_CAPTURE);
	g_signal_connect(buttons, "event", G_CALLBACK(notice_position_button), bar);
	gtk_widget_add_controller(bar->position, buttons);
	GtkAdjustment *adjustment = gtk_range_get_adjustment(GTK_RANGE(bar->position));
	AdwAnimationTarget *target = adw_property_animation_target_new(G_OBJECT(adjustment), "value");
	bar->glide = adw_timed_animation_new(bar->position, 0, 0, GLIDE_MILLISECONDS, target);
	adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(bar->glide), ADW_LINEAR);
	for (GtkWidget *label = bar->elapsed; label; label = label == bar->elapsed ? bar->length : NULL) {
		gtk_widget_add_css_class(label, "numeric");
		gtk_widget_add_css_class(label, "dimmed");
		gtk_label_set_width_chars(GTK_LABEL(label), TIME_CHARACTERS);
	}
	gtk_label_set_xalign(GTK_LABEL(bar->elapsed), 1);
	gtk_label_set_xalign(GTK_LABEL(bar->length), 0);
	gtk_box_append(GTK_BOX(box), bar->elapsed);
	gtk_box_append(GTK_BOX(box), bar->position);
	gtk_box_append(GTK_BOX(box), bar->length);
	return box;
}


static void follow_cover(GtkWidget *cover, GParamSpec *spec, PlayerBar *bar) {
	set_ambience_texture(bar->ambience, get_cover_texture(cover));
}


PlayerBar *create_player_bar(const PlayerBarEvents *events, gpointer data, int volume) {
	PlayerBar *bar = g_new0(PlayerBar, 1);
	bar->events = events;
	bar->data = data;
	bar->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(bar->root, "player-bar");
	GtkWidget *row = gtk_center_box_new();
	GtkWidget *details = create_song_details(bar);
	gtk_widget_set_hexpand(details, TRUE);
	gtk_center_box_set_start_widget(GTK_CENTER_BOX(row), details);
	gtk_center_box_set_center_widget(GTK_CENTER_BOX(row), create_controls(bar));
	GtkWidget *extras = create_extras(bar, volume);
	gtk_widget_set_hexpand(extras, TRUE);
	gtk_widget_set_halign(extras, GTK_ALIGN_END);
	gtk_center_box_set_end_widget(GTK_CENTER_BOX(row), extras);
	gtk_box_append(GTK_BOX(bar->root), row);
	gtk_box_append(GTK_BOX(bar->root), create_position(bar));
	bar->ambience = create_ambience(bar->root);
	fade_ambience_edges(bar->ambience);
	g_signal_connect(bar->artwork, "notify::texture", G_CALLBACK(follow_cover), bar);
	return bar;
}


GtkWidget *get_player_bar_widget(PlayerBar *bar) {
	return bar->ambience;
}


/* One name of the artist line, as a link to that artist or as the comma between two names */
static GtkWidget *add_artist_name(PlayerBar *bar, const char *text, const char *artist_id) {
	GtkWidget *name = gtk_label_new(text);
	if (artist_id) {
		gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
		gtk_widget_add_css_class(name, "link-label");
		gtk_widget_set_cursor_from_name(name, "pointer");
		g_object_set_data_full(G_OBJECT(name), ARTIST_ID, g_strdup(artist_id), g_free);
		GtkEventController *click = add_click(name, GDK_BUTTON_PRIMARY, G_CALLBACK(click_artist), bar);
		g_object_set_data(G_OBJECT(name), ARTIST_CLICK, click);
	}
	gtk_box_append(GTK_BOX(bar->artist), name);
	return name;
}


/* Every linked artist's name leads to it, joined as the song's artist text joins them */
static void show_artist_names(PlayerBar *bar, CrimsonSong *song) {
	for (GtkWidget *name; (name = gtk_widget_get_first_child(bar->artist));)
		gtk_box_remove(GTK_BOX(bar->artist), name);
	g_auto(GStrv) artists = list_song_artists(song);
	for (guint index = 0; artists[index]; index += 2) {
		if (index)
			add_artist_name(bar, ", ", NULL);
		add_artist_name(bar, artists[index + 1], artists[index]);
	}
	if (!artists[0])
		add_artist_name(bar, song->artist, "");
}


void show_player_song(PlayerBar *bar, CrimsonSong *song) {
	g_set_object(&bar->song, song);
	gtk_label_set_text(GTK_LABEL(bar->title), song->title);
	show_artist_names(bar, song);
	gtk_widget_set_tooltip_text(bar->title, song->title);
	gtk_widget_set_tooltip_text(bar->artist, song->artist);
	show_artwork(bar->artwork, song->artwork);
	replay_entrance(bar->details);
	bar->duration = 0;
	show_player_time(bar, 0, read_song_length(song));
}


void show_player_time(PlayerBar *bar, double time, double duration) {
	/* The player knows no length until the video loads, so the listed one stays until then */
	if (duration <= 0)
		duration = bar->duration;
	bar->duration = duration;
	bar->time = time;
	if (gtk_menu_button_get_active(GTK_MENU_BUTTON(bar->chapters)))
		mark_current_chapter(bar);
	if (bar->is_holding_position || g_get_monotonic_time() - bar->last_seek_time < SEEK_SETTLE_MICROSECONDS)
		return;
	gtk_range_set_range(GTK_RANGE(bar->position), 0, MAX(duration, 1));
	double shown = gtk_range_get_value(GTK_RANGE(bar->position));
	adw_animation_reset(bar->glide);
	if (time > shown && time - shown < GLIDE_LIMIT_SECONDS) {
		adw_timed_animation_set_value_from(ADW_TIMED_ANIMATION(bar->glide), shown);
		adw_timed_animation_set_value_to(ADW_TIMED_ANIMATION(bar->glide), time);
		adw_animation_play(bar->glide);
	} else {
		gtk_range_set_value(GTK_RANGE(bar->position), time);
	}
	show_time_labels(bar, time);
}


void set_player_has_comments(PlayerBar *bar, gboolean has_comments) {
	slide_button(bar->comments, has_comments);
}


void set_player_has_video(PlayerBar *bar, gboolean has_video) {
	slide_button(bar->video, has_video);
}


void set_player_has_lyrics(PlayerBar *bar, gboolean has_lyrics) {
	slide_button(bar->lyrics, has_lyrics);
}


void set_player_likable(PlayerBar *bar, gboolean is_likable) {
	gtk_widget_set_visible(bar->like, is_likable);
	gtk_widget_set_visible(bar->dislike, is_likable);
}


void show_player_liked(PlayerBar *bar, gboolean is_liked) {
	gtk_button_set_icon_name(GTK_BUTTON(bar->like), is_liked ? "crimson-like-filled-symbolic" : "crimson-like-symbolic");
	gtk_widget_set_tooltip_text(bar->like, is_liked ? TEXT_UNLIKE : TEXT_LIKE);
	if (is_liked)
		gtk_widget_add_css_class(bar->like, "liked");
	else
		gtk_widget_remove_css_class(bar->like, "liked");
}


void show_player_disliked(PlayerBar *bar, gboolean is_disliked) {
	gtk_button_set_icon_name(GTK_BUTTON(bar->dislike),
		is_disliked ? "crimson-dislike-filled-symbolic" : "crimson-dislike-symbolic");
	gtk_widget_set_tooltip_text(bar->dislike, is_disliked ? TEXT_UNDISLIKE : TEXT_DISLIKE);
	if (is_disliked)
		gtk_widget_add_css_class(bar->dislike, "liked");
	else
		gtk_widget_remove_css_class(bar->dislike, "liked");
}


void show_player_playing(PlayerBar *bar, gboolean is_playing, gboolean is_waiting) {
	const char *shown = is_waiting ? "spinner" : is_playing ? "pause" : "play";
	gtk_stack_set_visible_child_name(GTK_STACK(bar->play_stack), shown);
	set_ambience_moving(bar->ambience, is_playing);
}


void set_player_seekable(PlayerBar *bar, gboolean is_seekable) {
	gtk_widget_set_can_target(bar->position, is_seekable);
}


void show_player_loved(PlayerBar *bar, gboolean is_shown, gboolean is_loved) {
	gtk_widget_set_visible(bar->love, is_shown);
	gtk_button_set_icon_name(GTK_BUTTON(bar->love), is_loved ? "crimson-heart-filled-symbolic" : "crimson-heart-symbolic");
	gtk_widget_set_tooltip_text(bar->love, is_loved ? TEXT_UNLOVE : TEXT_LOVE);
	if (is_loved)
		gtk_widget_add_css_class(bar->love, "loved");
	else
		gtk_widget_remove_css_class(bar->love, "loved");
}


void set_player_colored(PlayerBar *bar, gboolean is_colored) {
	set_ambience_plain(bar->ambience, !is_colored);
}


void step_player_volume(PlayerBar *bar, int steps) {
	double volume = gtk_scale_button_get_value(GTK_SCALE_BUTTON(bar->sound));
	gtk_scale_button_set_value(GTK_SCALE_BUTTON(bar->sound), CLAMP(volume + steps * VOLUME_STEP, 0, 100));
}

#include <string.h>
#include <adwaita.h>

#include "lang/en.h"
#include "visuals/ambience.h"
#include "visuals/artwork.h"
#include "common/widgets/loading.h"
#include "common/widgets/smooth-scroll.h"
#include "visuals/motion.h"
#include "pages/song-page.h"
#include "common/widgets/spacing.h"

#define SKELETON_WAVES 6
#define STAGGER_STEPS 10
#define SKELETON_ROWS 8
#define SKELETON_CARDS 6
#define SKELETON_COVER 40
#define SKELETON_LINE 12
#define SKELETON_HEADING 140
/* The heading shape stands a little taller than a line, and each row's second line is a shorter one two widths on */
#define SKELETON_HEADING_EXTRA 4
#define SKELETON_SECOND_LINE_OFFSET 2
static const int SKELETON_TITLE_WIDTHS[] = {260, 180, 320, 220, 280};
#define CARD_GRID_SPACING 6
/* A shelf that comes or goes slides open or shut in this time, while one whose cards changed shows the new ones at
 * once, as any refresh of data on screen does */
#define SHELF_MOTION_MILLISECONDS 300
#define SHELF_TITLE_KEY "shelf-title"
#define SHELF_CARDS_KEY "shelf-cards"
#define SHELF_ROWS_KEY "shelf-rows"
#define PAGE_EDGE_KEY "page-edge"
#define HEADING_ARTWORK_SIZE 120
/* Cards are fetched at the size they show, sharp at any width of the page */
#define CARD_ARTWORK_SIZE 160

/* The Last.fm text beside the heading stays a short paragraph, with the whole of it in its tooltip */
#define ABOUT_LINES 4
#define ABOUT_WIDTH 420
/* The heading drops the Last.fm text below this width, when the comments beside it or a narrow window leave too
 * little room for it and the title */
#define ABOUT_ROOM "max-width: 820px"

/* Headings, cards and songs keep to one centred column this wide, so on a wide window the eye never travels far
 * from a song's title to its length */
#define CONTENT_WIDTH 1040
/* From this width a page has room to give the album a column of its own in its song rows */
#define ALBUM_ROOM "min-width: 820px"

#define HERO_RESOLUTION 1200
#define BANNER_HEIGHT 180
#define HEADING_RESOLUTION 400

struct SongPage {
	const SongPageEvents *events;
	gpointer data;
	gboolean is_long;
	GtkWidget *root, *heading, *backdrop, *cover_slot, *title, *detail, *play_button, *shuffle_button;
	/* A round picture is a person's own, shown as a round cover beside the title rather than across the heading */
	gboolean is_profile;
	/* What Last.fm tells of an artist or album, beside the heading's title */
	GtkWidget *about, *about_text, *about_facts;
	/* Last.fm's line of facts, which takes the place under the title when the page gives none of its own, so every
	 * heading has a line there */
	char *facts;
	gboolean has_own_detail;
	GtkWidget *cards, *songs_heading, *stack, *list, *empty, *skeleton_cards, *scrolled;
	/* A place to scroll back to once the list has grown long enough to reach it */
	double wanted_scroll;
	gboolean is_restoring_scroll;
	gulong restore_handler;
	GListModel *songs, *extras;
	char *extras_title;
	gboolean is_extras_addable;
	/* The search in the heading narrows the page to the songs and cards whose words hold what is typed */
	GtkWidget *search_button, *search_revealer, *search_entry;
	GtkFilter *song_filter;
	GtkFilterListModel *shown_songs;
	char *query;
	gboolean is_reorderable;
	/* The songs are all known, so no songs means empty rather than still loading */
	gboolean is_loaded;
	/* The picture the heading cover shows, so a heading refreshed with the same one keeps its cover still */
	char *artwork;
	/* The artist or album the heading shows, which dragging the cover or title pins, or NULL on a playlist */
	Card *pin;
};


static void free_song_page(gpointer data) {
	SongPage *page = data;
	g_clear_object(&page->songs);
	g_clear_object(&page->extras);
	g_free(page->extras_title);
	g_clear_object(&page->shown_songs);
	g_clear_object(&page->song_filter);
	g_clear_pointer(&page->query, g_free);
	g_clear_pointer(&page->artwork, g_free);
	g_clear_pointer(&page->facts, g_free);
	g_clear_pointer(&page->pin, free_card);
	g_free(page);
}


/* Play and Random play what the page shows, so a search plays only the songs it found */
static void play_page(GtkButton *button, SongPage *page) {
	GListModel *songs = G_LIST_MODEL(page->shown_songs);
	guint count = g_list_model_get_n_items(songs);
	if (!count)
		return;
	gboolean is_shuffled = GTK_WIDGET(button) == page->shuffle_button;
	guint start = is_shuffled ? (guint)g_random_int_range(0, (gint32)count) : 0;
	page->events->play(songs, start, is_shuffled, page->data);
}


/* An extra song plays among the other extra songs, never as part of the page's own */
static void activate_song(GtkListView *list, guint position, SongPage *page) {
	GListModel *songs = G_LIST_MODEL(page->shown_songs);
	guint count = g_list_model_get_n_items(songs);
	if (position < count)
		page->events->choose(songs, position, page->data);
	else if (page->extras)
		page->events->choose(page->extras, position - count, page->data);
}


static GtkWidget *create_pill(const char *icon, const char *label, SongPage *page) {
	GtkWidget *button = gtk_button_new();
	GtkWidget *content = adw_button_content_new();
	adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), icon);
	adw_button_content_set_label(ADW_BUTTON_CONTENT(content), label);
	gtk_button_set_child(GTK_BUTTON(button), content);
	gtk_widget_add_css_class(button, "pill");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	g_signal_connect(button, "clicked", G_CALLBACK(play_page), page);
	return button;
}


static GdkContentProvider *start_heading_drag(GtkDragSource *source, double x, double y, SongPage *page) {
	if (!page->pin)
		return NULL;
	GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
	show_dragged_widget(source, widget, x, y);
	return gdk_content_provider_new_typed(CRIMSON_TYPE_CARD, page->pin);
}


static void add_heading_drag(GtkWidget *widget, SongPage *page) {
	GtkDragSource *source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_COPY);
	g_signal_connect(source, "prepare", G_CALLBACK(start_heading_drag), page);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(source));
}


/* Search =================================================================== */


static gboolean has_query_in(const char *const *texts, guint count, const char *query) {
	for (guint index = 0; index < count; index++) {
		if (!texts[index])
			continue;
		g_autofree char *folded = g_utf8_casefold(texts[index], -1);
		if (strstr(folded, query))
			return TRUE;
	}
	return FALSE;
}


static gboolean match_song(gpointer item, gpointer data) {
	SongPage *page = data;
	if (!page->query)
		return TRUE;
	CrimsonSong *song = item;
	const char *texts[] = {song->title, song->artist, song->album};
	return has_query_in(texts, G_N_ELEMENTS(texts), page->query);
}


/* Shows the cards under the widget that match and tells whether any did, so a row left with none hides as well */
static gboolean filter_cards(GtkWidget *widget, const char *query) {
	Card *card = g_object_get_data(G_OBJECT(widget), "card");
	if (card) {
		const char *texts[] = {card->title, card->subtitle};
		gboolean is_match = !query || has_query_in(texts, G_N_ELEMENTS(texts), query);
		gtk_widget_set_visible(widget, is_match);
		return is_match;
	}
	gboolean has_match = FALSE;
	for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
		has_match |= filter_cards(child, query);
	return has_match;
}


static guint count_shown_extras(SongPage *page) {
	return page->extras && !page->query ? g_list_model_get_n_items(page->extras) : 0;
}


static void show_extras(SongPage *page) {
	set_song_list_extras(page->list, page->extras_title, page->query ? NULL : page->extras, page->is_extras_addable);
}


static void filter_page(SongPage *page) {
	gtk_filter_changed(page->song_filter, GTK_FILTER_CHANGE_DIFFERENT);
	for (GtkWidget *section = gtk_widget_get_first_child(page->cards); section;
		section = gtk_widget_get_next_sibling(section))
		gtk_widget_set_visible(section, filter_cards(section, page->query));
	/* Rows keep their places while the songs are narrowed, since a filtered position is not the playlist's own */
	set_song_list_reorderable(page->list, page->is_reorderable && !page->query);
	/* Extra songs are no part of the page's own songs, so they step aside while it is searched */
	show_extras(page);
	update_song_page(page, page->is_loaded);
}


static void change_search(GtkSearchEntry *entry, SongPage *page) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
	g_clear_pointer(&page->query, g_free);
	if (*text)
		page->query = g_utf8_casefold(text, -1);
	filter_page(page);
}


static void toggle_search(GtkToggleButton *button, SongPage *page) {
	gboolean is_open = gtk_toggle_button_get_active(button);
	gtk_revealer_set_reveal_child(GTK_REVEALER(page->search_revealer), is_open);
	if (is_open) {
		gtk_widget_grab_focus(page->search_entry);
		return;
	}
	gtk_editable_set_text(GTK_EDITABLE(page->search_entry), "");
}


static void stop_search(GtkSearchEntry *entry, SongPage *page) {
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->search_button), FALSE);
}


static GtkWidget *create_search_button(SongPage *page) {
	page->search_button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(page->search_button), "system-search-symbolic");
	gtk_widget_set_tooltip_text(page->search_button, TEXT_SEARCH_PAGE);
	gtk_widget_add_css_class(page->search_button, "circular");
	gtk_widget_set_valign(page->search_button, GTK_ALIGN_CENTER);
	g_signal_connect(page->search_button, "toggled", G_CALLBACK(toggle_search), page);
	return page->search_button;
}


/* The search unrolls under the heading across the page's width, so it never crowds the heading on narrow windows,
 * and closing it brings every song back */
static GtkWidget *create_search_bar(SongPage *page) {
	page->search_entry = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(page->search_entry), TEXT_SEARCH_PAGE);
	gtk_widget_add_css_class(page->search_entry, "page-search");
	g_signal_connect(page->search_entry, "search-changed", G_CALLBACK(change_search), page);
	g_signal_connect(page->search_entry, "stop-search", G_CALLBACK(stop_search), page);
	page->search_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(page->search_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_child(GTK_REVEALER(page->search_revealer), page->search_entry);
	return page->search_revealer;
}


/* Loading ================================================================== */


static GtkWidget *create_bone(int width, int height, guint index) {
	GtkWidget *bone = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(bone, "bone");
	g_autofree char *delay = g_strdup_printf("wave-%u", index % SKELETON_WAVES);
	gtk_widget_add_css_class(bone, delay);
	gtk_widget_set_size_request(bone, width, height);
	gtk_widget_set_halign(bone, GTK_ALIGN_START);
	return bone;
}


static GtkWidget *create_bone_row(guint index) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	gtk_widget_add_css_class(row, "bone-row");
	gtk_box_append(GTK_BOX(row), create_bone(SKELETON_COVER, SKELETON_COVER, index));
	GtkWidget *lines = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_SNUG);
	gtk_widget_set_valign(lines, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(lines), create_bone(SKELETON_TITLE_WIDTHS[index % G_N_ELEMENTS(SKELETON_TITLE_WIDTHS)],
		SKELETON_LINE, index));
	guint second = (index + SKELETON_SECOND_LINE_OFFSET) % G_N_ELEMENTS(SKELETON_TITLE_WIDTHS);
	gtk_box_append(GTK_BOX(lines), create_bone(SKELETON_TITLE_WIDTHS[second] / 2, SKELETON_LINE, index));
	gtk_box_append(GTK_BOX(row), lines);
	return row;
}


/* While a page waits, soft shapes in its own layout breathe in a wave, rows of songs and for search a row of cards
 * above them, so the results take the places already shown */
static GtkWidget *create_skeleton(SongPage *page) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(box, "skeleton");
	page->skeleton_cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	GtkWidget *heading_bone = create_bone(SKELETON_HEADING, SKELETON_LINE + SKELETON_HEADING_EXTRA, 0);
	gtk_box_append(GTK_BOX(page->skeleton_cards), heading_bone);
	GtkWidget *cards = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	for (guint index = 0; index < SKELETON_CARDS; index++)
		gtk_box_append(GTK_BOX(cards), create_bone(CARD_ARTWORK_SIZE, CARD_ARTWORK_SIZE, index));
	/* The shapes are cut off at the edge as the cards they stand for scroll, so they never widen the window */
	GtkWidget *edge = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(edge), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(edge), cards);
	gtk_box_append(GTK_BOX(page->skeleton_cards), edge);
	gtk_widget_set_visible(page->skeleton_cards, FALSE);
	gtk_box_append(GTK_BOX(box), page->skeleton_cards);
	for (guint index = 0; index < SKELETON_ROWS; index++)
		gtk_box_append(GTK_BOX(box), create_bone_row(index));
	gtk_widget_set_valign(box, GTK_ALIGN_START);
	return box;
}


void set_song_page_waits_for_cards(SongPage *page, gboolean is_waiting) {
	gtk_widget_set_visible(page->skeleton_cards, is_waiting);
}


/* A plain clamp grows and shrinks with the window up to the column's width, rather than easing in its margins
 * before that */
static GtkWidget *create_column(GtkWidget *child) {
	GtkWidget *clamp = adw_clamp_new();
	adw_clamp_set_maximum_size(ADW_CLAMP(clamp), CONTENT_WIDTH);
	adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), CONTENT_WIDTH);
	adw_clamp_set_child(ADW_CLAMP(clamp), child);
	return clamp;
}


/* The heading sits on a still blur of its own cover, which sets it apart from the songs below */
static GtkWidget *create_heading(SongPage *page) {
	page->heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_LARGE);
	gtk_widget_add_css_class(page->heading, "page-heading");

	page->cover_slot = adw_bin_new();
	gtk_widget_set_valign(page->cover_slot, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(page->heading), page->cover_slot);
	add_heading_drag(page->cover_slot, page);
	/* The title starts level with the cover's top and the buttons end level with its bottom, and the words are as
	 * tall as a cover whether the heading has one or not, so every heading puts them in the same place and the page
	 * never jumps while it fills in */
	GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_SMALL);
	gtk_widget_set_size_request(labels, -1, HEADING_ARTWORK_SIZE);
	gtk_widget_set_hexpand(labels, TRUE);
	page->title = gtk_label_new(NULL);
	page->detail = gtk_label_new(NULL);
	gtk_widget_add_css_class(page->title, "title-1");
	add_heading_drag(page->title, page);
	gtk_widget_add_css_class(page->detail, "dimmed");
	gtk_widget_add_css_class(page->detail, "heading-detail");
	for (GtkWidget *label = page->title; label; label = label == page->title ? page->detail : NULL) {
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_box_append(GTK_BOX(labels), label);
	}
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	gtk_widget_set_vexpand(buttons, TRUE);
	gtk_widget_set_valign(buttons, GTK_ALIGN_END);
	page->play_button = create_pill("media-playback-start-symbolic", TEXT_PLAY, page);
	gtk_widget_add_css_class(page->play_button, "suggested-action");
	page->shuffle_button = create_pill("media-playlist-shuffle-symbolic", TEXT_SHUFFLE, page);
	gtk_box_append(GTK_BOX(buttons), page->play_button);
	gtk_box_append(GTK_BOX(buttons), page->shuffle_button);
	gtk_box_append(GTK_BOX(buttons), create_search_button(page));
	gtk_box_append(GTK_BOX(labels), buttons);
	gtk_box_append(GTK_BOX(page->heading), labels);
	/* A clamp holds the text to one exact width, empty or full, since a label grows to fit the text it is given and
	 * the title and picture beside it would move */
	GtkWidget *about_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_SNUG);
	page->about = adw_clamp_new();
	adw_clamp_set_maximum_size(ADW_CLAMP(page->about), ABOUT_WIDTH);
	adw_clamp_set_tightening_threshold(ADW_CLAMP(page->about), ABOUT_WIDTH);
	adw_clamp_set_child(ADW_CLAMP(page->about), about_content);
	gtk_widget_add_css_class(page->about, "page-about");
	gtk_widget_set_valign(page->about, GTK_ALIGN_CENTER);
	gtk_widget_set_vexpand(page->about, FALSE);
	gtk_widget_set_size_request(page->about, ABOUT_WIDTH, -1);
	gtk_widget_set_visible(page->about, FALSE);
	page->about_text = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(page->about_text), TRUE);
	gtk_label_set_lines(GTK_LABEL(page->about_text), ABOUT_LINES);
	gtk_label_set_ellipsize(GTK_LABEL(page->about_text), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(page->about_text), 0);
	page->about_facts = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(page->about_facts), 0);
	gtk_label_set_ellipsize(GTK_LABEL(page->about_facts), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(page->about_facts, "dimmed");
	gtk_widget_add_css_class(page->about_facts, "caption");
	gtk_box_append(GTK_BOX(about_content), page->about_text);
	gtk_box_append(GTK_BOX(about_content), page->about_facts);
	GtkWidget *about_slot = adw_bin_new();
	adw_bin_set_child(ADW_BIN(about_slot), page->about);
	gtk_box_append(GTK_BOX(page->heading), about_slot);
	GtkWidget *room = adw_breakpoint_bin_new();
	gtk_widget_set_size_request(room, 1, 1);
	/* The cover's glow reaches past the heading, which a breakpoint bin would cut off at its edge */
	gtk_widget_set_overflow(room, GTK_OVERFLOW_VISIBLE);
	adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(room), create_column(page->heading));
	AdwBreakpoint *narrow = adw_breakpoint_new(adw_breakpoint_condition_parse(ABOUT_ROOM));
	GValue hidden = G_VALUE_INIT;
	g_value_init(&hidden, G_TYPE_BOOLEAN);
	adw_breakpoint_add_setter(narrow, G_OBJECT(about_slot), "visible", &hidden);
	adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(room), narrow);
	GtkWidget *veil = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(veil, "page-veil");
	gtk_box_append(GTK_BOX(veil), room);
	/* Every banner is this tall whatever it holds, so moving between pages or loading one never changes its size */
	gtk_widget_set_size_request(veil, -1, BANNER_HEIGHT);
	gtk_widget_set_valign(room, GTK_ALIGN_CENTER);
	gtk_widget_set_vexpand(room, TRUE);
	page->backdrop = create_ambience(veil);
	gtk_widget_set_overflow(page->backdrop, GTK_OVERFLOW_HIDDEN);
	return page->backdrop;
}


static GtkWidget *create_status(const char *icon, const char *title) {
	GtkWidget *status = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), icon);
	adw_status_page_set_title(ADW_STATUS_PAGE(status), title);
	return status;
}


static GtkWidget *create_section_heading(const char *title) {
	GtkWidget *heading = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(heading), 0);
	gtk_widget_add_css_class(heading, "title-4");
	gtk_widget_add_css_class(heading, "section-heading");
	return heading;
}

/* A place still being scrolled back to counts as where the page is, so leaving at once never loses it */
double get_song_page_scroll(SongPage *page) {
	if (page->is_restoring_scroll)
		return page->wanted_scroll;
	return gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(page->scrolled)));
}


static void stop_restoring_scroll(SongPage *page) {
	page->is_restoring_scroll = FALSE;
	GdkFrameClock *clock = gtk_widget_get_frame_clock(page->scrolled);
	if (clock)
		g_clear_signal_handler(&page->restore_handler, clock);
}


/* The list only knows its new length once it has laid out its new songs, and while it lays out it ignores scrolling,
 * so the place is set just after each layout, which the list takes up before the frame is drawn. A list off screen
 * or still without songs has no length yet, so the place waits for it, and is kept until it is reached or every
 * song is in */
static void restore_scroll(GdkFrameClock *clock, GtkWidget *scrolled) {
	SongPage *page = g_object_get_data(G_OBJECT(scrolled), "song-page");
	guint count = g_list_model_get_n_items(G_LIST_MODEL(page->shown_songs));
	if (!count || !gtk_widget_get_mapped(page->list)) {
		if (!count && page->is_loaded)
			stop_restoring_scroll(page);
		return;
	}
	GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
	gtk_adjustment_set_value(adjustment, page->wanted_scroll);
	gboolean is_reached = gtk_adjustment_get_value(adjustment) >= page->wanted_scroll - 1;
	if (is_reached || page->is_loaded)
		stop_restoring_scroll(page);
}


/* The listener scrolling or clicking in the list takes over from a place still being waited for */
static gboolean notice_listener(GtkEventControllerLegacy *controller, GdkEvent *event, SongPage *page) {
	GdkEventType type = gdk_event_get_event_type(event);
	gboolean is_input = type == GDK_SCROLL || type == GDK_BUTTON_PRESS || type == GDK_KEY_PRESS
		|| type == GDK_TOUCH_BEGIN;
	if (is_input && page->is_restoring_scroll)
		stop_restoring_scroll(page);
	return FALSE;
}


/* The layouts are watched from before the next one starts, since a watch added during a layout only sees the one
 * after it, a frame too late */
static void watch_layouts(SongPage *page) {
	GdkFrameClock *clock = gtk_widget_get_frame_clock(page->scrolled);
	if (!page->is_restoring_scroll || page->restore_handler || !clock)
		return;
	page->restore_handler = g_signal_connect_object(clock, "layout", G_CALLBACK(restore_scroll), page->scrolled,
		G_CONNECT_AFTER);
}


void set_song_page_scroll(SongPage *page, double value) {
	page->wanted_scroll = value;
	page->is_restoring_scroll = TRUE;
	watch_layouts(page);
}


/* A long playlist scrolls only its songs under a fixed heading, so thousands of rows stay quick, while
 * artists, albums and search results scroll as one page */
static GtkWidget *create_body(SongPage *page) {
	GtkWidget *scrolled = gtk_scrolled_window_new();
	page->scrolled = scrolled;
	gtk_widget_add_css_class(scrolled, "page-scroller");
	g_object_set_data(G_OBJECT(scrolled), "song-page", page);
	/* A place asked for before the window is on screen is watched for once the page shows */
	g_signal_connect_swapped(scrolled, "map", G_CALLBACK(watch_layouts), page);
	GtkEventController *listener = gtk_event_controller_legacy_new();
	gtk_event_controller_set_propagation_phase(listener, GTK_PHASE_CAPTURE);
	g_signal_connect(listener, "event", G_CALLBACK(notice_listener), page);
	gtk_widget_add_controller(scrolled, listener);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	add_smooth_scrolling(GTK_SCROLLED_WINDOW(scrolled));
	if (page->is_long) {
		GtkWidget *column = adw_clamp_scrollable_new();
		adw_clamp_scrollable_set_maximum_size(ADW_CLAMP_SCROLLABLE(column), CONTENT_WIDTH);
		adw_clamp_scrollable_set_tightening_threshold(ADW_CLAMP_SCROLLABLE(column), CONTENT_WIDTH);
		adw_clamp_scrollable_set_child(ADW_CLAMP_SCROLLABLE(column), page->list);
		gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), column);
		return scrolled;
	}
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(box, "page-body");
	gtk_box_append(GTK_BOX(box), page->cards);
	page->songs_heading = create_section_heading(TEXT_SONGS);
	gtk_box_append(GTK_BOX(box), page->songs_heading);
	gtk_box_append(GTK_BOX(box), page->list);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), create_column(box));
	return scrolled;
}


static void show_album_column(SongPage *page) {
	set_song_list_album_column(page->list, TRUE);
}


static void hide_album_column(SongPage *page) {
	set_song_list_album_column(page->list, FALSE);
}


/* The page's own width decides the album column, so the comments beside it narrow the rows as a smaller window
 * does */
static GtkWidget *create_album_room(SongPage *page) {
	GtkWidget *room = adw_breakpoint_bin_new();
	gtk_widget_set_size_request(room, 1, 1);
	gtk_widget_set_vexpand(room, TRUE);
	adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(room), page->stack);
	AdwBreakpoint *wide = adw_breakpoint_new(adw_breakpoint_condition_parse(ALBUM_ROOM));
	g_signal_connect_swapped(wide, "apply", G_CALLBACK(show_album_column), page);
	g_signal_connect_swapped(wide, "unapply", G_CALLBACK(hide_album_column), page);
	adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(room), wide);
	return room;
}


SongPage *create_song_page(const SongPageEvents *events, gpointer data, gboolean is_long) {
	SongPage *page = g_new0(SongPage, 1);
	page->events = events;
	page->data = data;
	page->is_long = is_long;
	page->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(page->root, "view");
	g_object_set_data_full(G_OBJECT(page->root), "song-page", page, free_song_page);
	gtk_box_append(GTK_BOX(page->root), create_heading(page));
	gtk_box_append(GTK_BOX(page->root), create_search_bar(page));
	page->cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	page->list = create_song_list(&events->list, data);
	page->song_filter = GTK_FILTER(gtk_custom_filter_new(match_song, page, NULL));
	page->shown_songs = gtk_filter_list_model_new(NULL, g_object_ref(page->song_filter));
	g_signal_connect(page->list, "activate", G_CALLBACK(activate_song), page);
	page->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(page->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	/* Only the state on show counts towards the page's height, since the waiting shapes alone are taller than a
	 * small window and would push the player out of it */
	gtk_stack_set_vhomogeneous(GTK_STACK(page->stack), FALSE);
	gtk_widget_set_vexpand(page->stack, TRUE);
	/* The waiting shapes are cut off at the page's foot rather than making the window taller and pushing the player
	 * out of it */
	GtkWidget *skeleton = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(skeleton), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(skeleton), create_column(create_skeleton(page)));
	gtk_stack_add_named(GTK_STACK(page->stack), skeleton, "loading");
	gtk_stack_add_named(GTK_STACK(page->stack), create_body(page), "songs");
	page->empty = create_status("view-list-symbolic", TEXT_EMPTY_PLAYLIST);
	gtk_stack_add_named(GTK_STACK(page->stack), page->empty, "empty");
	gtk_box_append(GTK_BOX(page->root), create_album_room(page));
	return page;
}


GtkWidget *get_song_page_widget(SongPage *page) {
	return page->root;
}


GListModel *get_song_page_songs(SongPage *page) {
	return page->songs;
}


/* The heading cover spins or pops in once its picture is here, with the backdrop and glow taking its colours */
static void follow_heading_cover(GtkWidget *cover, GParamSpec *spec, SongPage *page) {
	set_ambience_texture(page->backdrop, get_cover_texture(cover));
	/* An artist's picture fills the heading instead of a small round cover */
	gboolean is_banner = gtk_widget_has_css_class(cover, "round") && !page->is_profile;
	set_ambience_picture(page->backdrop, is_banner ? get_cover_texture(cover) : NULL);
	if (!get_cover_texture(cover))
		return;
	if (gtk_widget_has_css_class(cover, "round"))
		spin_cover_in(cover);
	else
		replay_entrance(cover);
}


void set_song_page_pin(SongPage *page, const char *id, const char *title, const char *artwork) {
	g_clear_pointer(&page->pin, free_card);
	page->pin = g_new0(Card, 1);
	page->pin->id = g_strdup(id);
	page->pin->title = g_strdup(title);
	page->pin->artwork = g_strdup(artwork);
}


void set_song_page_profile(SongPage *page) {
	page->is_profile = TRUE;
}


/* The line under the title is the page's own, or Last.fm's facts in its place, fading in when it arrives */
static void show_heading_detail(SongPage *page, const char *detail) {
	const char *shown = page->has_own_detail ? detail : page->facts ? page->facts : "";
	gtk_label_set_text(GTK_LABEL(page->detail), shown);
	if (*shown)
		gtk_widget_remove_css_class(page->detail, "waiting");
	else
		gtk_widget_add_css_class(page->detail, "waiting");
	gboolean has_facts_beside = page->has_own_detail && page->facts;
	gtk_label_set_text(GTK_LABEL(page->about_facts), has_facts_beside ? page->facts : "");
	gtk_widget_set_visible(page->about_facts, has_facts_beside);
}


void set_song_page_heading(SongPage *page, const char *title, const char *detail, const char *artwork,
	gboolean is_round) {
	gboolean is_banner = is_round && !page->is_profile;
	gtk_widget_set_visible(page->backdrop, title != NULL);
	gtk_label_set_text(GTK_LABEL(page->title), title ? title : "");
	/* A title arriving fades in rather than appearing at once */
	if (title && *title)
		gtk_widget_remove_css_class(page->title, "waiting");
	else
		gtk_widget_add_css_class(page->title, "waiting");
	page->has_own_detail = detail && *detail;
	show_heading_detail(page, detail);
	GtkWidget *shown = adw_bin_get_child(ADW_BIN(page->cover_slot));
	if (shown && artwork && g_strcmp0(artwork, page->artwork) == 0 && gtk_widget_has_css_class(shown, "round") == is_round)
		return;
	g_set_str(&page->artwork, artwork);
	GtkWidget *cover = create_cover(HEADING_ARTWORK_SIZE);
	gtk_widget_add_css_class(cover, "heading-cover");
	add_cover_glow(cover);
	if (is_round)
		gtk_widget_add_css_class(cover, "round");
	/* An artist's picture spreads across the whole heading, so it is fetched far larger than an album's cover */
	set_cover_resolution(cover, is_banner ? HERO_RESOLUTION : HEADING_RESOLUTION);
	gtk_widget_set_visible(page->cover_slot, !is_banner);
	/* Over an artist's picture the heading's title and buttons are lifted off the picture by a soft shadow */
	if (is_banner)
		gtk_widget_add_css_class(page->heading, "over-picture");
	else
		gtk_widget_remove_css_class(page->heading, "over-picture");
	g_signal_connect(cover, "notify::texture", G_CALLBACK(follow_heading_cover), page);
	show_artwork(cover, artwork);
	adw_bin_set_child(ADW_BIN(page->cover_slot), cover);
}


/* Songs added or removed anywhere switch the page between its list and its empty view at once */
static void follow_song_count(GListModel *songs, guint position, guint removed, guint added, SongPage *page) {
	update_song_page(page, page->is_loaded);
}


void set_song_page_songs(SongPage *page, GListModel *songs, gboolean is_reorderable, gboolean is_entering) {
	if (page->songs != songs) {
		if (page->songs)
			g_signal_handlers_disconnect_by_data(page->songs, page);
		g_set_object(&page->songs, songs);
		if (songs)
			g_signal_connect(songs, "items-changed", G_CALLBACK(follow_song_count), page);
		/* Each list of songs gets its own narrowed copy, so the view starts afresh rather than holding its place */
		g_clear_object(&page->shown_songs);
		page->shown_songs = gtk_filter_list_model_new(songs ? g_object_ref(songs) : NULL, g_object_ref(page->song_filter));
		set_song_list_songs(page->list, G_LIST_MODEL(page->shown_songs), is_reorderable && !page->query);
	}
	page->is_reorderable = is_reorderable;
	set_song_list_reorderable(page->list, is_reorderable && !page->query);
	if (!is_entering)
		return;
	/* Another playlist opening in the same page starts with all of its songs */
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->search_button), FALSE);
	replay_entrance(page->heading);
	replay_entrance(page->stack);
}


void set_song_page_extras(SongPage *page, const char *title, GListModel *extras, gboolean is_addable) {
	g_set_object(&page->extras, extras);
	g_free(page->extras_title);
	page->extras_title = g_strdup(title);
	page->is_extras_addable = is_addable;
	show_extras(page);
}


/* The songs heading only shows above songs that share the page with artists or albums */
void update_song_page(SongPage *page, gboolean is_loaded) {
	page->is_loaded = is_loaded;
	guint count = page->songs ? g_list_model_get_n_items(page->songs) : 0;
	guint shown_count = g_list_model_get_n_items(G_LIST_MODEL(page->shown_songs));
	gboolean has_cards = gtk_widget_get_first_child(page->cards) != NULL;
	guint extra_count = count_shown_extras(page);
	const char *shown = count || has_cards || extra_count ? "songs" : !is_loaded ? "loading" : "empty";
	gtk_stack_set_visible_child_name(GTK_STACK(page->stack), shown);
	if (page->songs_heading)
		gtk_widget_set_visible(page->songs_heading, shown_count && has_cards);
	gtk_widget_set_visible(page->list, shown_count || extra_count);
	gtk_widget_set_sensitive(page->play_button, shown_count > 0);
	gtk_widget_set_sensitive(page->shuffle_button, shown_count > 0);
}


void set_song_page_empty_title(SongPage *page, const char *title) {
	adw_status_page_set_title(ADW_STATUS_PAGE(page->empty), title);
}


void set_song_page_empty(SongPage *page, const char *icon, const char *title, const char *detail) {
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(page->empty), icon);
	adw_status_page_set_title(ADW_STATUS_PAGE(page->empty), title);
	adw_status_page_set_description(ADW_STATUS_PAGE(page->empty), detail);
}


void remove_song_page_cards(SongPage *page, GtkWidget *section) {
	if (section && gtk_widget_get_parent(section) == page->cards)
		gtk_box_remove(GTK_BOX(page->cards), section);
}


static void open_card(GtkButton *button, SongPage *page) {
	page->events->open_card(g_object_get_data(G_OBJECT(button), "card-id"), page->data);
}


static void click_card(GtkGestureClick *gesture, int presses, double x, double y, SongPage *page) {
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	GtkWidget *button = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	page->events->open_card_menu(g_object_get_data(G_OBJECT(button), "card-id"), button, x, y, page->data);
}


static GdkContentProvider *start_card_drag(GtkDragSource *source, double x, double y, GtkWidget *button) {
	show_dragged_widget(source, button, x, y);
	return gdk_content_provider_new_typed(CRIMSON_TYPE_CARD, g_object_get_data(G_OBJECT(button), "card"));
}


static GtkWidget *create_card(Card *card, gboolean is_round, SongPage *page) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_SMALL);
	GtkWidget *cover = create_cover(CARD_ARTWORK_SIZE);
	set_cover_resolution(cover, CARD_ARTWORK_SIZE);
	if (is_round)
		gtk_widget_add_css_class(cover, "round");
	show_artwork(cover, card->artwork);
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), cover);
	if (card->video_id) {
		GtkWidget *playing = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
		gtk_widget_add_css_class(playing, "playing-icon");
		gtk_overlay_add_overlay(GTK_OVERLAY(overlay), playing);
	}
	gtk_box_append(GTK_BOX(box), overlay);
	const char *texts[] = {card->title, card->subtitle};
	for (guint index = 0; index < G_N_ELEMENTS(texts); index++) {
		GtkWidget *label = gtk_label_new(texts[index]);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
		/* Words start at the picture's edge, in line with the headings and songs, so cut ones end evenly too */
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_widget_set_size_request(label, CARD_ARTWORK_SIZE, -1);
		gtk_widget_add_css_class(label, index ? "card-detail" : "card-title");
		gtk_box_append(GTK_BOX(box), label);
	}
	GtkWidget *button = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(button), box);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_add_css_class(button, "card-button");
	gtk_widget_set_tooltip_text(button, card->title);
	g_object_set_data_full(G_OBJECT(button), "card-id", g_strdup(card->id), g_free);
	g_signal_connect(button, "clicked", G_CALLBACK(open_card), page);
	if (card->video_id)
		follow_playing_video(button, card->video_id);
	g_object_set_data_full(G_OBJECT(button), "card", copy_card(card), free_card);
	GtkDragSource *source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_COPY);
	g_signal_connect(source, "prepare", G_CALLBACK(start_card_drag), button);
	gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(source));
	GtkGesture *menu_click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu_click), GDK_BUTTON_SECONDARY);
	g_signal_connect(menu_click, "pressed", G_CALLBACK(click_card), page);
	gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(menu_click));
	return button;
}


/* A titled row of covers that scrolls sideways */
/* Cards rise into place one shortly after another, so a row builds up from the left */
static void stagger_entrance(GtkWidget *card, guint index) {
	g_autofree char *delay = g_strdup_printf("stagger-%u", MIN(index, STAGGER_STEPS - 1));
	gtk_widget_add_css_class(card, "card-in");
	gtk_widget_add_css_class(card, delay);
	replay_entrance(card);
}


static void measure_shelf_room(GtkWidget *room, GtkOrientation orientation, int for_size, int *minimum,
	int *natural, int *minimum_baseline, int *natural_baseline) {
	gtk_widget_measure(gtk_widget_get_first_child(room), orientation, for_size, minimum, natural, minimum_baseline,
		natural_baseline);
}


/* A row of cards starts at the column's edge like everything on the page but runs on to the page's own right edge,
 * so the window cuts it, as a row that goes on, rather than the middle of the page */
static void allocate_shelf_room(GtkWidget *room, int width, int height, int baseline) {
	GtkWidget *page_edge = g_object_get_data(G_OBJECT(room), PAGE_EDGE_KEY);
	graphene_point_t place;
	int reach = 0;
	if (page_edge && gtk_widget_compute_point(room, page_edge, &GRAPHENE_POINT_INIT(0, 0), &place))
		reach = MAX(0, gtk_widget_get_width(page_edge) - (int)place.x - width);
	gtk_widget_allocate(gtk_widget_get_first_child(room), width + reach, height, baseline, NULL);
}


static GtkWidget *create_shelf_room(SongPage *page, GtkWidget *shelf) {
	GtkWidget *room = adw_bin_new();
	gtk_widget_set_layout_manager(room, gtk_custom_layout_new(NULL, measure_shelf_room, allocate_shelf_room));
	g_object_set_data(G_OBJECT(room), PAGE_EDGE_KEY, page->scrolled);
	adw_bin_set_child(ADW_BIN(room), shelf);
	return room;
}


static GtkWidget *create_card_row(SongPage *page, GPtrArray *cards, gboolean is_round, gboolean is_staggered) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SMALL);
	gtk_widget_add_css_class(row, "card-row");
	for (guint index = 0; index < cards->len; index++) {
		GtkWidget *card = create_card(g_ptr_array_index(cards, index), is_round, page);
		if (is_staggered)
			stagger_entrance(card, index);
		gtk_box_append(GTK_BOX(row), card);
	}
	GtkWidget *scrolled = gtk_scrolled_window_new();
	gtk_widget_add_css_class(scrolled, "card-shelf");
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), row);
	add_smooth_scrolling(GTK_SCROLLED_WINDOW(scrolled));
	return create_shelf_room(page, scrolled);
}


GtkWidget *add_song_page_cards(SongPage *page, const char *title, GPtrArray *cards, gboolean is_round,
	gboolean is_first) {
	if (!cards->len)
		return NULL;
	GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_box_append(GTK_BOX(section), create_section_heading(title));
	GtkWidget *scrolled = create_card_row(page, cards, is_round, TRUE);
	gtk_box_append(GTK_BOX(section), scrolled);
	if (is_first)
		gtk_box_prepend(GTK_BOX(page->cards), section);
	else
		gtk_box_append(GTK_BOX(page->cards), section);
	/* Cards arriving during a search show only when they match it */
	gtk_widget_set_visible(section, filter_cards(section, page->query));
	replay_entrance(scrolled);
	return section;
}


/* Every word and picture a shelf shows, so a refresh can tell whether its cards changed */
static char *describe_shelf(Shelf *shelf) {
	GString *text = g_string_new(shelf->is_round ? "round" : "");
	for (guint index = 0; index < shelf->cards->len; index++) {
		Card *card = g_ptr_array_index(shelf->cards, index);
		g_string_append_printf(text, "\x1e%s\x1f%s\x1f%s\x1f%s", card->id, card->title, card->subtitle,
			card->artwork);
	}
	return g_string_free(text, FALSE);
}


static void fill_shelf(SongPage *page, GtkWidget *widget, Shelf *shelf, gboolean is_staggered) {
	g_autofree char *description = describe_shelf(shelf);
	if (g_strcmp0(description, g_object_get_data(G_OBJECT(widget), SHELF_CARDS_KEY)) == 0)
		return;
	g_object_set_data_full(G_OBJECT(widget), SHELF_CARDS_KEY, g_steal_pointer(&description), g_free);
	GtkWidget *rows = g_object_get_data(G_OBJECT(widget), SHELF_ROWS_KEY);
	GtkWidget *old = gtk_widget_get_first_child(rows);
	if (old)
		gtk_box_remove(GTK_BOX(rows), old);
	gtk_box_append(GTK_BOX(rows), create_card_row(page, shelf->cards, shelf->is_round, is_staggered));
	gtk_widget_set_visible(widget, filter_cards(widget, page->query));
}


static void drop_closed_shelf(GtkRevealer *shelf, GParamSpec *spec, SongPage *page) {
	if (!gtk_revealer_get_reveal_child(shelf) && !gtk_revealer_get_child_revealed(shelf))
		gtk_box_remove(GTK_BOX(page->cards), GTK_WIDGET(shelf));
}


/* A shelf on a page that shows nothing yet is open from the start, with its cards rising in one after another */
static GtkWidget *create_shelf(SongPage *page, Shelf *shelf, gboolean is_first) {
	GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_box_append(GTK_BOX(section), create_section_heading(shelf->title));
	GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(section), rows);
	GtkWidget *widget = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(widget), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_transition_duration(GTK_REVEALER(widget), SHELF_MOTION_MILLISECONDS);
	gtk_revealer_set_reveal_child(GTK_REVEALER(widget), is_first);
	gtk_revealer_set_child(GTK_REVEALER(widget), section);
	g_object_set_data_full(G_OBJECT(widget), SHELF_TITLE_KEY, g_strdup(shelf->title), g_free);
	g_object_set_data(G_OBJECT(widget), SHELF_ROWS_KEY, rows);
	g_signal_connect(widget, "notify::child-revealed", G_CALLBACK(drop_closed_shelf), page);
	fill_shelf(page, widget, shelf, is_first);
	return widget;
}


/* The shelf with the title that this refresh has not matched to a shelf yet, or NULL for a new one */
static GtkWidget *find_shelf(SongPage *page, const char *title, GHashTable *matched) {
	for (GtkWidget *widget = gtk_widget_get_first_child(page->cards); widget;
		widget = gtk_widget_get_next_sibling(widget)) {
		if (g_strcmp0(g_object_get_data(G_OBJECT(widget), SHELF_TITLE_KEY), title) == 0
			&& !g_hash_table_contains(matched, widget))
			return widget;
	}
	return NULL;
}


void set_song_page_shelves(SongPage *page, GPtrArray *shelves) {
	gboolean is_first = gtk_widget_get_first_child(page->cards) == NULL;
	g_autoptr(GHashTable) matched = g_hash_table_new(NULL, NULL);
	GtkWidget *previous = NULL;
	for (guint index = 0; index < shelves->len; index++) {
		Shelf *shelf = g_ptr_array_index(shelves, index);
		if (!shelf->cards->len)
			continue;
		GtkWidget *widget = find_shelf(page, shelf->title, matched);
		if (widget) {
			fill_shelf(page, widget, shelf, FALSE);
			gtk_box_reorder_child_after(GTK_BOX(page->cards), widget, previous);
		} else {
			widget = create_shelf(page, shelf, is_first);
			gtk_box_insert_child_after(GTK_BOX(page->cards), widget, previous);
		}
		gtk_revealer_set_reveal_child(GTK_REVEALER(widget), TRUE);
		g_hash_table_add(matched, widget);
		previous = widget;
	}
	/* A shelf off screen shuts at once and leaves the page, so the next one is found before closing it */
	GtkWidget *next = NULL;
	for (GtkWidget *widget = gtk_widget_get_first_child(page->cards); widget; widget = next) {
		next = gtk_widget_get_next_sibling(widget);
		if (g_object_get_data(G_OBJECT(widget), SHELF_TITLE_KEY) && !g_hash_table_contains(matched, widget))
			gtk_revealer_set_reveal_child(GTK_REVEALER(widget), FALSE);
	}
}


void add_song_page_card_grid(SongPage *page, GPtrArray *cards, gboolean is_round) {
	GtkWidget *grid = adw_wrap_box_new();
	adw_wrap_box_set_child_spacing(ADW_WRAP_BOX(grid), CARD_GRID_SPACING);
	adw_wrap_box_set_line_spacing(ADW_WRAP_BOX(grid), CARD_GRID_SPACING);
	gtk_widget_add_css_class(grid, "card-grid");
	for (guint index = 0; index < cards->len; index++) {
		GtkWidget *card = create_card(g_ptr_array_index(cards, index), is_round, page);
		stagger_entrance(card, index);
		adw_wrap_box_append(ADW_WRAP_BOX(grid), card);
	}
	gtk_box_append(GTK_BOX(page->cards), grid);
	gtk_widget_set_visible(grid, filter_cards(grid, page->query));
	replay_entrance(grid);
}


void set_song_page_playable(SongPage *page, gboolean is_playable) {
	gtk_widget_set_visible(gtk_widget_get_parent(page->play_button), is_playable);
}


void set_song_page_quiet_likes(SongPage *page, gboolean is_quiet) {
	if (is_quiet)
		gtk_widget_add_css_class(page->list, "quiet-likes");
	else
		gtk_widget_remove_css_class(page->list, "quiet-likes");
}


/* The place for the Last.fm text is kept from the start, so neither the title nor the picture moves when it arrives */
void keep_song_page_about(SongPage *page) {
	gtk_widget_add_css_class(page->about, "waiting");
	gtk_widget_set_visible(page->about, TRUE);
}


/* The text fades in where its place was kept */
void set_song_page_about(SongPage *page, const char *text, const char *facts) {
	gboolean has_text = text && *text, has_facts = facts && *facts;
	gtk_label_set_text(GTK_LABEL(page->about_text), has_text ? text : "");
	gtk_widget_set_tooltip_text(page->about_text, has_text ? text : NULL);
	g_set_str(&page->facts, has_facts ? facts : NULL);
	g_autofree char *detail = g_strdup(gtk_label_get_text(GTK_LABEL(page->detail)));
	show_heading_detail(page, detail);
	gtk_widget_set_visible(page->about, TRUE);
	gtk_widget_remove_css_class(page->about, "waiting");
}


void reveal_song_page_song(SongPage *page, const char *video_id) {
	reveal_song_in_list(page->list, video_id);
}

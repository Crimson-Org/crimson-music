#include <math.h>
#include <string.h>
#include <adwaita.h>

#include "../lang/en.h"
#include "ambience.h"
#include "artwork.h"
#include "common/loading.h"
#include "common/smooth-scroll.h"
#include "motion.h"
#include "song-page.h"

#define SKELETON_WAVES 6
#define STAGGER_STEPS 10
#define SKELETON_ROWS 8
#define SKELETON_CARDS 6
#define SKELETON_COVER 40
#define SKELETON_LINE 12
#define SKELETON_CARD 120
#define SKELETON_HEADING 140
#define SKELETON_WIDTH 1000
static const int SKELETON_TITLE_WIDTHS[] = {260, 180, 320, 220, 280};
#define CARD_GRID_SPACING 6
#define HEADING_ARTWORK_SIZE 120
#define CARD_ARTWORK_SIZE 120
/* Content stops growing at this width, so lines stay easy to follow on wide windows */
#define CONTENT_MAXIMUM_WIDTH 1100
#define CONTENT_TIGHTENING_WIDTH 800

#define SCROLL_HOLD_MICROSECONDS (G_USEC_PER_SEC / 2)

/* The Last.fm text beside the heading stays a short paragraph, with the whole of it in its tooltip */
#define ABOUT_LINES 4
#define ABOUT_WIDTH 420
/* The heading drops the Last.fm text below this width, when the comments beside it or a narrow window leave too
 * little room for it and the title */
#define ABOUT_ROOM "max-width: 820px"

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
	GtkWidget *about, *about_text, *about_facts, *heading_clamp;
	GtkWidget *cards, *songs_heading, *stack, *list, *empty, *skeleton_cards, *scrolled;
	/* A place to scroll back to once the songs are long enough to reach it, and when to stop waiting for that */
	double wanted_scroll;
	gint64 scroll_settled;
	/* Where the page last put the scroll, so a different place means the listener moved it */
	double set_scroll;
	gboolean has_set_scroll;
	guint scroll_tick;
	GListModel *songs, *suggestions;
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
	g_clear_object(&page->suggestions);
	g_clear_object(&page->shown_songs);
	g_clear_object(&page->song_filter);
	g_clear_pointer(&page->query, g_free);
	g_clear_pointer(&page->artwork, g_free);
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


/* A suggestion plays among the other suggestions, never as part of the playlist */
static void activate_song(GtkListView *list, guint position, SongPage *page) {
	GListModel *songs = G_LIST_MODEL(page->shown_songs);
	guint count = g_list_model_get_n_items(songs);
	if (position < count)
		page->events->choose(songs, position, page->data);
	else if (page->suggestions)
		page->events->choose(page->suggestions, position - count, page->data);
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


static GtkWidget *create_clamp(GtkWidget *child) {
	GtkWidget *clamp = adw_clamp_new();
	adw_clamp_set_maximum_size(ADW_CLAMP(clamp), CONTENT_MAXIMUM_WIDTH);
	adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), CONTENT_TIGHTENING_WIDTH);
	adw_clamp_set_child(ADW_CLAMP(clamp), child);
	return clamp;
}


static GdkContentProvider *start_heading_drag(GtkDragSource *source, double x, double y, SongPage *page) {
	if (!page->pin)
		return NULL;
	GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
	gtk_drag_source_set_icon(source, gtk_widget_paintable_new(widget), (int)x, (int)y);
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


static void filter_page(SongPage *page) {
	gtk_filter_changed(page->song_filter, GTK_FILTER_CHANGE_DIFFERENT);
	for (GtkWidget *section = gtk_widget_get_first_child(page->cards); section;
		section = gtk_widget_get_next_sibling(section))
		gtk_widget_set_visible(section, filter_cards(section, page->query));
	/* Rows keep their places while the songs are narrowed, since a filtered position is not the playlist's own */
	set_song_list_reorderable(page->list, page->is_reorderable && !page->query);
	/* Suggestions are no part of the page's own songs, so they step aside while it is searched */
	set_song_list_suggestions(page->list, page->query ? NULL : page->suggestions);
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
	gtk_revealer_set_child(GTK_REVEALER(page->search_revealer), create_clamp(page->search_entry));
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
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_add_css_class(row, "bone-row");
	gtk_box_append(GTK_BOX(row), create_bone(SKELETON_COVER, SKELETON_COVER, index));
	GtkWidget *lines = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_valign(lines, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(lines), create_bone(SKELETON_TITLE_WIDTHS[index % G_N_ELEMENTS(SKELETON_TITLE_WIDTHS)],
		SKELETON_LINE, index));
	gtk_box_append(GTK_BOX(lines), create_bone(SKELETON_TITLE_WIDTHS[(index + 2) % G_N_ELEMENTS(SKELETON_TITLE_WIDTHS)]
		/ 2, SKELETON_LINE, index));
	gtk_box_append(GTK_BOX(row), lines);
	return row;
}


/* While a page waits, soft shapes in its own layout breathe in a wave, rows of songs and for search a row of cards
 * above them, so the results take the places already shown */
static GtkWidget *create_skeleton(SongPage *page) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_add_css_class(box, "skeleton");
	page->skeleton_cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_box_append(GTK_BOX(page->skeleton_cards), create_bone(SKELETON_HEADING, SKELETON_LINE + 4, 0));
	GtkWidget *cards = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	for (guint index = 0; index < SKELETON_CARDS; index++)
		gtk_box_append(GTK_BOX(cards), create_bone(SKELETON_CARD, SKELETON_CARD, index));
	/* The shapes are cut off at the edge as the cards they stand for scroll, so they never widen the window */
	GtkWidget *edge = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(edge), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(edge), cards);
	gtk_box_append(GTK_BOX(page->skeleton_cards), edge);
	gtk_widget_set_visible(page->skeleton_cards, FALSE);
	gtk_box_append(GTK_BOX(box), page->skeleton_cards);
	for (guint index = 0; index < SKELETON_ROWS; index++)
		gtk_box_append(GTK_BOX(box), create_bone_row(index));
	GtkWidget *clamp = adw_clamp_new();
	adw_clamp_set_maximum_size(ADW_CLAMP(clamp), SKELETON_WIDTH);
	adw_clamp_set_child(ADW_CLAMP(clamp), box);
	gtk_widget_set_valign(clamp, GTK_ALIGN_START);
	return clamp;
}


void set_song_page_waits_for_cards(SongPage *page, gboolean is_waiting) {
	gtk_widget_set_visible(page->skeleton_cards, is_waiting);
}


/* The heading sits on a still blur of its own cover, which sets it apart from the songs below */
static GtkWidget *create_heading(SongPage *page) {
	page->heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
	gtk_widget_add_css_class(page->heading, "page-heading");
	/* Every heading is as tall as its cover whatever loads into it, so the page never jumps while it fills in */
	gtk_widget_set_size_request(page->heading, -1, HEADING_ARTWORK_SIZE);
	page->cover_slot = adw_bin_new();
	gtk_widget_set_valign(page->cover_slot, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(page->heading), page->cover_slot);
	add_heading_drag(page->cover_slot, page);
	GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(labels, TRUE);
	page->title = gtk_label_new(NULL);
	page->detail = gtk_label_new(NULL);
	gtk_widget_add_css_class(page->title, "title-1");
	add_heading_drag(page->title, page);
	gtk_widget_add_css_class(page->detail, "dimmed");
	for (GtkWidget *label = page->title; label; label = label == page->title ? page->detail : NULL) {
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_box_append(GTK_BOX(labels), label);
	}
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_margin_top(buttons, 12);
	page->play_button = create_pill("media-playback-start-symbolic", TEXT_PLAY, page);
	gtk_widget_add_css_class(page->play_button, "suggested-action");
	page->shuffle_button = create_pill("media-playlist-shuffle-symbolic", TEXT_RANDOM, page);
	gtk_box_append(GTK_BOX(buttons), page->play_button);
	gtk_box_append(GTK_BOX(buttons), page->shuffle_button);
	gtk_box_append(GTK_BOX(buttons), create_search_button(page));
	gtk_box_append(GTK_BOX(labels), buttons);
	gtk_box_append(GTK_BOX(page->heading), labels);
	/* A clamp holds the text to one exact width, empty or full, since a label grows to fit the text it is given and
	 * the title and picture beside it would move */
	GtkWidget *about_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
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
	adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(room), page->heading);
	AdwBreakpoint *narrow = adw_breakpoint_new(adw_breakpoint_condition_parse(ABOUT_ROOM));
	GValue hidden = G_VALUE_INIT;
	g_value_init(&hidden, G_TYPE_BOOLEAN);
	adw_breakpoint_add_setter(narrow, G_OBJECT(about_slot), "visible", &hidden);
	adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(room), narrow);
	GtkWidget *veil = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(veil, "page-veil");
	page->heading_clamp = create_clamp(room);
	gtk_box_append(GTK_BOX(veil), page->heading_clamp);
	/* Every banner is this tall whatever it holds, so moving between pages or loading one never changes its size */
	gtk_widget_set_size_request(veil, -1, BANNER_HEIGHT);
	gtk_widget_set_valign(page->heading_clamp, GTK_ALIGN_CENTER);
	gtk_widget_set_vexpand(page->heading_clamp, TRUE);
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


/* A long playlist scrolls only its songs under a fixed heading, so thousands of rows stay quick, while
 * artists, albums and search results scroll as one page */
static GtkWidget *create_body(SongPage *page) {
	GtkWidget *scrolled = gtk_scrolled_window_new();
	page->scrolled = scrolled;
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	add_smooth_scrolling(GTK_SCROLLED_WINDOW(scrolled));
	if (page->is_long) {
		GtkWidget *clamp = adw_clamp_scrollable_new();
		adw_clamp_scrollable_set_maximum_size(ADW_CLAMP_SCROLLABLE(clamp), CONTENT_MAXIMUM_WIDTH);
		adw_clamp_scrollable_set_tightening_threshold(ADW_CLAMP_SCROLLABLE(clamp), CONTENT_TIGHTENING_WIDTH);
		adw_clamp_scrollable_set_child(ADW_CLAMP_SCROLLABLE(clamp), page->list);
		gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), clamp);
		return scrolled;
	}
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_add_css_class(box, "page-body");
	gtk_box_append(GTK_BOX(box), page->cards);
	page->songs_heading = create_section_heading(TEXT_SONGS);
	gtk_box_append(GTK_BOX(box), page->songs_heading);
	gtk_box_append(GTK_BOX(box), page->list);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), create_clamp(box));
	return scrolled;
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
	page->cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
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
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(skeleton), create_skeleton(page));
	gtk_stack_add_named(GTK_STACK(page->stack), skeleton, "loading");
	gtk_stack_add_named(GTK_STACK(page->stack), create_body(page), "songs");
	page->empty = create_status("view-list-symbolic", TEXT_EMPTY_PLAYLIST);
	gtk_stack_add_named(GTK_STACK(page->stack), page->empty, "empty");
	gtk_box_append(GTK_BOX(page->root), page->stack);
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
	gtk_label_set_text(GTK_LABEL(page->detail), detail ? detail : "");
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
	/* Over an artist's picture the heading spans the whole width, its title and buttons near the left edge, lifted
	 * off the picture by a soft shadow */
	adw_clamp_set_maximum_size(ADW_CLAMP(page->heading_clamp), is_banner ? G_MAXINT : CONTENT_MAXIMUM_WIDTH);
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


void set_song_page_suggestions(SongPage *page, GListModel *suggestions) {
	g_set_object(&page->suggestions, suggestions);
	set_song_list_suggestions(page->list, page->query ? NULL : suggestions);
}


/* The songs heading only shows above songs that share the page with artists or albums */
void update_song_page(SongPage *page, gboolean is_loaded) {
	page->is_loaded = is_loaded;
	guint count = page->songs ? g_list_model_get_n_items(page->songs) : 0;
	guint shown_count = g_list_model_get_n_items(G_LIST_MODEL(page->shown_songs));
	gboolean has_cards = gtk_widget_get_first_child(page->cards) != NULL;
	const char *shown = count || has_cards ? "songs" : !is_loaded ? "loading" : "empty";
	gtk_stack_set_visible_child_name(GTK_STACK(page->stack), shown);
	if (page->songs_heading)
		gtk_widget_set_visible(page->songs_heading, shown_count && has_cards);
	gtk_widget_set_visible(page->list, shown_count > 0);
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
	gtk_drag_source_set_icon(source, gtk_widget_paintable_new(button), (int)x, (int)y);
	return gdk_content_provider_new_typed(CRIMSON_TYPE_CARD, g_object_get_data(G_OBJECT(button), "card"));
}


static GtkWidget *create_card(Card *card, gboolean is_round, SongPage *page) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *cover = create_cover(CARD_ARTWORK_SIZE);
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


GtkWidget *add_song_page_cards(SongPage *page, const char *title, GPtrArray *cards, gboolean is_round,
	gboolean is_first) {
	if (!cards->len)
		return NULL;
	GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_box_append(GTK_BOX(section), create_section_heading(title));
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_add_css_class(row, "card-row");
	for (guint index = 0; index < cards->len; index++) {
		GtkWidget *card = create_card(g_ptr_array_index(cards, index), is_round, page);
		stagger_entrance(card, index);
		gtk_box_append(GTK_BOX(row), card);
	}
	GtkWidget *scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), row);
	add_smooth_scrolling(GTK_SCROLLED_WINDOW(scrolled));
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


/* A place still being scrolled back to counts as where the page is, so leaving at once never loses it */
double get_song_page_scroll(SongPage *page) {
	if (page->scroll_tick)
		return page->wanted_scroll;
	return gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(page->scrolled)));
}


/* The songs may still be loading, so the place is set again each frame until the list is long enough for it, and
 * given up only when the listener scrolls somewhere else themselves */
static gboolean keep_scroll(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
	SongPage *page = data;
	GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(page->scrolled));
	gint64 now = gdk_frame_clock_get_frame_time(clock);
	gboolean is_settled = now >= page->scroll_settled;
	double value = gtk_adjustment_get_value(adjustment);
	gboolean is_moved = is_settled && page->has_set_scroll && fabs(value - page->set_scroll) > 1;
	if (!is_moved) {
		gtk_adjustment_set_value(adjustment, page->wanted_scroll);
		page->set_scroll = gtk_adjustment_get_value(adjustment);
		page->has_set_scroll = TRUE;
	}
	gboolean is_reached = page->set_scroll >= page->wanted_scroll - 1;
	if (!is_moved && (!is_reached || !is_settled))
		return G_SOURCE_CONTINUE;
	page->scroll_tick = 0;
	return G_SOURCE_REMOVE;
}


void set_song_page_scroll(SongPage *page, double value) {
	if (page->scroll_tick)
		gtk_widget_remove_tick_callback(page->scrolled, page->scroll_tick);
	page->wanted_scroll = value;
	page->has_set_scroll = FALSE;
	/* The list puts itself back at the top a frame or two after it changes songs, so the place is held a moment */
	page->scroll_settled = g_get_monotonic_time() + SCROLL_HOLD_MICROSECONDS;
	gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(page->scrolled)), value);
	page->scroll_tick = gtk_widget_add_tick_callback(page->scrolled, keep_scroll, page, NULL);
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
	gtk_label_set_text(GTK_LABEL(page->about_facts), has_facts ? facts : "");
	gtk_widget_set_visible(page->about, TRUE);
	gtk_widget_remove_css_class(page->about, "waiting");
}


void reveal_song_page_song(SongPage *page, const char *video_id) {
	reveal_song_in_list(page->list, video_id);
}

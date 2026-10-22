#include <adwaita.h>

#include "../lang/en.h"
#include "common/smooth-scroll.h"
#include "lyrics-page.h"

#define LYRICS_WIDTH 640
#define PAGE_KEY "lyrics-parts"
#define START_KEY "lyric-start"
#define CURRENT_CLASS "current"
/* The page glides this long to bring the next line to its middle */
#define FOLLOW_MILLISECONDS 450

/* The widgets a lyrics page fills in, and the line lit now */
typedef struct {
	GtkWidget *title, *artist, *lines, *source, *empty, *stack, *scroller;
	GtkWidget *current;
	LyricChosen chosen;
	gpointer data;
} LyricsParts;


void free_lyric_line(LyricLine *line) {
	g_free(line->text);
	g_free(line);
}


static GtkWidget *create_label(const char *style) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	if (style)
		gtk_widget_add_css_class(label, style);
	return label;
}


GtkWidget *create_lyrics_page(LyricChosen chosen, gpointer data) {
	LyricsParts *parts = g_new0(LyricsParts, 1);
	parts->chosen = chosen;
	parts->data = data;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_add_css_class(box, "lyrics");
	parts->title = create_label("title-1");
	parts->artist = create_label("dimmed");
	parts->lines = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(parts->lines, "lyric-lines");
	parts->source = create_label("caption");
	gtk_widget_add_css_class(parts->source, "dimmed");
	gtk_box_append(GTK_BOX(box), parts->title);
	gtk_box_append(GTK_BOX(box), parts->artist);
	gtk_box_append(GTK_BOX(box), parts->lines);
	gtk_box_append(GTK_BOX(box), parts->source);
	GtkWidget *clamp = adw_clamp_new();
	adw_clamp_set_maximum_size(ADW_CLAMP(clamp), LYRICS_WIDTH);
	adw_clamp_set_child(ADW_CLAMP(clamp), box);
	parts->scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(parts->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	set_scroll_child(GTK_SCROLLED_WINDOW(parts->scroller), clamp);
	add_smooth_scrolling(GTK_SCROLLED_WINDOW(parts->scroller));
	parts->empty = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(parts->empty), "audio-input-microphone-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(parts->empty), TEXT_NO_LYRICS);
	parts->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(parts->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(parts->stack), parts->scroller, "lyrics");
	gtk_stack_add_named(GTK_STACK(parts->stack), parts->empty, "empty");
	gtk_widget_add_css_class(parts->stack, "view");
	g_object_set_data_full(G_OBJECT(parts->stack), PAGE_KEY, parts, g_free);
	return parts->stack;
}


static void choose_line(GtkGestureClick *gesture, int presses, double x, double y, LyricsParts *parts) {
	GtkWidget *line = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	gint64 start = *(gint64 *)g_object_get_data(G_OBJECT(line), START_KEY);
	parts->chosen(start / 1000.0, parts->data);
}


/* Untimed words get the same lines as timed ones, all lit and still, with a gap where a verse ends */
static void fill_plain_lines(LyricsParts *parts, const char *text) {
	g_auto(GStrv) lines = g_strsplit(text ? text : "", "\n", -1);
	for (guint index = 0; lines[index]; index++) {
		GtkWidget *label = create_label(*lines[index] ? "lyric-line" : "lyric-gap");
		gtk_widget_add_css_class(label, "plain");
		gtk_label_set_text(GTK_LABEL(label), lines[index]);
		gtk_label_set_selectable(GTK_LABEL(label), TRUE);
		gtk_box_append(GTK_BOX(parts->lines), label);
	}
}


static void fill_lines(LyricsParts *parts, GPtrArray *lines, const char *text) {
	parts->current = NULL;
	for (GtkWidget *child = gtk_widget_get_first_child(parts->lines); child;
		child = gtk_widget_get_first_child(parts->lines))
		gtk_box_remove(GTK_BOX(parts->lines), child);
	if (!lines || !lines->len) {
		fill_plain_lines(parts, text);
		return;
	}
	for (guint index = 0; index < lines->len; index++) {
		const LyricLine *line = g_ptr_array_index(lines, index);
		GtkWidget *label = create_label("lyric-line");
		gtk_label_set_text(GTK_LABEL(label), line->text);
		gint64 *start = g_new(gint64, 1);
		*start = line->start;
		g_object_set_data_full(G_OBJECT(label), START_KEY, start, g_free);
		gtk_widget_set_cursor_from_name(label, "pointer");
		GtkGesture *click = gtk_gesture_click_new();
		g_signal_connect(click, "released", G_CALLBACK(choose_line), parts);
		gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(click));
		gtk_box_append(GTK_BOX(parts->lines), label);
	}
}


void show_lyrics(GtkWidget *page, const char *title, const char *artist, const char *text, const char *source,
	GPtrArray *lines) {
	LyricsParts *parts = g_object_get_data(G_OBJECT(page), PAGE_KEY);
	gboolean is_timed = lines && lines->len > 0;
	gtk_label_set_text(GTK_LABEL(parts->title), title ? title : "");
	gtk_label_set_text(GTK_LABEL(parts->artist), artist ? artist : "");
	fill_lines(parts, lines, text);
	gtk_label_set_text(GTK_LABEL(parts->source), source ? source : "");
	gtk_widget_set_visible(parts->source, source && *source);
	adw_status_page_set_description(ADW_STATUS_PAGE(parts->empty), title);
	/* No text at all means the words are still on their way, which shows the title alone rather than none */
	gboolean has_words = is_timed || !text || *text;
	gtk_stack_set_visible_child_name(GTK_STACK(parts->stack), has_words ? "lyrics" : "empty");
	gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parts->scroller)), 0);
}


/* The line sung now is the last one started, and the page only moves when that changes */
void follow_lyrics(GtkWidget *page, double seconds) {
	LyricsParts *parts = g_object_get_data(G_OBJECT(page), PAGE_KEY);
	gint64 now = (gint64)(seconds * 1000);
	GtkWidget *sung = NULL;
	for (GtkWidget *line = gtk_widget_get_first_child(parts->lines); line; line = gtk_widget_get_next_sibling(line)) {
		gint64 *start = g_object_get_data(G_OBJECT(line), START_KEY);
		/* Untimed lines have no start and never follow the song */
		if (!start || *start > now)
			break;
		sung = line;
	}
	if (sung == parts->current)
		return;
	if (parts->current)
		gtk_widget_remove_css_class(parts->current, CURRENT_CLASS);
	parts->current = sung;
	if (!sung)
		return;
	gtk_widget_add_css_class(sung, CURRENT_CLASS);
	/* The viewport moves its content as it scrolls, so the line is measured against the content itself, whose top
	 * is the top of everything that can be scrolled to */
	graphene_point_t point;
	GtkWidget *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(parts->scroller));
	GtkWidget *content = gtk_viewport_get_child(GTK_VIEWPORT(viewport));
	if (!gtk_widget_compute_point(sung, content, &GRAPHENE_POINT_INIT(0, 0), &point))
		return;
	double page_size = gtk_adjustment_get_page_size(
		gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parts->scroller)));
	double middle = point.y + gtk_widget_get_height(sung) / 2.0 - page_size / 2;
	glide_scroller(GTK_SCROLLED_WINDOW(parts->scroller), middle, FOLLOW_MILLISECONDS);
}

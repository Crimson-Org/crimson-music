#include <adwaita.h>

#include "../lang/en.h"
#include "common/smooth-scroll.h"
#include "comments-page.h"

#define PAGE_KEY "comments-parts"
#define THREAD_KEY "comment-thread"
/* Replies sit this far in from the comment they answer */
#define REPLY_INDENT 24

/* The widgets the panel fills in, and whether more comments can be asked for */
typedef struct {
	GtkWidget *stack, *scroller, *list;
	gboolean has_more, is_asking;
	CommentsWanted wanted;
	RepliesWanted replies_wanted;
	gpointer data;
	/* The threads whose replies are on their way or can be added to, by the id their answers come back with */
	GHashTable *threads;
	guint next_id;
} CommentsParts;

/* A comment's replies, the button that asks for the next of them and where those start */
typedef struct {
	CommentsParts *parts;
	GtkWidget *replies, *more;
	char *token;
	guint id;
} CommentThread;


static void free_thread(CommentThread *thread) {
	g_free(thread->token);
	g_free(thread);
}


void free_comment(Comment *comment) {
	g_free(comment->author);
	g_free(comment->text);
	g_free(comment->published);
	g_free(comment->likes);
	g_free(comment->replies);
	g_free(comment->replies_token);
	g_free(comment);
}


static GtkWidget *create_label(const char *text, const char *style) {
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	if (style)
		gtk_widget_add_css_class(label, style);
	return label;
}


static void open_replies(GtkButton *button, CommentThread *thread) {
	gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
	thread->parts->replies_wanted(thread->token, thread->id, thread->parts->data);
}


/* A thread's replies start behind a button naming how many there are, which then asks for each further batch */
static GtkWidget *create_thread(CommentsParts *parts, const Comment *comment) {
	CommentThread *thread = g_new0(CommentThread, 1);
	*thread = (CommentThread){parts, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), NULL, g_strdup(comment->replies_token),
		++parts->next_id};
	g_autofree char *label = g_strdup_printf(TEXT_COMMENT_REPLIES, comment->replies);
	thread->more = gtk_button_new_with_label(label);
	gtk_widget_add_css_class(thread->more, "flat");
	gtk_widget_add_css_class(thread->more, "reply-button");
	gtk_widget_set_halign(thread->more, GTK_ALIGN_START);
	g_signal_connect(thread->more, "clicked", G_CALLBACK(open_replies), thread);
	gtk_widget_set_margin_start(thread->replies, REPLY_INDENT);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(box), thread->replies);
	gtk_box_append(GTK_BOX(box), thread->more);
	g_object_set_data_full(G_OBJECT(box), THREAD_KEY, thread, (GDestroyNotify)free_thread);
	g_hash_table_insert(parts->threads, GUINT_TO_POINTER(thread->id), thread);
	return box;
}


/* The author and when they wrote above the comment, and its likes below it */
static GtkWidget *create_comment(const Comment *comment) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_add_css_class(box, "comment");
	GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *author = create_label(comment->author, "heading");
	gtk_label_set_ellipsize(GTK_LABEL(author), PANGO_ELLIPSIZE_END);
	gtk_label_set_wrap(GTK_LABEL(author), FALSE);
	gtk_box_append(GTK_BOX(heading), author);
	GtkWidget *published = create_label(comment->published, "dimmed");
	gtk_widget_add_css_class(published, "caption");
	gtk_label_set_wrap(GTK_LABEL(published), FALSE);
	gtk_box_append(GTK_BOX(heading), published);
	gtk_box_append(GTK_BOX(box), heading);
	GtkWidget *text = create_label(comment->text, NULL);
	gtk_label_set_selectable(GTK_LABEL(text), TRUE);
	gtk_box_append(GTK_BOX(box), text);
	GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_add_css_class(footer, "dimmed");
	if (comment->likes && *comment->likes) {
		gtk_box_append(GTK_BOX(footer), gtk_image_new_from_icon_name("crimson-like-symbolic"));
		gtk_box_append(GTK_BOX(footer), create_label(comment->likes, "caption"));
	}
	gtk_box_append(GTK_BOX(box), footer);
	return box;
}


static void ask_for_more(CommentsParts *parts) {
	if (!parts->has_more || parts->is_asking)
		return;
	parts->is_asking = TRUE;
	parts->wanted(parts->data);
}


/* Reaching the end of the list asks for the next comments, as YouTube's own page does */
static void reach_edge(GtkScrolledWindow *scroller, GtkPositionType position, CommentsParts *parts) {
	if (position == GTK_POS_BOTTOM)
		ask_for_more(parts);
}


static void free_parts(CommentsParts *parts) {
	g_hash_table_unref(parts->threads);
	g_free(parts);
}


GtkWidget *create_comments_page(CommentsWanted wanted, RepliesWanted replies_wanted, gpointer data) {
	CommentsParts *parts = g_new0(CommentsParts, 1);
	parts->wanted = wanted;
	parts->replies_wanted = replies_wanted;
	parts->threads = g_hash_table_new(NULL, NULL);
	parts->data = data;
	GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(panel, "comments");
	GtkWidget *title = create_label(TEXT_COMMENTS, "title-4");
	gtk_widget_add_css_class(title, "comments-title");
	gtk_box_append(GTK_BOX(panel), title);
	parts->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	parts->scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(parts->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	set_scroll_child(GTK_SCROLLED_WINDOW(parts->scroller), parts->list);
	add_smooth_scrolling(GTK_SCROLLED_WINDOW(parts->scroller));
	g_signal_connect(parts->scroller, "edge-reached", G_CALLBACK(reach_edge), parts);
	GtkWidget *empty = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "crimson-comments-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(empty), TEXT_NO_COMMENTS);
	gtk_widget_add_css_class(empty, "compact");
	parts->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(parts->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(parts->stack), adw_spinner_new(), "loading");
	gtk_stack_add_named(GTK_STACK(parts->stack), parts->scroller, "comments");
	gtk_stack_add_named(GTK_STACK(parts->stack), empty, "empty");
	gtk_widget_set_vexpand(parts->stack, TRUE);
	gtk_box_append(GTK_BOX(panel), parts->stack);
	g_object_set_data_full(G_OBJECT(panel), PAGE_KEY, parts, (GDestroyNotify)free_parts);
	return panel;
}


void clear_comments(GtkWidget *page) {
	CommentsParts *parts = g_object_get_data(G_OBJECT(page), PAGE_KEY);
	g_hash_table_remove_all(parts->threads);
	for (GtkWidget *child = gtk_widget_get_first_child(parts->list); child;
		child = gtk_widget_get_first_child(parts->list))
		gtk_box_remove(GTK_BOX(parts->list), child);
	parts->has_more = FALSE;
	parts->is_asking = FALSE;
	gtk_stack_set_visible_child_name(GTK_STACK(parts->stack), "loading");
	gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parts->scroller)), 0);
}


void add_comments(GtkWidget *page, GPtrArray *comments, gboolean has_more) {
	CommentsParts *parts = g_object_get_data(G_OBJECT(page), PAGE_KEY);
	for (guint index = 0; index < comments->len; index++) {
		const Comment *comment = g_ptr_array_index(comments, index);
		gtk_box_append(GTK_BOX(parts->list), create_comment(comment));
		if (comment->replies_token && *comment->replies_token && comment->replies && *comment->replies)
			gtk_box_append(GTK_BOX(parts->list), create_thread(parts, comment));
	}
	parts->has_more = has_more;
	parts->is_asking = FALSE;
	gboolean has_comments = gtk_widget_get_first_child(parts->list) != NULL;
	gtk_stack_set_visible_child_name(GTK_STACK(parts->stack), has_comments ? "comments" : "empty");
}


void add_replies(GtkWidget *page, guint id, GPtrArray *replies, const char *next) {
	CommentsParts *parts = g_object_get_data(G_OBJECT(page), PAGE_KEY);
	CommentThread *thread = g_hash_table_lookup(parts->threads, GUINT_TO_POINTER(id));
	if (!thread)
		return;
	for (guint index = 0; index < replies->len; index++)
		gtk_box_append(GTK_BOX(thread->replies), create_comment(g_ptr_array_index(replies, index)));
	g_free(thread->token);
	thread->token = g_strdup(next);
	gboolean has_more = next && *next;
	gtk_button_set_label(GTK_BUTTON(thread->more), TEXT_MORE_REPLIES);
	gtk_widget_set_sensitive(thread->more, TRUE);
	gtk_widget_set_visible(thread->more, has_more);
}

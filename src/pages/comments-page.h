#pragma once

#include <gtk/gtk.h>

/* One comment under a video, with what YouTube shows beside it as text */
typedef struct {
	char *author, *text, *published, *likes, *replies;
	/* Where the comment's replies start, or empty for a reply or a comment without any */
	char *replies_token;
} Comment;

/* Called when the listener has scrolled to the last comment and more can follow */
typedef void (*CommentsWanted)(gpointer data);
/* Called when the listener opens a thread's replies, with the id their answer is handed back with */
typedef void (*RepliesWanted)(const char *token, guint id, gpointer data);

void free_comment(Comment *comment);

/* The panel at the right of the window that lists the comments under the playing video */
GtkWidget *create_comments_page(CommentsWanted wanted, RepliesWanted replies_wanted, gpointer data);
/* Empties the panel and shows it loading, for a song whose comments are on their way */
void clear_comments(GtkWidget *page);
/* Adds comments below those shown, and says whether YouTube has more after them */
void add_comments(GtkWidget *page, GPtrArray *comments, gboolean has_more);
/* Adds replies under the thread the id belongs to, with a button for more when the token is not empty */
void add_replies(GtkWidget *page, guint id, GPtrArray *replies, const char *next);

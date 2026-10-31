#pragma once

#include <gtk/gtk.h>

/* One sung line and when it starts, in milliseconds from the start of the song */
typedef struct {
	char *text;
	gint64 start;
} LyricLine;

/* Called with the second a clicked line starts at */
typedef void (*LyricChosen)(double seconds, gpointer data);

void free_lyric_line(LyricLine *line);

/* The words of the playing song across the middle of the window, under its title and artist, with timed lines
 * following the song and leading to their place when clicked */
GtkWidget *create_lyrics_page(LyricChosen chosen, gpointer data);
/* Shows timed lines when there are any, the plain text otherwise, and that YouTube has none when both are empty,
 * while a NULL text shows the title alone as the words are still coming */
void show_lyrics(GtkWidget *page, const char *title, const char *artist, const char *text, const char *source,
	GPtrArray *lines);
/* Lights the line being sung and keeps it in the middle of the page */
void follow_lyrics(GtkWidget *page, double seconds);

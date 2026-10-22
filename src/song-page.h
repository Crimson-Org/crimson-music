#pragma once

#include <gtk/gtk.h>

#include "song-list.h"

/* What a page asks of its owner */
typedef struct {
	/* Plays the page's songs from the position, shuffled when asked */
	void (*play)(GListModel *songs, guint position, gboolean is_shuffled, gpointer data);
	/* A click on one song, which the song already playing does not start over */
	void (*choose)(GListModel *songs, guint position, gpointer data);
	/* Opens an artist or album card by its id */
	void (*open_card)(const char *id, gpointer data);
	/* Opens the menu of a card that was right clicked, at that spot of the card */
	void (*open_card_menu)(const char *id, GtkWidget *card, double x, double y, gpointer data);
	SongListEvents list;
} SongPageEvents;

/* A heading with a cover, rows of artist or album cards and a list of songs, used for playlists, artists,
 * albums and search results alike */
typedef struct SongPage SongPage;

/* A long page is a playlist, whose songs scroll under a fixed heading */
SongPage *create_song_page(const SongPageEvents *events, gpointer data, gboolean is_long);
GtkWidget *get_song_page_widget(SongPage *page);
GListModel *get_song_page_songs(SongPage *page);
/* An artist or album page can be pinned by dragging its cover or title to the sidebar */
void set_song_page_pin(SongPage *page, const char *id, const char *title, const char *artwork);
/* No title hides the heading, and a round cover is for artists */
void set_song_page_heading(SongPage *page, const char *title, const char *detail, const char *artwork,
	gboolean is_round);
/* Sliding the page in is only for a page that now shows something else, never for a refresh */
void set_song_page_songs(SongPage *page, GListModel *songs, gboolean is_reorderable, gboolean is_entering);
/* Songs to add, after the page's own, or none for NULL */
void set_song_page_suggestions(SongPage *page, GListModel *suggestions);
/* Shows a spinner until the songs are known, then the songs or the empty state */
void update_song_page(SongPage *page, gboolean is_loaded);
/* Shows hearts only under the pointer, for a list where every song is liked */
void set_song_page_quiet_likes(SongPage *page, gboolean is_quiet);
void remove_song_page_cards(SongPage *page, GtkWidget *section);
void set_song_page_empty_title(SongPage *page, const char *title);
/* Makes a round picture a round cover beside the title, as for the listener's own picture, not an artist's banner */
void set_song_page_profile(SongPage *page);
void set_song_page_empty(SongPage *page, const char *icon, const char *title, const char *detail);
/* Adds a titled row of cards at the top or the bottom and returns it for removing later, or NULL for no cards */
GtkWidget *add_song_page_cards(SongPage *page, const char *title, GPtrArray *cards, gboolean is_round,
	gboolean is_first);
/* Shows the shelves in order, leaving the ones whose cards did not change as they are, so a refresh never jolts
 * the page, while a changed shelf crossfades to its new cards and shelves that come or go slide open or shut */
void set_song_page_shelves(SongPage *page, GPtrArray *shelves);
/* All the cards wrapped in rows the page's width, for a page made of cards alone */
void add_song_page_card_grid(SongPage *page, GPtrArray *cards, gboolean is_round);
/* The waiting page shows a row of card shapes above its song shapes, for pages whose results bring cards */
void set_song_page_waits_for_cards(SongPage *page, gboolean is_waiting);
/* Play and Random only belong on pages with songs */
void set_song_page_playable(SongPage *page, gboolean is_playable);
void reveal_song_page_song(SongPage *page, const char *video_id);
/* How far the songs are scrolled, and scrolling back there once the songs reach that far */
double get_song_page_scroll(SongPage *page);
/* A short text and a line of facts beside the heading, hidden without either */
void set_song_page_about(SongPage *page, const char *text, const char *facts);
/* Keeps the place for that text empty until it comes */
void keep_song_page_about(SongPage *page);
void set_song_page_scroll(SongPage *page, double value);

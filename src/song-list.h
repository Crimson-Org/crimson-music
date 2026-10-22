#pragma once

#include <gtk/gtk.h>

#include "library.h"

/* What a song list asks of its owner when a row is right clicked or dragged onto another */
typedef struct {
	void (*open_menu)(CrimsonSong *song, guint position, GtkWidget *row, double x, double y, gpointer data);
	void (*move_song)(guint from, guint to, gpointer data);
	void (*toggle_like)(CrimsonSong *song, gpointer data);
	void (*toggle_dislike)(CrimsonSong *song, gpointer data);
	void (*toggle_love)(CrimsonSong *song, gpointer data);
	/* The + of a suggested song was pressed */
	void (*add_suggestion)(CrimsonSong *song, gpointer data);
} SongListEvents;

/* A list of songs with cover, title, artist and length, where one click activates a song */
GtkWidget *create_song_list(const SongListEvents *events, gpointer data);
/* Rows can be dragged to new places only in a reorderable list */
void set_song_list_songs(GtkWidget *list, GListModel *songs, gboolean is_reorderable);
void set_song_list_reorderable(GtkWidget *list, gboolean is_reorderable);
/* Songs offered after the list's own under a heading of their own, each with a button that adds it, or none for
 * NULL */
void set_song_list_suggestions(GtkWidget *list, GListModel *suggestions);
gboolean is_suggested_song(CrimsonSong *song);

/* A song just added slides open the next time its row shows, which is at once when it is on screen */
void mark_song_arriving(CrimsonSong *song);
/* Folds the song's row shut and calls done once it is, or at once when the row is not on screen, so the song can
 * leave its list without the rows below jumping */
typedef void (*SongFolded)(CrimsonSong *song, gpointer data);
void fold_song_away(CrimsonSong *song, SongFolded done, gpointer data);

/* Marks the rows of this song in every list, or none for NULL */
void set_playing_video(const char *video_id);
/* Scrolls the list to the song and lights it for a moment */
void reveal_song_in_list(GtkWidget *list, const char *video_id);
/* Marks the widget as playing whenever its song plays, for song cards outside the lists */
void follow_playing_video(GtkWidget *widget, const char *video_id);

/* The ids of the songs in Liked Music, whose rows show a filled heart, read again on every call */
void set_liked_videos(GHashTable *video_ids);
gboolean is_video_liked(const char *video_id);
/* Shows every row's dislike again, after the disliked songs changed */
void update_disliked_rows(void);

/* Hearts show the songs loved on Last.fm, and only while it is connected, which the check tells */
typedef gboolean (*SongLoveCheck)(CrimsonSong *song);
void set_song_love_check(SongLoveCheck check, gboolean is_shown);

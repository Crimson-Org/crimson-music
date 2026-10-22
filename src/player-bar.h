#pragma once

#include <adwaita.h>

#include "library.h"
#include "song-info.h"

/* What the player bar asks of the window when the listener drags the position or volume */
typedef struct {
	void (*seek)(double seconds, gpointer data);
	void (*change_volume)(int volume, gpointer data);
	/* A click on the title goes to where the song plays from, and one on an artist's name to that artist, whose
	 * id is empty for a song that names its artist only as text */
	void (*open_source)(gpointer data);
	void (*open_artist)(const char *artist_id, gpointer data);
	/* A right click on the song asks for its menu at that spot of the widget */
	void (*open_menu)(GtkWidget *widget, double x, double y, gpointer data);
	void (*toggle_like)(gpointer data);
	void (*toggle_dislike)(gpointer data);
	void (*toggle_love)(gpointer data);
	/* A song dropped on the upcoming covers is to play next */
	void (*queue_song)(CrimsonSong *song, gpointer data);
} PlayerBarEvents;

/* The bar along the bottom with the current song, its controls and its position */
typedef struct PlayerBar PlayerBar;

/* The buttons run the window actions play-pause, previous, next and shuffle */
PlayerBar *create_player_bar(const PlayerBarEvents *events, gpointer data, int volume);
GtkWidget *get_player_bar_widget(PlayerBar *bar);
void show_player_song(PlayerBar *bar, CrimsonSong *song);
void show_player_time(PlayerBar *bar, double time, double duration);
/* The position takes no clicks or scrolls until the song has begun playing */
void set_player_seekable(PlayerBar *bar, gboolean is_seekable);
/* Local songs are not on YouTube, so their bar has no Like button */
void set_player_likable(PlayerBar *bar, gboolean is_likable);
/* The Lyrics button shows only for a song YouTube has words for */
void set_player_has_lyrics(PlayerBar *bar, gboolean has_lyrics);
/* The camera button shows only while the song is a music video */
void set_player_has_video(PlayerBar *bar, gboolean has_video);
/* The comments button shows only while the song has comments */
void set_player_has_comments(PlayerBar *bar, gboolean has_comments);
void show_player_liked(PlayerBar *bar, gboolean is_liked);
void show_player_disliked(PlayerBar *bar, gboolean is_disliked);
/* The heart beside the thumb, shown only while Last.fm is connected */
void show_player_loved(PlayerBar *bar, gboolean is_shown, gboolean is_loved);
/* Colours the bar after the playing song's cover, or keeps it plain */
void set_player_colored(PlayerBar *bar, gboolean is_colored);
/* The songs coming up, as covers beside the Next button and as the queue it opens */
void show_player_upcoming(PlayerBar *bar, GPtrArray *songs, guint queued_count);
/* Turns the volume up for positive steps and down for negative ones, as the volume button would */
void step_player_volume(PlayerBar *bar, int steps);
/* A button beside the heart lists the song's chapters, and only shows when there are some */
void show_player_chapters(PlayerBar *bar, GPtrArray *chapters);
void show_player_playing(PlayerBar *bar, gboolean is_playing, gboolean is_waiting);

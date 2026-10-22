#pragma once

#include "library.h"

/* The songs that play next, in playlist or shuffled order */
typedef struct {
	GPtrArray *songs;
	GArray *order;
	/* Whether each place in the order was queued by the listener with Play Next, alongside the order */
	GArray *queued;
	guint position;
	gboolean is_shuffled;
} Queue;

Queue *create_queue(void);
/* Starts from the song at the index of the list, which shuffling keeps first */
void fill_queue(Queue *queue, GListModel *songs, guint start, gboolean is_shuffled);
void clear_queue(Queue *queue);
void shuffle_queue(Queue *queue, gboolean is_shuffled);
CrimsonSong *get_queue_song(Queue *queue);
/* Puts the song right after the playing one, or after any queued before it as well, whatever the order */
void queue_song_next(Queue *queue, CrimsonSong *song, gboolean is_last);
/* Moves one song forward or back and returns it, or NULL past either end */
CrimsonSong *step_queue(Queue *queue, int step);
/* Up to so many of the songs after the playing one, in the order they will play */
GPtrArray *list_upcoming(Queue *queue, guint limit);
/* Whether the song so many places after the playing one was queued with Play Next */
gboolean is_upcoming_queued(Queue *queue, guint offset);
/* Takes the song so many places after the playing one out of what plays */
void remove_upcoming(Queue *queue, guint offset);
/* Moves an upcoming song from one place after the playing one to another, keeping whether it was queued */
void move_upcoming(Queue *queue, guint from, guint to);
/* Marks the next so many songs after the playing one as queued with Play Next */
void mark_upcoming_queued(Queue *queue, guint count);

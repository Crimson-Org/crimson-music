#pragma once

#include "library.h"

/* The song the listener had up and the songs up next after it, of which the first are the ones they queued, with
 * where the song played from */
typedef struct {
	CrimsonSong *song;
	GPtrArray *upcoming;
	guint queued;
	char *page_tag, *playlist_id;
} LastSong;

/* Keeps them for the next launch, or forgets them for a NULL song */
void save_last_song(CrimsonSong *song, GPtrArray *upcoming, guint queued, const char *page_tag,
	const char *playlist_id);
/* What was kept, or NULL */
LastSong *load_last_song(void);
void free_last_song(LastSong *last);

#pragma once

#include "library.h"

/* Every Discover Mix YouTube Music made for the listener, kept in the data folder, since YouTube fills the one mix
 * with new songs again and again */
#define DISCOVER_MIX_PREFIX "discover-mix:"

/* Keeps the songs the mix has now, with the cover and subtitle of its card, as a mix of their own unless the same
 * songs were kept before, and gives cards for every kept mix, the one it has now first and then newest first */
GPtrArray *keep_discover_mix(GPtrArray *songs, const Card *card);
/* The songs of the kept mix with the card id, with its subtitle and its cover */
GPtrArray *load_discover_mix(const char *id, char **detail, char **artwork);
/* The mixes belong to the account they were made for */
void forget_discover_mixes(void);

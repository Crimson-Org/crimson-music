#pragma once

#include "library.h"

#define DYNAMIC_PREFIX "dynamic:"

/* The kinds of values a song can be included or left out by, each shown as a row of chips */
typedef enum {
	FACET_GENRE,
	FACET_MOOD,
	FACET_ARTIST,
	FACET_TAG,
	FACET_COUNT,
} Facet;

/* The kinds of numbers a song can be kept within */
typedef enum {
	RANGE_TEMPO,
	RANGE_YEAR,
	RANGE_LENGTH,
	RANGE_COUNT,
} RangeKind;

typedef struct {
	gboolean is_on;
	double minimum, maximum;
} Range;

/* A playlist of the liked songs that pass its filters, kept on this computer */
typedef struct {
	char *id, *name;
	/* Values as "facet:value" in lowercase, where a song needs one included value of each facet that has any,
	 * and no left out value at all */
	GHashTable *included, *excluded;
	Range ranges[RANGE_COUNT];
} DynamicRules;

DynamicRules *create_dynamic_rules(void);
DynamicRules *copy_dynamic_rules(const DynamicRules *rules);
void free_dynamic_rules(gpointer rules);

GPtrArray *load_dynamic_playlists(void);
void save_dynamic_playlists(GPtrArray *playlists);

/* The facet values of a song in lowercase, which are the keys the rules hold, without the facet name */
GPtrArray *get_song_facet_values(CrimsonSong *song, Facet facet);
char *make_facet_key(Facet facet, const char *value);
/* The song's number for a range, or a negative one when it is not known */
double get_song_range_value(CrimsonSong *song, RangeKind kind);

gboolean is_song_matching(CrimsonSong *song, const DynamicRules *rules);
/* The songs of the pool that pass, in the pool's order */
GPtrArray *filter_dynamic_songs(GListModel *pool, const DynamicRules *rules);

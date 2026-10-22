#pragma once

#include "library.h"

/* What is known of a song, gathered from YouTube, Deezer, MusicBrainz and ReccoBeats, all kept as text the way
 * data/song-info.js gives it, with numbers as digits and lists joined by commas */
typedef enum {
	INFO_TITLE,
	INFO_ARTIST,
	INFO_ALBUM,
	INFO_SECONDS,
	INFO_RELEASED,
	INFO_UPLOADED,
	INFO_BPM,
	INFO_KEY,
	INFO_GAIN,
	INFO_ISRC,
	INFO_EXPLICIT,
	INFO_LABEL,
	INFO_GENRES,
	INFO_TAGS,
	INFO_TAG_SOURCE,
	/* Genres named in the upload's YouTube keywords, or in its title when no music service knows it */
	INFO_STYLES,
	/* Genres and moods listeners tagged the song with on Last.fm, or its artist when the song has none */
	INFO_LASTFM,
	INFO_VIEWS,
	INFO_CHANNEL,
	INFO_DESCRIPTION,
	INFO_CHAPTERS,
	INFO_SOURCES,
	/* The album cover Deezer has, for songs that carry no picture of their own */
	INFO_COVER,
	/* Levels from 0 to 100 from the ReccoBeats analysis, or -1 when it does not know the song */
	INFO_DANCEABLE,
	INFO_ENERGETIC,
	INFO_CALM,
	INFO_HAPPY,
	INFO_SAD,
	INFO_ACOUSTIC,
	INFO_INSTRUMENTAL,
	INFO_FIELD_COUNT,
} InfoField;

#define INFO_FIRST_MOOD INFO_DANCEABLE

typedef struct {
	char *values[INFO_FIELD_COUNT];
	/* The version of data/song-info.js that gathered it, so songs gathered by an older one are asked for again */
	int version;
} SongInfo;

/* Called on the main thread whenever a song's information or its own tags change */
typedef void (*SongInfoChanged)(const char *video_id, gpointer data);

/* Opens the store in the data folder, where the listener's own tags live beside what was gathered */
void start_song_info(SongInfoChanged changed, gpointer data);
/* Gathering starts once the YouTube Music page is up, since starting JavaScriptCore here before WebKit does keeps
 * the page from ever loading */
void allow_song_info(void);
/* NULL until the song has been gathered */
const SongInfo *get_song_info(const char *video_id);
const char *get_song_info_value(const char *video_id, InfoField field);
double get_song_info_number(const char *video_id, InfoField field);

/* A named part of a long video, starting this many seconds in */
typedef struct {
	char *title;
	int start;
} Chapter;

void free_chapter(gpointer chapter);
/* The chapters of a song, empty when it has none or is not gathered yet */
GPtrArray *get_song_chapters(const char *video_id);

/* Gathers the songs not known yet in the background, one at a time, or this one before all others */
void gather_song_info(GListModel *songs);
void gather_song_info_now(CrimsonSong *song);
/* How many songs wait to be gathered, and whether this one is among them or being gathered right now */
guint count_waiting_song_info(void);
gboolean is_gathering_song_info(const char *video_id);

/* Reads the Last.fm answers fetch_lastfm_details gives into plain values by name, or NULL before the script runs */
GHashTable *read_lastfm_details(char **answers);
/* An artist's or album's summary, listeners, plays, tags and page from Last.fm's answer by name, or NULL */
GHashTable *read_lastfm_about(const char *text);
/* The loved tracks on one page of Last.fm's answer as artist and title after each other, or NULL */
char **read_lastfm_loved(const char *text, int *pages);
/* Last.fm's similar tracks as artist and title after each other, or its similar artists' names */
char **read_lastfm_similar(const char *text);

/* Drops what was gathered of a song, such as a local file whose tags changed, so it is gathered again */
void forget_song_info(const char *video_id);

/* Tags the listener gave a song themselves, kept apart so gathering again never loses them */
char **get_custom_tags(const char *video_id);
void set_custom_tags(const char *video_id, const char *const *tags);

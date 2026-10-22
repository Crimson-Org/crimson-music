#pragma once

#include <gio/gio.h>
#include <jsc/jsc.h>

/* One song, with the ids that lead to its artists, its album and its place in a playlist. The artist id holds
 * a line of id and name split by a tab for each linked artist, or one bare id named by the whole artist text */
struct _CrimsonSong {
	GObject parent;
	char *id, *title, *artist, *artist_id, *album, *album_id, *duration, *artwork;
	/* YouTube's id for this song's entry in a playlist, which removing and moving it need */
	char *set_id;
	/* LIKE, DISLIKE or INDIFFERENT as YouTube sent it with the song, or empty when it sent none */
	char *like_status;
};

#define CRIMSON_TYPE_SONG crimson_song_get_type()
G_DECLARE_FINAL_TYPE(CrimsonSong, crimson_song, CRIMSON, SONG, GObject)

/* One playlist from the library, with its songs once they are known */
struct _CrimsonPlaylist {
	GObject parent;
	char *id, *title, *subtitle, *artwork;
	GListStore *songs;
	/* The songs came from the cache or YouTube, so an empty list really is empty */
	gboolean has_songs;
	/* The listener made it, so songs can be added, removed and moved */
	gboolean is_editable;
	/* Pages of a refresh, held back until the last one so the list changes once */
	GPtrArray *incoming;
	/* Edits YouTube has not shown back yet, since it can answer with the playlist from before them: the songs
	 * added and the entry ids of the songs removed */
	GPtrArray *pending_added;
	GHashTable *pending_removed;
};

#define CRIMSON_TYPE_PLAYLIST crimson_playlist_get_type()
G_DECLARE_FINAL_TYPE(CrimsonPlaylist, crimson_playlist, CRIMSON, PLAYLIST, GObject)

/* An artist, album or song shown as a cover with a name */
typedef struct {
	char *id, *title, *subtitle, *artwork;
	/* The song a card plays, so the card shows when that song is playing, or NULL for others */
	char *video_id;
} Card;

#define CRIMSON_TYPE_CARD crimson_card_get_type()
GType crimson_card_get_type(void);

void free_card(gpointer card);
Card *copy_card(const Card *card);

/* A titled row of cards, with round covers for artists */
typedef struct {
	char *title;
	gboolean is_round;
	GPtrArray *cards;
} Shelf;

void free_shelf(gpointer shelf);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Shelf, free_shelf)

/* Home as YouTube Music last sent it, with the songs played lately and the page listing them all, the shelves and
 * the songs it picks */
typedef struct {
	char *history_id;
	GPtrArray *history, *shelves, *songs;
	/* The Discover Mix as it is now with its songs, which only a fresh answer brings */
	Card *discover;
	GPtrArray *discover_songs;
} Home;

void free_home(Home *home);

/* A length as 3:42, or 1:04:33 from an hour on */
char *format_song_length(double seconds);
/* The seconds in a song's length as YouTube Music writes it, or 0 without one */
double read_song_length(CrimsonSong *song);
/* The artists a song links to, as each one's id and name after each other, since a song can have several */
GStrv list_song_artists(CrimsonSong *song);
/* The id of the first artist a song links to, or an empty text for a song that names its artist only as text */
char *get_song_artist_id(CrimsonSong *song);

/* Turn the page's answers into objects */
GPtrArray *read_playlists(JSCValue *array);
GPtrArray *read_songs(JSCValue *array);
GPtrArray *read_cards(JSCValue *array);
Home *read_home(JSCValue *message);
/* A song as bytes to store, and back */
GBytes *save_song_bytes(CrimsonSong *song);
CrimsonSong *load_song_bytes(GBytes *bytes);

/* Songs the account dislikes, as YouTube last said with a song or as a dislike made here since */
gboolean is_video_disliked(const char *video_id);
void set_video_disliked(const char *video_id, gboolean is_disliked);

/* Swaps the items in only when they differ, so an unchanged refresh leaves the list and its scrolling alone */
gboolean replace_playlists(GListStore *store, GPtrArray *playlists);
gboolean replace_songs(CrimsonPlaylist *playlist, GPtrArray *songs);
gboolean update_song_store(GListStore *store, GPtrArray *songs);
/* Edits sent to YouTube stay on every list of the playlist until a list fresh from YouTube shows them */
void note_playlist_addition(CrimsonPlaylist *playlist, CrimsonSong *song);
void note_playlist_removal(CrimsonPlaylist *playlist, CrimsonSong *song);
void forget_playlist_edits(CrimsonPlaylist *playlist);
/* Lays the waiting edits over a list of the playlist's songs, and lets go of the ones a fresh list shows */
void apply_playlist_edits(CrimsonPlaylist *playlist, GPtrArray *songs, gboolean is_fresh);
void order_playlists(GPtrArray *playlists, const char *const *order);

/* Playlists made in Crimson Music and kept on this computer, which can hold YouTube and local songs alike */
#define LOCAL_PLAYLIST_PREFIX "crimson-playlist:"
gboolean is_local_playlist(CrimsonPlaylist *playlist);
GPtrArray *load_local_playlists(void);
/* Saves the local playlists among the store's, leaving the others to the cache */
void save_local_playlists(GListStore *store);
CrimsonPlaylist *create_local_playlist(const char *title);

/* The last library seen, so the next launch shows it before anything is fetched */
void load_cached_playlists(GListStore *store, const char *const *order);
void load_cached_songs(CrimsonPlaylist *playlist);
void save_cached_playlists(GListStore *store);
void save_cached_songs(CrimsonPlaylist *playlist);
Home *load_cached_home(void);
void save_cached_home(Home *home);
void clear_cache(void);

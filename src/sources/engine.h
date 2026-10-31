#pragma once

#include <jsc/jsc.h>
#include <webkit/webkit.h>

/* What the YouTube player reports, with times in seconds */
typedef struct {
	const char *video_id;
	int state;
	double time, duration;
	/* The song is a music video rather than audio with a still cover */
	gboolean is_video;
} PlayerReport;

/* Player states as YouTube numbers them */
enum {
	PLAYER_ENDED = 0,
	PLAYER_PLAYING = 1,
	PLAYER_PAUSED = 2,
	PLAYER_BUFFERING = 3,
};

/* Answers from the hidden page, each called on the main thread */
typedef struct {
	void (*ready)(gboolean is_signed_in, gpointer data);
	void (*playlists_arrived)(JSCValue *playlists, gpointer data);
	void (*songs_arrived)(const char *playlist_id, JSCValue *songs, gboolean is_editable, gboolean is_first,
		gboolean is_done, gpointer data);
	/* Search results, artist and album pages, a new playlist or an edited one, by their type */
	void (*answer_arrived)(const char *type, JSCValue *message, gpointer data);
	void (*player_changed)(const PlayerReport *report, gpointer data);
	void (*failed)(const char *message, const char *playlist_id, gpointer data);
	/* A media key pressed for the page, "play", "pause", "next" or "previous", which the app answers */
	void (*media_key)(const char *action, gpointer data);
} EngineEvents;

/* A YouTube Music page that never shows, which signs in, fetches the library and plays the songs */
typedef struct Engine Engine;

/* The page lives inside the host so it counts as on screen and keeps playing, while the host keeps it invisible */
Engine *create_engine(const EngineEvents *events, gpointer data);
GtkWidget *get_engine_widget(Engine *engine);
WebKitNetworkSession *get_engine_session(Engine *engine);
void start_engine(Engine *engine);
void reload_engine(Engine *engine);
void sign_out_engine(Engine *engine);

void request_library(Engine *engine);
/* Asks for YouTube Music's Home, its shelves of mixes, playlists, albums and artists and its quick picks */
void request_home(Engine *engine);
void request_playlist(Engine *engine, const char *playlist_id);
/* Songs YouTube Music suggests adding to one of the listener's own playlists, answered as suggestions */
void request_suggestions(Engine *engine, const char *playlist_id);
/* The token comes back with the results, so answers to an older query can be told apart */
void request_search(Engine *engine, const char *query, int token);
void request_artist(Engine *engine, const char *artist_id);
/* Songs or artists like one, from Last.fm's names, as artist and title after each other or as artist names, found
 * on YouTube Music, which adds its own after them */
void request_similar_songs(Engine *engine, int token, const char *video_id, char **pairs);
void request_similar_artists(Engine *engine, int token, const char *artist_id, char **names);
void request_album(Engine *engine, const char *album_id);
/* Looks up the album of a song whose list did not name it, answered as album-found */
void find_album(Engine *engine, const char *video_id);
/* The song's words from YouTube Music, answered as lyrics with an empty text when it has none */
void request_lyrics(Engine *engine, const char *video_id);
/* Asks for a video's first comments, or for those after a token an earlier answer gave */
void request_comments(Engine *engine, const char *video_id, const char *token);
/* Asks for replies from where a token says they start, answered with the id so they reach the right thread */
void request_replies(Engine *engine, const char *video_id, const char *token, guint id);
void create_playlist(Engine *engine, const char *title);
/* Adds the song to Liked Music or takes it out, answered as liked */
void like_song(Engine *engine, const char *video_id, gboolean is_liked);
/* Dislikes the song or takes the dislike away, answered as disliked */
void dislike_song(Engine *engine, const char *video_id, gboolean is_disliked);
void add_to_playlist(Engine *engine, const char *playlist_id, const char *video_id);
void remove_from_playlist(Engine *engine, const char *playlist_id, const char *video_id, const char *set_id);
void rename_playlist(Engine *engine, const char *playlist_id, const char *title);
void delete_playlist(Engine *engine, const char *playlist_id);
/* Moves the entry before the successor entry, or to the end when the successor is empty */
void move_in_playlist(Engine *engine, const char *playlist_id, const char *set_id, const char *successor_id);

/* Volume runs from 0 to 100 */
/* A song that does not resume starts over even when YouTube remembers where it was stopped */
void play_video(Engine *engine, const char *video_id, int volume, gboolean is_resuming);
void pause_video(Engine *engine);
/* Stops every sound from the page at once and stops passing its messages on, for a window that is closing */
void silence_engine(Engine *engine);
/* Called once the page shows the video alone, so whatever covered it until then can go */
typedef void (*EngineVideoDrawn)(gpointer data);
/* Fills the page with the playing video, or hides it again, while the app shows the page itself */
void show_engine_video(Engine *engine, gboolean is_shown, EngineVideoDrawn drawn);
void resume_video(Engine *engine);
void seek_video(Engine *engine, double seconds);
void set_video_volume(Engine *engine, int volume);

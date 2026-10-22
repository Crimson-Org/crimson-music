#pragma once

#include <gtk/gtk.h>

#include "library.h"

/* Called whenever the connection to Last.fm starts, finishes, fails or ends */
typedef void (*LastfmChanged)(gpointer data);

void start_lastfm(LastfmChanged changed, gpointer data);
/* The build carries an API key, without which Last.fm stays out of sight */
gboolean is_lastfm_available(void);
const char *get_lastfm_key(void);
/* The connected listener's name, or NULL */
const char *get_lastfm_user(void);
gboolean is_lastfm_connecting(void);

/* Opens Last.fm in the browser for the listener to allow Crimson Music, and waits for that */
void connect_lastfm(GtkWidget *parent);
void cancel_lastfm(void);
void disconnect_lastfm(void);

/* Tells Last.fm the song started, which it shows on the profile without keeping */
void send_now_playing(CrimsonSong *song, double duration);
/* Keeps a listen that counts, and sends it with any older ones still waiting */
void scrobble_song(CrimsonSong *song, gint64 started, double duration);
guint count_waiting_scrobbles(void);

/* The connected listener's scrobbles in total and their profile page, once Last.fm told them, or 0 and NULL */
guint64 get_lastfm_scrobbles(void);
const char *get_lastfm_profile(void);
/* The listener's profile picture, or NULL when they have none or it is still loading */
GdkTexture *get_lastfm_picture(void);

/* Last.fm's page of an artist, or of one of their albums or songs, with names written the way Last.fm writes them */
char *make_lastfm_address(const char *artist, const char *album, const char *track);
/* The main artist of a song, which is the one Last.fm knows it by */
char *get_song_lastfm_artist(CrimsonSong *song);

/* Loves are Last.fm's own, so the heart only shows while it is connected, and the loved tracks are read from it */
gboolean is_song_loved(CrimsonSong *song);
void toggle_song_love(CrimsonSong *song);
/* Reads the listener's loved tracks again, once the song information script can read Last.fm's answers */
void refresh_lastfm_loves(void);
void tag_on_lastfm(CrimsonSong *song, const char *tag, gboolean is_added);

/* What Last.fm knows of a song and its artist, as the answers of track.getInfo, artist.getInfo, track.getSimilar
 * and artist.getSimilar in that order, any of them NULL when it failed */
#define LASTFM_DETAIL_COUNT 4
typedef void (*LastfmDetailsDone)(const char *video_id, char **answers, gpointer data);
void fetch_lastfm_details(CrimsonSong *song, LastfmDetailsDone done, gpointer data);

/* Similar tracks of the track, or similar artists without one, as read_lastfm_similar gives them, empty when Last.fm
 * knows none or is not built in */
typedef void (*LastfmSimilarDone)(char **found, gpointer data);
/* What Last.fm tells of an artist, or of their album when one is named, as read_lastfm_about gives it, or NULL */
typedef void (*LastfmAboutDone)(GHashTable *about, gpointer data);
void fetch_lastfm_about(const char *artist, const char *album, LastfmAboutDone done, gpointer data);
void fetch_lastfm_similar(const char *artist, const char *track, LastfmSimilarDone done, gpointer data);

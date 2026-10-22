#pragma once

#include "library.h"

/* Local songs go by their file address, which no YouTube id ever starts with */
#define LOCAL_SONG_PREFIX "file://"
#define LOCAL_PLAYLIST "local-library"

/* Called on the main thread whenever the local songs changed */
typedef void (*LocalLibraryChanged)(gpointer data);

/* Fills the playlist from the saved index at once, then checks the media folders in the background and watches them
 * for changes from then on */
void start_local_library(CrimsonPlaylist *playlist, LocalLibraryChanged changed, gpointer data);
/* Checks the folders again, after Settings changed which ones there are */
void rescan_local_library(void);
/* Shows what was found online for local songs, such as an album cover */
void update_local_song_info(const char *id);
gboolean is_local_song(const char *id);
/* The files of a local song found in more than one format or copy, as their addresses, empty when there is one */
GPtrArray *list_local_versions(const char *id);
/* A version as its format and size, such as FLAC · 31.2 MB */
char *describe_local_version(const char *id);
/* Shows this version of its song in the library from now on, while playlists keep the file they were given */
void choose_local_version(const char *id);
/* The file a local song plays from */
char *get_local_song_path(const char *id);

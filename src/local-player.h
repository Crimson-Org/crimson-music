#pragma once

#include "engine.h"

/* Plays songs from this computer, reporting the way the YouTube page does so the player bar treats both alike */
typedef struct LocalPlayer LocalPlayer;
typedef void (*LocalPlayerReport)(const PlayerReport *report, gpointer data);

LocalPlayer *create_local_player(LocalPlayerReport report, gpointer data);
void prepare_local_player(void);
/* Volume runs from 0 to 100 */
void play_local_song(LocalPlayer *player, const char *id, int volume);
void pause_local_song(LocalPlayer *player);
void resume_local_song(LocalPlayer *player);
void seek_local_song(LocalPlayer *player, double seconds);
void set_local_volume(LocalPlayer *player, int volume);
/* Stops and lets go of the file, when a YouTube song takes over */
void stop_local_song(LocalPlayer *player);

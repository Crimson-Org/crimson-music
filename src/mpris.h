#pragma once

#include <gtk/gtk.h>

#include "library.h"

/* The player the desktop's media keys and media controls reach, there from launch for YouTube and local songs */
typedef struct Mpris Mpris;

/* Its controls run the window's play-pause, next and previous actions */
Mpris *create_mpris(GApplication *application, GtkWindow *window);
/* The song shown in the player bar, or NULL for none */
void set_mpris_song(Mpris *mpris, CrimsonSong *song);
void set_mpris_playing(Mpris *mpris, gboolean is_playing);
void set_mpris_position(Mpris *mpris, double seconds);

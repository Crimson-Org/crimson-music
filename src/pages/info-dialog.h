#pragma once

#include <adwaita.h>

#include "sources/library.h"

/* Shows everything gathered about the song, and lets the listener give it tags of their own */
void show_info_dialog(GtkWidget *parent, CrimsonSong *song);
/* Fills the open dialog in again when its song was gathered or its tags changed */
void update_info_dialog(const char *video_id);

#pragma once

#include <adwaita.h>

typedef struct Window Window;

Window *create_window(GtkApplication *application);
GtkWindow *get_gtk_window(Window *window);
/* Shows the Last.fm connection everywhere it appears, after it or the reminder setting changed */
void update_lastfm_state(void);
/* Gives every player bar the look Settings asks for */
void apply_player_visuals(void);
void apply_playlist_suggestions(void);

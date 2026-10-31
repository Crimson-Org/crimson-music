#pragma once

#include <gtk/gtk.h>

#define SETTINGS_PAGE_PLAYBACK "playback"
#define SETTINGS_PAGE_APPEARANCE "appearance"
#define SETTINGS_PAGE_LASTFM "lastfm"
#define SETTINGS_PAGE_LOCAL "local-media"

/* Opens Settings over the window, on the named page or the first, saving and applying each change as it is made */
void show_settings(GtkWidget *parent, const char *page);
/* Shows the Last.fm connection as it is now in an open settings dialog */
void update_lastfm_settings(void);

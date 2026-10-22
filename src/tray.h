#pragma once

#include <gtk/gtk.h>

/* The panel icon, which shows the window on a click and offers the player controls on a right click */
typedef struct Tray Tray;

/* Its menu runs the window's play-pause, next and previous actions and closes the window to quit */
Tray *create_tray(GApplication *application, GtkWindow *window);
/* Switches the first menu entry between Play and Pause */
void set_tray_playing(Tray *tray, gboolean is_playing);
/* Whether a panel took the icon, since without one a hidden window could not be brought back */
gboolean is_tray_shown(Tray *tray);

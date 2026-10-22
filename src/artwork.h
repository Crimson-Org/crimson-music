#pragma once

#include <gtk/gtk.h>

#define CRIMSON_TYPE_COVER crimson_cover_get_type()
G_DECLARE_FINAL_TYPE(CrimsonCover, crimson_cover, CRIMSON, COVER, GtkWidget)

GtkWidget *create_cover(int size);
/* Surrounds the cover with a soft glow in its own colour */
void add_cover_glow(GtkWidget *cover);
/* Asks for a larger picture than the usual small one, for a cover shown big */
void set_cover_resolution(GtkWidget *cover, int pixels);
void spin_cover_in(GtkWidget *cover);
/* The picture on show, or NULL while it loads, which notify::texture announces */
GdkTexture *get_cover_texture(GtkWidget *cover);

/* Shows the cover at the address, from memory, disk or the web, or from a file when the address is a path, fading
 * in when it had to wait.
 * The widget keeps only the cover it asked for last, so a recycled list row never shows an old one */
/* An artwork address naming a drawn emblem instead of a picture, a white symbol on a colour gradient */
#define LOCAL_LIBRARY_ARTWORK "emblem:local-library"
#define HOME_ARTWORK "emblem:home"
#define HISTORY_ARTWORK "emblem:history"

void show_artwork(GtkWidget *cover, const char *address);

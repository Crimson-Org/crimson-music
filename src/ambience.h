#pragma once

#include <gtk/gtk.h>

#define CRIMSON_TYPE_AMBIENCE crimson_ambience_get_type()
G_DECLARE_FINAL_TYPE(CrimsonAmbience, crimson_ambience, CRIMSON, AMBIENCE, GtkWidget)

/* Wraps the child in slowly drifting colours read from a cover under a veil and film grain, which also lend the
 * child its accent colour */
GtkWidget *create_ambience(GtkWidget *child);
/* Crossfades to the new cover, or keeps the last one while the next is still loading when given NULL */
void set_ambience_texture(GtkWidget *ambience, GdkTexture *texture);
/* Lets the colours drift while the window has focus, or holds them still where they are, as they start */
void set_ambience_moving(GtkWidget *ambience, gboolean is_moving);
/* Fades both sides into the style's background, so the colours sit inside the bar rather than run off it */
void fade_ambience_edges(GtkWidget *ambience);
/* Lays the picture across the left of the backdrop, fading in once loaded, or takes it away for NULL */
void set_ambience_picture(GtkWidget *ambience, GdkTexture *texture);
/* The cover's most colourful average, bright enough to carry white icons, as accents and glows use it */
GdkRGBA read_cover_accent(GdkTexture *texture);
/* Turns the cover's colours off, leaving a plain background and the usual accent, or back on */
void set_ambience_plain(GtkWidget *ambience, gboolean is_plain);

#pragma once

#include <glib.h>

/* Called with the collage's file, or NULL when no cover could be loaded */
typedef void (*CollageReady)(const char *path, gpointer data);

/* Fans up to three covers out like a small stack of records over a blurred wash of the first, saved once as a
 * file named after the covers, so the same covers never need drawing again */
void make_collage(GPtrArray *addresses, CollageReady ready, gpointer data);

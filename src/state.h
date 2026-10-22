#pragma once

#include "common/window-geometry.h"

#define DEFAULT_VOLUME 80

/* What the listener left behind, kept between launches */
typedef struct {
	WindowGeometry geometry;
	char *playlist_id;
	int volume;
	gboolean is_shuffled;
	/* Playlist ids in the order the listener dragged them into, or NULL for YouTube's order */
	char **playlist_order;
	/* The sidebar's width in pixels as the listener dragged it */
	int sidebar_width;
	/* The listener's YouTube account picture, so Home shows it from the first moment, or NULL */
	char *account_photo;
} PlayerState;

/* The sidebar can be dragged between these widths */
#define SIDEBAR_MINIMUM_WIDTH 200
#define SIDEBAR_MAXIMUM_WIDTH 520

void load_player_state(PlayerState *state);
void save_player_state(const PlayerState *state);

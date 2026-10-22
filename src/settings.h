#pragma once

#include <glib.h>

/* The colour styles the listener can choose, in the order Settings lists them */
typedef enum {
	STYLE_SYSTEM,
	STYLE_LIGHT,
	STYLE_DARK,
	STYLE_COUNT,
} Style;

/* Plain colours or the drifting colours of the song's cover */
typedef enum {
	VISUALS_PLAIN,
	VISUALS_COLORS,
	VISUALS_COUNT,
} PlayerVisuals;

/* Choices made in Settings, kept between launches */
typedef struct {
	Style style;
	/* Long songs and videos carry on where they were last stopped instead of starting over */
	gboolean resumes_songs;
	/* The song the listener had up and their queue come back, paused, on the next launch */
	gboolean restores_last_song;
	/* A music video opens over the middle of the window by itself when it starts */
	gboolean autoplays_videos;
	/* Songs YouTube Music suggests show under the listener's own playlists */
	gboolean shows_suggestions;
	/* The listener's own tags are added to the song on Last.fm too, where everyone can see them */
	gboolean shares_tags;
	/* A button in the top bar reminds the listener that Last.fm is not connected */
	gboolean shows_lastfm_reminder;
	/* How the player bar looks behind its controls */
	PlayerVisuals visuals;
	/* The folders the Local Media Library shows the music of, the music folder until the listener changes them */
	char **media_folders;
} Settings;

Settings *get_settings(void);
void save_settings(void);
void apply_style(void);

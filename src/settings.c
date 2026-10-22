#include <adwaita.h>

#include "common/key-store.h"
#include "settings.h"

#define SETTINGS_FILE "settings.ini"
#define SETTINGS_GROUP "settings"

/* How each style is written in the settings file and the colour scheme it asks libadwaita for */
static const struct {
	const char *name;
	AdwColorScheme scheme;
} STYLES[STYLE_COUNT] = {
	{"system", ADW_COLOR_SCHEME_DEFAULT},
	{"light", ADW_COLOR_SCHEME_FORCE_LIGHT},
	{"dark", ADW_COLOR_SCHEME_FORCE_DARK},
};

/* How each look of the player bar is written in the settings file */
static const char *const VISUALS_NAMES[VISUALS_COUNT] = {"plain", "colors"};

static Settings settings;
static gboolean is_loaded;


static gboolean read_boolean(GKeyFile *file, const char *key, gboolean fallback) {
	return g_key_file_has_key(file, SETTINGS_GROUP, key, NULL) ? g_key_file_get_boolean(file, SETTINGS_GROUP, key, NULL)
		: fallback;
}


/* Steam keeps game soundtracks in each of its libraries, which its own list of libraries names */
static void add_steam_music(GPtrArray *folders, const char *steam) {
	g_autofree char *list = g_build_filename(steam, "steamapps", "libraryfolders.vdf", NULL);
	g_autofree char *text = NULL;
	if (!g_file_get_contents(list, &text, NULL, NULL))
		return;
	g_autoptr(GRegex) path = g_regex_new("\"path\"\\s+\"([^\"]+)\"", 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;
	for (g_regex_match(path, text, 0, &match); g_match_info_matches(match); g_match_info_next(match, NULL)) {
		g_autofree char *library = g_match_info_fetch(match, 1);
		g_autofree char *apps = g_build_filename(library, "steamapps", NULL);
		char *music = g_build_filename(apps, "music", NULL);
		if (g_file_test(apps, G_FILE_TEST_IS_DIR) && !g_ptr_array_find_with_equal_func(folders, music, g_str_equal, NULL))
			g_ptr_array_add(folders, music);
		else
			g_free(music);
	}
}


/* The music folder, and the soundtracks of every Steam library, normal or Flatpak, even before any is bought */
static char **list_default_media_folders(void) {
	GPtrArray *folders = g_ptr_array_new();
	const char *music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
	g_ptr_array_add(folders, music ? g_strdup(music) : g_build_filename(g_get_home_dir(), "Music", NULL));
	g_autofree char *steam = g_build_filename(g_get_user_data_dir(), "Steam", NULL);
	g_autofree char *flatpak = g_build_filename(g_get_home_dir(), ".var", "app", "com.valvesoftware.Steam", ".local",
		"share", "Steam", NULL);
	add_steam_music(folders, steam);
	add_steam_music(folders, flatpak);
	g_ptr_array_add(folders, NULL);
	return (char **)g_ptr_array_free(folders, FALSE);
}


static void load_settings(void) {
	g_autofree char *path = get_config_file_path(SETTINGS_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	g_autofree char *style = g_key_file_get_string(file, SETTINGS_GROUP, "style", NULL);
	for (Style index = 0; index < STYLE_COUNT; index++) {
		if (g_strcmp0(style, STYLES[index].name) == 0)
			settings.style = index;
	}
	settings.resumes_songs = g_key_file_get_boolean(file, SETTINGS_GROUP, "resume-songs", NULL);
	settings.shares_tags = read_boolean(file, "share-tags", FALSE);
	settings.restores_last_song = read_boolean(file, "restore-last-song", TRUE);
	settings.autoplays_videos = read_boolean(file, "autoplay-music-video", FALSE);
	settings.shows_suggestions = read_boolean(file, "show-suggestions", TRUE);
	settings.shows_lastfm_reminder = read_boolean(file, "lastfm-reminder", TRUE);
	g_autofree char *visuals = g_key_file_get_string(file, SETTINGS_GROUP, "player-visuals", NULL);
	settings.visuals = read_boolean(file, "song-colors", TRUE) ? VISUALS_COLORS : VISUALS_PLAIN;
	for (PlayerVisuals index = 0; index < VISUALS_COUNT; index++) {
		if (g_strcmp0(visuals, VISUALS_NAMES[index]) == 0)
			settings.visuals = index;
	}
	settings.media_folders = g_key_file_get_string_list(file, SETTINGS_GROUP, "media-folders", NULL, NULL);
	if (!settings.media_folders)
		settings.media_folders = list_default_media_folders();
	is_loaded = TRUE;
}


Settings *get_settings(void) {
	if (!is_loaded)
		load_settings();
	return &settings;
}


void save_settings(void) {
	g_autofree char *path = get_config_file_path(SETTINGS_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_key_file_set_string(file, SETTINGS_GROUP, "style", STYLES[get_settings()->style].name);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "resume-songs", settings.resumes_songs);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "restore-last-song", settings.restores_last_song);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "autoplay-music-video", settings.autoplays_videos);
	g_key_file_set_string_list(file, SETTINGS_GROUP, "media-folders", (const char *const *)settings.media_folders,
		g_strv_length(settings.media_folders));
	g_key_file_set_boolean(file, SETTINGS_GROUP, "show-suggestions", settings.shows_suggestions);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "share-tags", settings.shares_tags);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "lastfm-reminder", settings.shows_lastfm_reminder);
	g_key_file_set_string(file, SETTINGS_GROUP, "player-visuals", VISUALS_NAMES[settings.visuals]);
	save_key_file(file, path);
}


void apply_style(void) {
	adw_style_manager_set_color_scheme(adw_style_manager_get_default(), STYLES[get_settings()->style].scheme);
}

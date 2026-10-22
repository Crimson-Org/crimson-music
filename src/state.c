#include "common/key-store.h"
#include "state.h"

#define STATE_FILE "player.ini"
#define STATE_GROUP "player"
#define DEFAULT_WIDTH 1100
#define DEFAULT_HEIGHT 760
#define DEFAULT_SIDEBAR_WIDTH 260


void load_player_state(PlayerState *state) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	load_window_geometry(file, &state->geometry, DEFAULT_WIDTH, DEFAULT_HEIGHT);
	state->playlist_id = g_key_file_get_string(file, STATE_GROUP, "playlist", NULL);
	state->volume = read_key_integer(file, STATE_GROUP, "volume", DEFAULT_VOLUME, 0, 100);
	state->is_shuffled = g_key_file_get_boolean(file, STATE_GROUP, "shuffle", NULL);
	state->playlist_order = g_key_file_get_string_list(file, STATE_GROUP, "playlist-order", NULL, NULL);
	state->sidebar_width = read_key_integer(file, STATE_GROUP, "sidebar-width", DEFAULT_SIDEBAR_WIDTH,
		SIDEBAR_MINIMUM_WIDTH, SIDEBAR_MAXIMUM_WIDTH);
	state->account_photo = g_key_file_get_string(file, STATE_GROUP, "account-photo", NULL);
}


void save_player_state(const PlayerState *state) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	store_window_geometry(file, &state->geometry);
	if (state->playlist_id)
		g_key_file_set_string(file, STATE_GROUP, "playlist", state->playlist_id);
	g_key_file_set_integer(file, STATE_GROUP, "volume", state->volume);
	g_key_file_set_boolean(file, STATE_GROUP, "shuffle", state->is_shuffled);
	g_key_file_set_integer(file, STATE_GROUP, "sidebar-width", state->sidebar_width);
	if (state->account_photo)
		g_key_file_set_string(file, STATE_GROUP, "account-photo", state->account_photo);
	if (state->playlist_order)
		g_key_file_set_string_list(file, STATE_GROUP, "playlist-order", (const char *const *)state->playlist_order,
			g_strv_length(state->playlist_order));
	save_key_file(file, path);
}

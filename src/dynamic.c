#include "common/key-store.h"
#include "dynamic.h"
#include "song-info.h"

#define DYNAMIC_FILE "dynamic-playlists.ini"
/* A mood counts as part of the song from this likelihood in percent on */
#define MOOD_LIKELY 60

static const char *const FACET_NAMES[FACET_COUNT] = {"genre", "mood", "artist", "tag"};
static const char *const RANGE_NAMES[RANGE_COUNT] = {"tempo", "year", "length"};
/* The keys of the moods, which the editor shows in its own words */
static const char *const MOOD_KEYS[] = {
	"danceable", "energetic", "calm", "happy", "sad", "acoustic", "instrumental",
};


DynamicRules *create_dynamic_rules(void) {
	DynamicRules *rules = g_new0(DynamicRules, 1);
	rules->included = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	rules->excluded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	return rules;
}


static void copy_keys(GHashTable *from, GHashTable *to) {
	GHashTableIter iter;
	gpointer key;
	g_hash_table_iter_init(&iter, from);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_hash_table_add(to, g_strdup(key));
}


DynamicRules *copy_dynamic_rules(const DynamicRules *rules) {
	DynamicRules *copy = create_dynamic_rules();
	copy->id = g_strdup(rules->id);
	copy->name = g_strdup(rules->name);
	copy_keys(rules->included, copy->included);
	copy_keys(rules->excluded, copy->excluded);
	memcpy(copy->ranges, rules->ranges, sizeof copy->ranges);
	return copy;
}


void free_dynamic_rules(gpointer data) {
	DynamicRules *rules = data;
	g_free(rules->id);
	g_free(rules->name);
	g_hash_table_unref(rules->included);
	g_hash_table_unref(rules->excluded);
	g_free(rules);
}


/* Storage ================================================================= */


static void read_keys(GKeyFile *file, const char *group, const char *name, GHashTable *keys) {
	g_auto(GStrv) values = g_key_file_get_string_list(file, group, name, NULL, NULL);
	for (char **value = values; value && *value; value++)
		g_hash_table_add(keys, g_strdup(*value));
}


static void write_keys(GKeyFile *file, const char *group, const char *name, GHashTable *keys) {
	g_autofree gpointer *values = g_hash_table_get_keys_as_array(keys, NULL);
	g_key_file_set_string_list(file, group, name, (const char *const *)values, g_hash_table_size(keys));
}


GPtrArray *load_dynamic_playlists(void) {
	g_autofree char *path = get_config_file_path(DYNAMIC_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	GPtrArray *playlists = g_ptr_array_new_with_free_func(free_dynamic_rules);
	g_auto(GStrv) groups = g_key_file_get_groups(file, NULL);
	for (char **group = groups; group && *group; group++) {
		DynamicRules *rules = create_dynamic_rules();
		rules->id = g_strconcat(DYNAMIC_PREFIX, *group, NULL);
		rules->name = g_key_file_get_string(file, *group, "name", NULL);
		read_keys(file, *group, "include", rules->included);
		read_keys(file, *group, "exclude", rules->excluded);
		for (int kind = 0; kind < RANGE_COUNT; kind++) {
			g_autofree char *on = g_strconcat(RANGE_NAMES[kind], "-on", NULL);
			g_autofree char *minimum = g_strconcat(RANGE_NAMES[kind], "-from", NULL);
			g_autofree char *maximum = g_strconcat(RANGE_NAMES[kind], "-to", NULL);
			rules->ranges[kind].is_on = g_key_file_get_boolean(file, *group, on, NULL);
			rules->ranges[kind].minimum = g_key_file_get_double(file, *group, minimum, NULL);
			rules->ranges[kind].maximum = g_key_file_get_double(file, *group, maximum, NULL);
		}
		if (!rules->name)
			rules->name = g_strdup("");
		g_ptr_array_add(playlists, rules);
	}
	return playlists;
}


void save_dynamic_playlists(GPtrArray *playlists) {
	g_autofree char *path = get_config_file_path(DYNAMIC_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	for (guint index = 0; index < playlists->len; index++) {
		DynamicRules *rules = g_ptr_array_index(playlists, index);
		const char *group = rules->id + strlen(DYNAMIC_PREFIX);
		g_key_file_set_string(file, group, "name", rules->name);
		write_keys(file, group, "include", rules->included);
		write_keys(file, group, "exclude", rules->excluded);
		for (int kind = 0; kind < RANGE_COUNT; kind++) {
			g_autofree char *on = g_strconcat(RANGE_NAMES[kind], "-on", NULL);
			g_autofree char *minimum = g_strconcat(RANGE_NAMES[kind], "-from", NULL);
			g_autofree char *maximum = g_strconcat(RANGE_NAMES[kind], "-to", NULL);
			g_key_file_set_boolean(file, group, on, rules->ranges[kind].is_on);
			g_key_file_set_double(file, group, minimum, rules->ranges[kind].minimum);
			g_key_file_set_double(file, group, maximum, rules->ranges[kind].maximum);
		}
	}
	save_key_file(file, path);
}


/* Songs =================================================================== */


static void add_list(GPtrArray *values, const char *text, const char *separator) {
	g_auto(GStrv) parts = g_strsplit(text ? text : "", separator, -1);
	for (char **part = parts; *part; part++) {
		g_autofree char *value = g_utf8_strdown(g_strstrip(*part), -1);
		if (*value && !g_ptr_array_find_with_equal_func(values, value, g_str_equal, NULL))
			g_ptr_array_add(values, g_steal_pointer(&value));
	}
}


GPtrArray *get_song_facet_values(CrimsonSong *song, Facet facet) {
	GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
	switch (facet) {
	case FACET_GENRE:
		add_list(values, get_song_info_value(song->id, INFO_GENRES), ",");
		add_list(values, get_song_info_value(song->id, INFO_TAGS), ",");
		add_list(values, get_song_info_value(song->id, INFO_STYLES), ",");
		add_list(values, get_song_info_value(song->id, INFO_LASTFM), ",");
		break;
	case FACET_MOOD:
		for (guint index = 0; index < G_N_ELEMENTS(MOOD_KEYS); index++) {
			if (get_song_info_number(song->id, INFO_FIRST_MOOD + index) >= MOOD_LIKELY)
				g_ptr_array_add(values, g_strdup(MOOD_KEYS[index]));
		}
		break;
	case FACET_ARTIST:
		add_list(values, song->artist, ",");
		break;
	case FACET_TAG: {
		g_auto(GStrv) tags = get_custom_tags(song->id);
		for (char **tag = tags; tag && *tag; tag++)
			add_list(values, *tag, "\n");
		break;
	}
	default:
		break;
	}
	return values;
}


char *make_facet_key(Facet facet, const char *value) {
	return g_strconcat(FACET_NAMES[facet], ":", value, NULL);
}


double get_song_range_value(CrimsonSong *song, RangeKind kind) {
	switch (kind) {
	case RANGE_TEMPO: {
		double bpm = get_song_info_number(song->id, INFO_BPM);
		return bpm > 0 ? bpm : -1;
	}
	case RANGE_YEAR: {
		const char *date = get_song_info_value(song->id, INFO_RELEASED);
		if (!*date)
			date = get_song_info_value(song->id, INFO_UPLOADED);
		return *date ? g_ascii_strtod(date, NULL) : -1;
	}
	case RANGE_LENGTH: {
		double seconds = get_song_info_number(song->id, INFO_SECONDS);
		if (seconds <= 0)
			seconds = read_song_length(song);
		return seconds > 0 ? seconds / 60 : -1;
	}
	default:
		return -1;
	}
}


gboolean is_song_matching(CrimsonSong *song, const DynamicRules *rules) {
	for (int kind = 0; kind < RANGE_COUNT; kind++) {
		const Range *range = &rules->ranges[kind];
		if (!range->is_on)
			continue;
		double value = get_song_range_value(song, kind);
		if (value < 0 || value < range->minimum || value > range->maximum)
			return FALSE;
	}
	for (int facet = 0; facet < FACET_COUNT; facet++) {
		g_autoptr(GPtrArray) values = get_song_facet_values(song, facet);
		gboolean has_includes = FALSE, is_included = FALSE;
		GHashTableIter iter;
		gpointer key;
		g_autofree char *prefix = make_facet_key(facet, "");
		g_hash_table_iter_init(&iter, rules->included);
		while (g_hash_table_iter_next(&iter, &key, NULL))
			has_includes = has_includes || g_str_has_prefix(key, prefix);
		for (guint index = 0; index < values->len; index++) {
			g_autofree char *facet_key = make_facet_key(facet, g_ptr_array_index(values, index));
			if (g_hash_table_contains(rules->excluded, facet_key))
				return FALSE;
			is_included = is_included || g_hash_table_contains(rules->included, facet_key);
		}
		if (has_includes && !is_included)
			return FALSE;
	}
	return TRUE;
}


GPtrArray *filter_dynamic_songs(GListModel *pool, const DynamicRules *rules) {
	GPtrArray *songs = g_ptr_array_new_with_free_func(g_object_unref);
	guint count = pool ? g_list_model_get_n_items(pool) : 0;
	for (guint index = 0; index < count; index++) {
		CrimsonSong *song = g_list_model_get_item(pool, index);
		if (is_song_matching(song, rules))
			g_ptr_array_add(songs, song);
		else
			g_object_unref(song);
	}
	return songs;
}

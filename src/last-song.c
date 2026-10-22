#include "common/key-store.h"
#include "last-song.h"

#define LAST_SONG_FILE "last-song.ini"
#define SONG_GROUP "song"
#define UPCOMING_GROUP "upcoming-%u"

/* The fields of a song by name, as the file keeps them */
static const struct {
	const char *name;
	gsize offset;
} FIELDS[] = {
	{"id", G_STRUCT_OFFSET(CrimsonSong, id)},
	{"title", G_STRUCT_OFFSET(CrimsonSong, title)},
	{"artist", G_STRUCT_OFFSET(CrimsonSong, artist)},
	{"artist-id", G_STRUCT_OFFSET(CrimsonSong, artist_id)},
	{"album", G_STRUCT_OFFSET(CrimsonSong, album)},
	{"album-id", G_STRUCT_OFFSET(CrimsonSong, album_id)},
	{"duration", G_STRUCT_OFFSET(CrimsonSong, duration)},
	{"artwork", G_STRUCT_OFFSET(CrimsonSong, artwork)},
	{"set-id", G_STRUCT_OFFSET(CrimsonSong, set_id)},
};


static char **get_field(CrimsonSong *song, guint index) {
	return G_STRUCT_MEMBER_P(song, FIELDS[index].offset);
}


static void write_song(GKeyFile *file, const char *group, CrimsonSong *song) {
	for (guint index = 0; index < G_N_ELEMENTS(FIELDS); index++) {
		const char *value = *get_field(song, index);
		g_key_file_set_string(file, group, FIELDS[index].name, value ? value : "");
	}
}


static CrimsonSong *read_song(GKeyFile *file, const char *group) {
	g_autofree char *id = g_key_file_get_string(file, group, "id", NULL);
	if (!id || !*id)
		return NULL;
	CrimsonSong *song = g_object_new(CRIMSON_TYPE_SONG, NULL);
	for (guint index = 0; index < G_N_ELEMENTS(FIELDS); index++) {
		char *value = g_key_file_get_string(file, group, FIELDS[index].name, NULL);
		*get_field(song, index) = value ? value : g_strdup("");
	}
	return song;
}


void save_last_song(CrimsonSong *song, GPtrArray *upcoming, guint queued, const char *page_tag,
	const char *playlist_id) {
	g_autofree char *path = get_state_file_path(LAST_SONG_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	if (song) {
		write_song(file, SONG_GROUP, song);
		g_key_file_set_string(file, SONG_GROUP, "page", page_tag ? page_tag : "");
		g_key_file_set_string(file, SONG_GROUP, "playlist", playlist_id ? playlist_id : "");
		g_key_file_set_integer(file, SONG_GROUP, "queued", (int)queued);
		for (guint index = 0; index < upcoming->len; index++) {
			g_autofree char *group = g_strdup_printf(UPCOMING_GROUP, index);
			write_song(file, group, g_ptr_array_index(upcoming, index));
		}
	}
	save_key_file(file, path);
}


LastSong *load_last_song(void) {
	g_autofree char *path = get_state_file_path(LAST_SONG_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	CrimsonSong *song = read_song(file, SONG_GROUP);
	if (!song)
		return NULL;
	LastSong *last = g_new0(LastSong, 1);
	last->song = song;
	last->upcoming = g_ptr_array_new_with_free_func(g_object_unref);
	last->page_tag = g_key_file_get_string(file, SONG_GROUP, "page", NULL);
	last->playlist_id = g_key_file_get_string(file, SONG_GROUP, "playlist", NULL);
	for (guint index = 0;; index++) {
		g_autofree char *group = g_strdup_printf(UPCOMING_GROUP, index);
		CrimsonSong *upcoming = read_song(file, group);
		if (!upcoming)
			break;
		g_ptr_array_add(last->upcoming, upcoming);
	}
	last->queued = MIN((guint)MAX(0, g_key_file_get_integer(file, SONG_GROUP, "queued", NULL)), last->upcoming->len);
	return last;
}


void free_last_song(LastSong *last) {
	g_clear_object(&last->song);
	g_ptr_array_unref(last->upcoming);
	g_free(last->page_tag);
	g_free(last->playlist_id);
	g_free(last);
}

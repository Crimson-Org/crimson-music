#include <stddef.h>
#include <string.h>

#include <glib/gstdio.h>

#include "common/identity.h"
#include "library.h"

#define LOCAL_PLAYLISTS_FILE "local-playlists"
#define CACHE_FOLDER "library"
#define PLAYLISTS_FILE "playlists-2.cache"
#define SONGS_FILE_FORMAT "songs-2-%s.cache"
#define SAFE_NAME_CHARACTERS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
#define MAXIMUM_FIELDS 10
#define DISLIKED_STATUS "DISLIKE"

/* A text field of an object, by the name the page uses for it and where it sits in the struct */
typedef struct {
	const char *name;
	size_t offset;
} Field;

static const Field PLAYLIST_FIELDS[] = {
	{"id", offsetof(CrimsonPlaylist, id)},
	{"title", offsetof(CrimsonPlaylist, title)},
	{"subtitle", offsetof(CrimsonPlaylist, subtitle)},
	{"artwork", offsetof(CrimsonPlaylist, artwork)},
};

static const Field SONG_FIELDS[] = {
	{"id", offsetof(CrimsonSong, id)},
	{"title", offsetof(CrimsonSong, title)},
	{"artist", offsetof(CrimsonSong, artist)},
	{"artistId", offsetof(CrimsonSong, artist_id)},
	{"album", offsetof(CrimsonSong, album)},
	{"albumId", offsetof(CrimsonSong, album_id)},
	{"duration", offsetof(CrimsonSong, duration)},
	{"artwork", offsetof(CrimsonSong, artwork)},
	{"setId", offsetof(CrimsonSong, set_id)},
	{"likeStatus", offsetof(CrimsonSong, like_status)},
};

static const Field CARD_FIELDS[] = {
	{"id", offsetof(Card, id)},
	{"title", offsetof(Card, title)},
	{"subtitle", offsetof(Card, subtitle)},
	{"artwork", offsetof(Card, artwork)},
};

/* How a kind of object is cached and compared, with a GVariant tuple of its text fields */
typedef struct {
	const Field *fields;
	guint count;
	const char *tuple;
} Shape;

static const Shape PLAYLIST_SHAPE = {PLAYLIST_FIELDS, G_N_ELEMENTS(PLAYLIST_FIELDS), "(ssss)"};
static const Shape SONG_SHAPE = {SONG_FIELDS, G_N_ELEMENTS(SONG_FIELDS), "(ssssssssss)"};

static GHashTable *disliked_videos;

G_DEFINE_FINAL_TYPE(CrimsonSong, crimson_song, G_TYPE_OBJECT)
G_DEFINE_FINAL_TYPE(CrimsonPlaylist, crimson_playlist, G_TYPE_OBJECT)


static char **get_field(gpointer object, const Field *field) {
	return (char **)((char *)object + field->offset);
}


static void free_fields(gpointer object, const Field *fields, guint count) {
	for (guint index = 0; index < count; index++)
		g_free(*get_field(object, &fields[index]));
}


static void finalize_song(GObject *object) {
	free_fields(object, SONG_FIELDS, G_N_ELEMENTS(SONG_FIELDS));
	G_OBJECT_CLASS(crimson_song_parent_class)->finalize(object);
}


static void crimson_song_class_init(CrimsonSongClass *type) {
	G_OBJECT_CLASS(type)->finalize = finalize_song;
}


static void crimson_song_init(CrimsonSong *song) {
}


static void finalize_playlist(GObject *object) {
	CrimsonPlaylist *playlist = CRIMSON_PLAYLIST(object);
	free_fields(object, PLAYLIST_FIELDS, G_N_ELEMENTS(PLAYLIST_FIELDS));
	g_object_unref(playlist->songs);
	g_clear_pointer(&playlist->incoming, g_ptr_array_unref);
	g_ptr_array_unref(playlist->pending_added);
	g_hash_table_unref(playlist->pending_removed);
	G_OBJECT_CLASS(crimson_playlist_parent_class)->finalize(object);
}


static void crimson_playlist_class_init(CrimsonPlaylistClass *type) {
	G_OBJECT_CLASS(type)->finalize = finalize_playlist;
}


static void crimson_playlist_init(CrimsonPlaylist *playlist) {
	playlist->songs = g_list_store_new(CRIMSON_TYPE_SONG);
	playlist->pending_added = g_ptr_array_new_with_free_func(g_object_unref);
	playlist->pending_removed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}


Card *copy_card(const Card *card) {
	Card *copy = g_new0(Card, 1);
	for (guint index = 0; index < G_N_ELEMENTS(CARD_FIELDS); index++)
		*get_field(copy, &CARD_FIELDS[index]) = g_strdup(*get_field((gpointer)card, &CARD_FIELDS[index]));
	copy->video_id = g_strdup(card->video_id);
	return copy;
}


/* Cards travel in drags as boxed values, so drop targets can ask for them by type */
GType crimson_card_get_type(void) {
	static gsize type;
	if (g_once_init_enter(&type)) {
		GType registered = g_boxed_type_register_static("CrimsonCard", (GBoxedCopyFunc)copy_card,
			(GBoxedFreeFunc)free_card);
		g_once_init_leave(&type, registered);
	}
	return type;
}


void free_card(gpointer card) {
	free_fields(card, CARD_FIELDS, G_N_ELEMENTS(CARD_FIELDS));
	g_free(((Card *)card)->video_id);
	g_free(card);
}


static char *read_text(JSCValue *object, const char *name) {
	g_autoptr(JSCValue) value = jsc_value_object_get_property(object, name);
	return jsc_value_is_string(value) ? jsc_value_to_string(value) : g_strdup("");
}


static void read_fields(gpointer object, JSCValue *item, const Field *fields, guint count) {
	for (guint index = 0; index < count; index++)
		*get_field(object, &fields[index]) = read_text(item, fields[index].name);
}


static guint get_array_length(JSCValue *array) {
	g_autoptr(JSCValue) length = jsc_value_object_get_property(array, "length");
	return jsc_value_is_number(length) ? (guint)jsc_value_to_int32(length) : 0;
}


/* Makes each item of the page's array into an object through the create function */
static GPtrArray *read_array(JSCValue *array, gpointer (*create)(JSCValue *item), GDestroyNotify free_item) {
	GPtrArray *items = g_ptr_array_new_with_free_func(free_item);
	for (guint index = 0; index < get_array_length(array); index++) {
		g_autoptr(JSCValue) item = jsc_value_object_get_property_at_index(array, index);
		g_ptr_array_add(items, create(item));
	}
	return items;
}


static gpointer create_read_playlist(JSCValue *item) {
	CrimsonPlaylist *playlist = g_object_new(CRIMSON_TYPE_PLAYLIST, NULL);
	read_fields(playlist, item, PLAYLIST_FIELDS, G_N_ELEMENTS(PLAYLIST_FIELDS));
	return playlist;
}


static gpointer create_read_song(JSCValue *item) {
	CrimsonSong *song = g_object_new(CRIMSON_TYPE_SONG, NULL);
	read_fields(song, item, SONG_FIELDS, G_N_ELEMENTS(SONG_FIELDS));
	return song;
}


static gpointer create_read_card(JSCValue *item) {
	Card *card = g_new0(Card, 1);
	read_fields(card, item, CARD_FIELDS, G_N_ELEMENTS(CARD_FIELDS));
	return card;
}


gboolean is_video_disliked(const char *video_id) {
	return disliked_videos && video_id && g_hash_table_contains(disliked_videos, video_id);
}


void set_video_disliked(const char *video_id, gboolean is_disliked) {
	if (!disliked_videos)
		disliked_videos = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	if (is_disliked)
		g_hash_table_add(disliked_videos, g_strdup(video_id));
	else
		g_hash_table_remove(disliked_videos, video_id);
}


/* Lists without a like status, such as some Home shelves, leave what is known about the song alone */
static void remember_ratings(GPtrArray *songs) {
	for (guint index = 0; index < songs->len; index++) {
		CrimsonSong *song = g_ptr_array_index(songs, index);
		if (*song->like_status)
			set_video_disliked(song->id, g_str_equal(song->like_status, DISLIKED_STATUS));
	}
}


GPtrArray *read_playlists(JSCValue *array) {
	return read_array(array, create_read_playlist, g_object_unref);
}


GPtrArray *read_songs(JSCValue *array) {
	GPtrArray *songs = read_array(array, create_read_song, g_object_unref);
	remember_ratings(songs);
	return songs;
}


GPtrArray *read_cards(JSCValue *array) {
	return read_array(array, create_read_card, free_card);
}


static GVariant *describe_object(gpointer object, const Shape *shape) {
	GVariant *texts[MAXIMUM_FIELDS];
	for (guint index = 0; index < shape->count; index++) {
		const char *text = *get_field(object, &shape->fields[index]);
		texts[index] = g_variant_new_string(text ? text : "");
	}
	return g_variant_new_tuple(texts, shape->count);
}


static GVariant *describe_objects(GPtrArray *objects, const Shape *shape) {
	g_autofree char *format = g_strconcat("a", shape->tuple, NULL);
	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE(format));
	for (guint index = 0; index < objects->len; index++)
		g_variant_builder_add_value(&builder, describe_object(g_ptr_array_index(objects, index), shape));
	return g_variant_ref_sink(g_variant_builder_end(&builder));
}


static gpointer create_described(GType type, GVariant *tuple, const Shape *shape) {
	gpointer object = g_object_new(type, NULL);
	for (guint index = 0; index < shape->count; index++)
		g_variant_get_child(tuple, index, "s", get_field(object, &shape->fields[index]));
	return object;
}


static gboolean objects_match(gpointer kept, gpointer fresh, const Shape *shape) {
	for (guint index = 0; index < shape->count; index++) {
		if (g_strcmp0(*get_field(kept, &shape->fields[index]), *get_field(fresh, &shape->fields[index])) != 0)
			return FALSE;
	}
	return TRUE;
}


static gboolean playlists_match(gpointer kept, gpointer fresh) {
	return objects_match(kept, fresh, &PLAYLIST_SHAPE);
}


static gboolean songs_match(gpointer kept, gpointer fresh) {
	return objects_match(kept, fresh, &SONG_SHAPE);
}


static GPtrArray *list_store_items(GListStore *store) {
	GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(store)); index++)
		g_ptr_array_add(items, g_list_model_get_item(G_LIST_MODEL(store), index));
	return items;
}


static void fill_store(GListStore *store, GPtrArray *items) {
	g_list_store_splice(store, 0, g_list_model_get_n_items(G_LIST_MODEL(store)), items->pdata, items->len);
}


typedef gboolean (*ItemsMatch)(gpointer kept, gpointer fresh);


/* Finds the run of items that changed, so only those rows are rebuilt and nothing else on screen moves or fades
 * again when a refresh brings a small change */
static void find_changes(GPtrArray *current, GPtrArray *items, ItemsMatch match, guint *first, guint *last) {
	guint start = 0;
	while (start < current->len && start < items->len
		&& match(g_ptr_array_index(current, start), g_ptr_array_index(items, start)))
		start++;
	guint end = 0;
	while (end < current->len - start && end < items->len - start
		&& match(g_ptr_array_index(current, current->len - 1 - end), g_ptr_array_index(items, items->len - 1 - end)))
		end++;
	*first = start;
	*last = end;
}


static void splice_changes(GListStore *store, GPtrArray *current, GPtrArray *items, guint start, guint end) {
	g_list_store_splice(store, start, current->len - start - end, items->pdata + start, items->len - start - end);
}


/* Playlists already known keep their object, so their songs and any refresh on its way survive */
static void keep_playlist_objects(GPtrArray *current, GPtrArray *playlists) {
	for (guint index = 0; index < playlists->len; index++) {
		CrimsonPlaylist *fresh = g_ptr_array_index(playlists, index);
		for (guint old = 0; old < current->len; old++) {
			CrimsonPlaylist *kept = g_ptr_array_index(current, old);
			if (strcmp(kept->id, fresh->id) != 0)
				continue;
			g_set_str(&kept->title, fresh->title);
			g_set_str(&kept->subtitle, fresh->subtitle);
			g_set_str(&kept->artwork, fresh->artwork);
			g_ptr_array_index(playlists, index) = g_object_ref(kept);
			g_object_unref(fresh);
			break;
		}
	}
}


gboolean replace_playlists(GListStore *store, GPtrArray *playlists) {
	g_autoptr(GPtrArray) current = list_store_items(store);
	guint start, end;
	find_changes(current, playlists, playlists_match, &start, &end);
	if (start == current->len && start == playlists->len)
		return FALSE;
	keep_playlist_objects(current, playlists);
	splice_changes(store, current, playlists, start, end);
	return TRUE;
}


gboolean replace_songs(CrimsonPlaylist *playlist, GPtrArray *songs) {
	gboolean had_songs = playlist->has_songs;
	playlist->has_songs = TRUE;
	g_autoptr(GPtrArray) current = list_store_items(playlist->songs);
	guint start, end;
	find_changes(current, songs, songs_match, &start, &end);
	if (start == current->len && start == songs->len)
		return !had_songs;
	splice_changes(playlist->songs, current, songs, start, end);
	return TRUE;
}


void note_playlist_addition(CrimsonPlaylist *playlist, CrimsonSong *song) {
	g_ptr_array_add(playlist->pending_added, g_object_ref(song));
}


/* A song removed before YouTube gave it an entry id is simply no longer waiting to be added */
void note_playlist_removal(CrimsonPlaylist *playlist, CrimsonSong *song) {
	if (g_ptr_array_remove(playlist->pending_added, song))
		return;
	if (song->set_id && *song->set_id)
		g_hash_table_add(playlist->pending_removed, g_strdup(song->set_id));
}


void forget_playlist_edits(CrimsonPlaylist *playlist) {
	g_ptr_array_set_size(playlist->pending_added, 0);
	g_hash_table_remove_all(playlist->pending_removed);
}


/* An added song is known by its entry id once YouTube answered, and by its video before that */
static gboolean holds_added_song(GPtrArray *songs, CrimsonSong *added) {
	for (guint index = 0; index < songs->len; index++) {
		CrimsonSong *song = g_ptr_array_index(songs, index);
		gboolean has_entry = added->set_id && *added->set_id;
		if (has_entry ? g_strcmp0(song->set_id, added->set_id) == 0 : g_strcmp0(song->id, added->id) == 0)
			return TRUE;
	}
	return FALSE;
}


void apply_playlist_edits(CrimsonPlaylist *playlist, GPtrArray *songs, gboolean is_fresh) {
	g_autoptr(GHashTable) still_listed = g_hash_table_new(g_str_hash, g_str_equal);
	for (guint index = songs->len; index > 0; index--) {
		CrimsonSong *song = g_ptr_array_index(songs, index - 1);
		if (!song->set_id || !g_hash_table_contains(playlist->pending_removed, song->set_id))
			continue;
		g_hash_table_add(still_listed, song->set_id);
		g_ptr_array_remove_index(songs, index - 1);
	}
	if (is_fresh) {
		GHashTableIter iter;
		gpointer set_id;
		g_hash_table_iter_init(&iter, playlist->pending_removed);
		while (g_hash_table_iter_next(&iter, &set_id, NULL)) {
			if (!g_hash_table_contains(still_listed, set_id))
				g_hash_table_iter_remove(&iter);
		}
	}
	for (guint index = playlist->pending_added->len; index > 0; index--) {
		CrimsonSong *added = g_ptr_array_index(playlist->pending_added, index - 1);
		if (!holds_added_song(songs, added))
			g_ptr_array_add(songs, g_object_ref(added));
		else if (is_fresh)
			g_ptr_array_remove_index(playlist->pending_added, index - 1);
	}
}


/* Puts the playlists in the listener's own order, with ones the order has never seen first, as YouTube puts
 * the newest first */
void order_playlists(GPtrArray *playlists, const char *const *order) {
	g_autoptr(GPtrArray) ordered = g_ptr_array_new();
	for (guint item = 0; item < playlists->len; item++) {
		CrimsonPlaylist *playlist = g_ptr_array_index(playlists, item);
		if (!order || !g_strv_contains(order, playlist->id))
			g_ptr_array_add(ordered, playlist);
	}
	for (guint index = 0; order && order[index]; index++) {
		for (guint item = 0; item < playlists->len; item++) {
			CrimsonPlaylist *playlist = g_ptr_array_index(playlists, item);
			if (strcmp(playlist->id, order[index]) == 0)
				g_ptr_array_add(ordered, playlist);
		}
	}
	memcpy(playlists->pdata, ordered->pdata, sizeof(gpointer) * ordered->len);
}


static char *get_cache_path(const char *file_name) {
	return g_build_filename(g_get_user_cache_dir(), get_application_identity()->slug, CACHE_FOLDER, file_name,
		NULL);
}


static char *get_songs_cache_path(const char *playlist_id) {
	g_autofree char *safe_id = g_strcanon(g_strdup(playlist_id), SAFE_NAME_CHARACTERS, '_');
	g_autofree char *file_name = g_strdup_printf(SONGS_FILE_FORMAT, safe_id);
	return get_cache_path(file_name);
}


static GVariant *read_cache(const char *path, const char *format) {
	char *contents = NULL;
	gsize length = 0;
	if (!g_file_get_contents(path, &contents, &length, NULL))
		return NULL;
	GVariant *value = g_variant_new_from_data(G_VARIANT_TYPE(format), contents, length, FALSE, g_free, contents);
	return g_variant_ref_sink(value);
}


static void write_cache(const char *path, GVariant *value) {
	g_autofree char *directory = g_path_get_dirname(path);
	g_autoptr(GError) error = NULL;
	if (g_mkdir_with_parents(directory, 0700) != 0
		|| !g_file_set_contents(path, g_variant_get_data(value), (gssize)g_variant_get_size(value), &error))
		g_warning("%s was not saved", path);
}


static GPtrArray *read_described(GVariant *array, GType type, const Shape *shape) {
	GPtrArray *objects = g_ptr_array_new_with_free_func(g_object_unref);
	for (gsize index = 0; index < g_variant_n_children(array); index++) {
		g_autoptr(GVariant) tuple = g_variant_get_child_value(array, index);
		g_ptr_array_add(objects, create_described(type, tuple, shape));
	}
	return objects;
}


void load_cached_playlists(GListStore *store, const char *const *order) {
	g_autofree char *path = get_cache_path(PLAYLISTS_FILE);
	g_autofree char *format = g_strconcat("a", PLAYLIST_SHAPE.tuple, NULL);
	g_autoptr(GVariant) cached = read_cache(path, format);
	if (!cached)
		return;
	g_autoptr(GPtrArray) playlists = read_described(cached, CRIMSON_TYPE_PLAYLIST, &PLAYLIST_SHAPE);
	order_playlists(playlists, order);
	fill_store(store, playlists);
}


/* Songs are cached with whether the playlist is the listener's own, which only its page tells */
void load_cached_songs(CrimsonPlaylist *playlist) {
	g_autofree char *path = get_songs_cache_path(playlist->id);
	g_autofree char *format = g_strconcat("(ba", SONG_SHAPE.tuple, ")", NULL);
	g_autoptr(GVariant) cached = read_cache(path, format);
	if (!cached)
		return;
	g_autoptr(GVariant) array = g_variant_get_child_value(cached, 1);
	g_variant_get_child(cached, 0, "b", &playlist->is_editable);
	g_autoptr(GPtrArray) songs = read_described(array, CRIMSON_TYPE_SONG, &SONG_SHAPE);
	remember_ratings(songs);
	apply_playlist_edits(playlist, songs, FALSE);
	fill_store(playlist->songs, songs);
	playlist->has_songs = TRUE;
}


void save_cached_playlists(GListStore *store) {
	g_autofree char *path = get_cache_path(PLAYLISTS_FILE);
	g_autoptr(GPtrArray) playlists = list_store_items(store);
	for (guint index = playlists->len; index > 0; index--) {
		if (is_local_playlist(g_ptr_array_index(playlists, index - 1)))
			g_ptr_array_remove_index(playlists, index - 1);
	}
	g_autoptr(GVariant) value = describe_objects(playlists, &PLAYLIST_SHAPE);
	write_cache(path, value);
}


void save_cached_songs(CrimsonPlaylist *playlist) {
	g_autofree char *path = get_songs_cache_path(playlist->id);
	g_autoptr(GPtrArray) songs = list_store_items(playlist->songs);
	g_autoptr(GVariant) array = describe_objects(songs, &SONG_SHAPE);
	g_autoptr(GVariant) value = g_variant_ref_sink(g_variant_new("(b@*)", playlist->is_editable, array));
	write_cache(path, value);
}


void clear_cache(void) {
	g_autofree char *folder = get_cache_path(NULL);
	g_autoptr(GDir) directory = g_dir_open(folder, 0, NULL);
	for (const char *name = directory ? g_dir_read_name(directory) : NULL; name; name = g_dir_read_name(directory)) {
		g_autofree char *path = g_build_filename(folder, name, NULL);
		g_unlink(path);
	}
}


char *format_song_length(double seconds) {
	int total = MAX(0, (int)seconds);
	if (total >= 3600)
		return g_strdup_printf("%d:%02d:%02d", total / 3600, total / 60 % 60, total % 60);
	return g_strdup_printf("%d:%02d", total / 60, total % 60);
}


double read_song_length(CrimsonSong *song) {
	g_auto(GStrv) parts = g_strsplit(song->duration ? song->duration : "", ":", -1);
	double seconds = 0;
	for (char **part = parts; *part; part++)
		seconds = seconds * 60 + g_ascii_strtod(*part, NULL);
	return seconds;
}


GStrv list_song_artists(CrimsonSong *song) {
	g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
	g_auto(GStrv) lines = g_strsplit(song->artist_id ? song->artist_id : "", "\n", -1);
	for (char **line = lines; *line; line++) {
		if (!**line)
			continue;
		char *tab = strchr(*line, '\t');
		g_strv_builder_take(builder, tab ? g_strndup(*line, tab - *line) : g_strdup(*line));
		g_strv_builder_add(builder, tab ? tab + 1 : song->artist);
	}
	return g_strv_builder_end(builder);
}


char *get_song_artist_id(CrimsonSong *song) {
	g_auto(GStrv) artists = list_song_artists(song);
	return g_strdup(artists[0] ? artists[0] : "");
}


/* Playlists made in Crimson Music ========================================== */


gboolean is_local_playlist(CrimsonPlaylist *playlist) {
	return playlist && playlist->id && g_str_has_prefix(playlist->id, LOCAL_PLAYLIST_PREFIX);
}


static char *get_local_playlists_path(void) {
	return g_build_filename(g_get_user_data_dir(), get_application_identity()->slug, LOCAL_PLAYLISTS_FILE, NULL);
}


/* They are the listener's own work rather than a copy of something online, so they live with the data, not the
 * cache, as each playlist with its songs */
GPtrArray *load_local_playlists(void) {
	GPtrArray *playlists = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree char *path = get_local_playlists_path();
	g_autofree char *format = g_strconcat("a(", PLAYLIST_SHAPE.tuple, "a", SONG_SHAPE.tuple, ")", NULL);
	g_autoptr(GVariant) saved = read_cache(path, format);
	for (gsize index = 0; saved && index < g_variant_n_children(saved); index++) {
		g_autoptr(GVariant) entry = g_variant_get_child_value(saved, index);
		g_autoptr(GVariant) heading = g_variant_get_child_value(entry, 0);
		g_autoptr(GVariant) songs = g_variant_get_child_value(entry, 1);
		CrimsonPlaylist *playlist = create_described(CRIMSON_TYPE_PLAYLIST, heading, &PLAYLIST_SHAPE);
		g_autoptr(GPtrArray) items = read_described(songs, CRIMSON_TYPE_SONG, &SONG_SHAPE);
		fill_store(playlist->songs, items);
		playlist->has_songs = TRUE;
		playlist->is_editable = TRUE;
		g_ptr_array_add(playlists, playlist);
	}
	return playlists;
}


void save_local_playlists(GListStore *store) {
	g_autofree char *format = g_strconcat("a(", PLAYLIST_SHAPE.tuple, "a", SONG_SHAPE.tuple, ")", NULL);
	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE(format));
	for (guint index = 0; index < g_list_model_get_n_items(G_LIST_MODEL(store)); index++) {
		g_autoptr(CrimsonPlaylist) playlist = g_list_model_get_item(G_LIST_MODEL(store), index);
		if (!is_local_playlist(playlist))
			continue;
		g_autoptr(GPtrArray) songs = list_store_items(playlist->songs);
		g_autoptr(GVariant) described = describe_objects(songs, &SONG_SHAPE);
		g_variant_builder_open(&builder, G_VARIANT_TYPE(format + 1));
		g_variant_builder_add_value(&builder, describe_object(playlist, &PLAYLIST_SHAPE));
		g_variant_builder_add_value(&builder, described);
		g_variant_builder_close(&builder);
	}
	g_autoptr(GVariant) value = g_variant_ref_sink(g_variant_builder_end(&builder));
	g_autofree char *path = get_local_playlists_path();
	write_cache(path, value);
}


CrimsonPlaylist *create_local_playlist(const char *title) {
	CrimsonPlaylist *playlist = g_object_new(CRIMSON_TYPE_PLAYLIST, NULL);
	g_autofree char *uuid = g_uuid_string_random();
	playlist->id = g_strconcat(LOCAL_PLAYLIST_PREFIX, uuid, NULL);
	playlist->title = g_strdup(title);
	playlist->subtitle = g_strdup("");
	playlist->artwork = g_strdup("");
	playlist->has_songs = TRUE;
	playlist->is_editable = TRUE;
	return playlist;
}

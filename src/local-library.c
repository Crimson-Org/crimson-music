#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <math.h>

#include "common/identity.h"
#include "local-library.h"
#include "settings.h"
#include "song-info.h"

#define INDEX_FILE "local-library.index"
#define INDEX_FORMAT "a(sxxsssdsu)"
#define COVERS_FOLDER "local-covers"
#define DISCOVER_SECONDS 5
#define CHOICES_FILE "local-versions.ini"
#define CHOICES_GROUP "versions"
/* Files of the same song in other formats differ a little in length from their encoding */
#define SAME_LENGTH_SECONDS 2
/* Changes in a folder usually come in bursts, such as a whole album being copied, so the check waits for quiet */
#define SETTLE_MILLISECONDS 1500
#define ENUMERATE_ATTRIBUTES G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
	G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_TIME_MODIFIED

static const char *const AUDIO_EXTENSIONS[] = {
	"mp3", "ogg", "oga", "opus", "flac", "m4a", "aac", "wav", "wma", "aif", "aiff", "ape", "wv", "mka", NULL,
};
static const char *const LOSSLESS_EXTENSIONS[] = {"flac", "wav", "aif", "aiff", "ape", "wv", NULL};
/* Pictures beside the songs that serve as the cover when a file carries none of its own */
static const char *const FOLDER_COVERS[] = {"cover.jpg", "cover.png", "folder.jpg", "folder.png", "front.jpg", NULL};

/* What is known of one file, kept so an unchanged file is never read again */
typedef struct {
	char *path;
	gint64 size, modified;
	char *title, *artist, *album;
	double seconds;
	char *cover;
	guint track;
} Entry;

typedef struct {
	char **folders;
	/* The entries from before by path, reused for files whose size and time are the same */
	GHashTable *known;
	GPtrArray *entries, *directories;
	/* Files read again because they changed, whose online lookups no longer fit them */
	GPtrArray *changed;
	GstDiscoverer *discoverer;
} Scan;

static CrimsonPlaylist *library;
static LocalLibraryChanged on_changed;
static gpointer changed_data;
static GPtrArray *index_entries;
static GHashTable *monitors;
static gboolean is_scanning, is_scan_wanted;
static guint settle_source;
/* Each version's address leads to all versions of its song, for songs found more than once */
static GHashTable *versions;
/* The version the listener picked for a song, by the song */
static GHashTable *choices;


static void free_entry(gpointer data) {
	Entry *entry = data;
	g_free(entry->path);
	g_free(entry->title);
	g_free(entry->artist);
	g_free(entry->album);
	g_free(entry->cover);
	g_free(entry);
}


static Entry *copy_entry(const Entry *entry) {
	Entry *copy = g_memdup2(entry, sizeof(Entry));
	copy->path = g_strdup(entry->path);
	copy->title = g_strdup(entry->title);
	copy->artist = g_strdup(entry->artist);
	copy->album = g_strdup(entry->album);
	copy->cover = g_strdup(entry->cover);
	return copy;
}


gboolean is_local_song(const char *id) {
	return id && g_str_has_prefix(id, LOCAL_SONG_PREFIX);
}


char *get_local_song_path(const char *id) {
	return g_filename_from_uri(id, NULL, NULL);
}


static char *get_cache_file(const char *name) {
	return g_build_filename(g_get_user_cache_dir(), get_application_identity()->slug, name, NULL);
}


/* The index ================================================================ */


static GPtrArray *load_index(void) {
	GPtrArray *entries = g_ptr_array_new_with_free_func(free_entry);
	g_autofree char *path = get_cache_file(INDEX_FILE);
	char *contents = NULL;
	gsize length = 0;
	if (!g_file_get_contents(path, &contents, &length, NULL))
		return entries;
	g_autoptr(GVariant) value = g_variant_ref_sink(g_variant_new_from_data(G_VARIANT_TYPE(INDEX_FORMAT), contents,
		length, FALSE, g_free, contents));
	GVariantIter iter;
	g_variant_iter_init(&iter, value);
	Entry entry;
	while (g_variant_iter_next(&iter, "(sxxsssdsu)", &entry.path, &entry.size, &entry.modified, &entry.title,
		&entry.artist, &entry.album, &entry.seconds, &entry.cover, &entry.track))
		g_ptr_array_add(entries, g_memdup2(&entry, sizeof(Entry)));
	return entries;
}


static void save_index(GPtrArray *entries) {
	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE(INDEX_FORMAT));
	for (guint index = 0; index < entries->len; index++) {
		Entry *entry = g_ptr_array_index(entries, index);
		g_variant_builder_add(&builder, "(sxxsssdsu)", entry->path, entry->size, entry->modified, entry->title,
			entry->artist, entry->album, entry->seconds, entry->cover, entry->track);
	}
	g_autoptr(GVariant) value = g_variant_ref_sink(g_variant_builder_end(&builder));
	g_autofree char *path = get_cache_file(INDEX_FILE);
	g_autofree char *directory = g_path_get_dirname(path);
	g_mkdir_with_parents(directory, 0700);
	g_file_set_contents(path, g_variant_get_data(value), (gssize)g_variant_get_size(value), NULL);
}


/* Songs are listed by artist, then album and track, as a music library usually is */
static int compare_entries(gconstpointer first, gconstpointer second) {
	const Entry *a = *(Entry *const *)first, *b = *(Entry *const *)second;
	int order = g_utf8_collate(a->artist, b->artist);
	if (!order)
		order = g_utf8_collate(a->album, b->album);
	if (!order)
		order = (int)a->track - (int)b->track;
	return order ? order : g_utf8_collate(a->title, b->title);
}


static CrimsonSong *create_song(const Entry *entry) {
	CrimsonSong *song = g_object_new(CRIMSON_TYPE_SONG, NULL);
	song->id = g_filename_to_uri(entry->path, NULL, NULL);
	song->title = g_strdup(entry->title);
	song->artist = g_strdup(entry->artist);
	song->artist_id = g_strdup("");
	song->album = g_strdup(entry->album);
	song->album_id = g_strdup("");
	song->duration = entry->seconds > 0 ? format_song_length(entry->seconds) : g_strdup("");
	/* A file without a picture takes the album cover found online for its artist and title, if there is one */
	const char *found = *entry->cover ? NULL : get_song_info_value(song->id, INFO_COVER);
	song->artwork = g_strdup(found && *found ? found : entry->cover);
	song->set_id = g_strdup("");
	return song;
}


/* Versions ================================================================= */


static void show_entries(void);


static gboolean is_same_song(const Entry *a, const Entry *b) {
	return g_utf8_collate(a->title, b->title) == 0 && g_utf8_collate(a->artist, b->artist) == 0
		&& g_utf8_collate(a->album, b->album) == 0 && fabs(a->seconds - b->seconds) <= SAME_LENGTH_SECONDS;
}


/* Lossless files sound best, and among the rest the largest usually has the highest quality */
static int rank_entry(const Entry *entry) {
	const char *dot = strrchr(entry->path, '.');
	g_autofree char *extension = g_ascii_strdown(dot ? dot + 1 : "", -1);
	return g_strv_contains(LOSSLESS_EXTENSIONS, extension) ? 1 : 0;
}


static gboolean is_better(const Entry *a, const Entry *b) {
	return rank_entry(a) != rank_entry(b) ? rank_entry(a) > rank_entry(b) : a->size > b->size;
}


static char *get_choices_path(void) {
	return g_build_filename(g_get_user_state_dir(), get_application_identity()->slug, CHOICES_FILE, NULL);
}


static GHashTable *load_choices(void) {
	GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autofree char *path = get_choices_path();
	g_autoptr(GKeyFile) file = g_key_file_new();
	if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
		return table;
	g_auto(GStrv) keys = g_key_file_get_keys(file, CHOICES_GROUP, NULL, NULL);
	for (char **key = keys; key && *key; key++)
		g_hash_table_insert(table, g_strdup(*key), g_key_file_get_string(file, CHOICES_GROUP, *key, NULL));
	return table;
}


static void save_choices(void) {
	g_autoptr(GKeyFile) file = g_key_file_new();
	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, choices);
	while (g_hash_table_iter_next(&iter, &key, &value))
		g_key_file_set_string(file, CHOICES_GROUP, key, value);
	g_autofree char *path = get_choices_path();
	g_autofree char *directory = g_path_get_dirname(path);
	g_mkdir_with_parents(directory, 0700);
	g_key_file_save_to_file(file, path, NULL);
}


/* A group of versions is remembered by the song it is, so the listener's pick survives files being added */
static char *get_group_key(const Entry *entry) {
	g_autofree char *joined = g_strjoin("\x1f", entry->title, entry->artist, entry->album, NULL);
	return g_compute_checksum_for_string(G_CHECKSUM_SHA1, joined, -1);
}


/* Versions of the same song show as one, playing the listener's pick or else the best file, with the others kept for
 * the song's menu */
static GPtrArray *merge_versions(void) {
	g_hash_table_remove_all(versions);
	GPtrArray *shown = g_ptr_array_new();
	g_autofree gboolean *is_used = g_new0(gboolean, index_entries->len);
	for (guint index = 0; index < index_entries->len; index++) {
		if (is_used[index])
			continue;
		Entry *first = g_ptr_array_index(index_entries, index);
		GPtrArray *group = g_ptr_array_new();
		for (guint other = index; other < index_entries->len; other++) {
			Entry *entry = g_ptr_array_index(index_entries, other);
			if (!is_used[other] && is_same_song(first, entry)) {
				is_used[other] = TRUE;
				g_ptr_array_add(group, entry);
			}
		}
		g_autofree char *key = get_group_key(first);
		const char *chosen = g_hash_table_lookup(choices, key);
		Entry *pick = NULL;
		for (guint member = 0; member < group->len; member++) {
			Entry *entry = g_ptr_array_index(group, member);
			if (chosen ? g_strcmp0(entry->path, chosen) == 0 : !pick || is_better(entry, pick))
				pick = entry;
		}
		for (guint member = 0; !pick && member < group->len; member++) {
			Entry *entry = g_ptr_array_index(group, member);
			if (!pick || is_better(entry, pick))
				pick = entry;
		}
		g_ptr_array_add(shown, pick);
		for (guint member = 0; group->len > 1 && member < group->len; member++) {
			Entry *entry = g_ptr_array_index(group, member);
			g_hash_table_insert(versions, g_filename_to_uri(entry->path, NULL, NULL), g_ptr_array_ref(group));
		}
		g_ptr_array_unref(group);
	}
	return shown;
}


GPtrArray *list_local_versions(const char *id) {
	GPtrArray *found = g_ptr_array_new_with_free_func(g_free);
	GPtrArray *group = versions ? g_hash_table_lookup(versions, id) : NULL;
	for (guint index = 0; group && index < group->len; index++)
		g_ptr_array_add(found, g_filename_to_uri(((Entry *)g_ptr_array_index(group, index))->path, NULL, NULL));
	return found;
}


char *describe_local_version(const char *id) {
	g_autofree char *path = get_local_song_path(id);
	GPtrArray *group = versions ? g_hash_table_lookup(versions, id) : NULL;
	for (guint index = 0; path && group && index < group->len; index++) {
		Entry *entry = g_ptr_array_index(group, index);
		if (g_strcmp0(entry->path, path) != 0)
			continue;
		const char *dot = strrchr(entry->path, '.');
		g_autofree char *extension = g_ascii_strup(dot ? dot + 1 : "", -1);
		g_autofree char *size = g_format_size((guint64)entry->size);
		return g_strdup_printf("%s · %s", extension, size);
	}
	return g_strdup("");
}


void choose_local_version(const char *id) {
	g_autofree char *path = get_local_song_path(id);
	GPtrArray *group = versions ? g_hash_table_lookup(versions, id) : NULL;
	if (!path || !group)
		return;
	g_hash_table_insert(choices, get_group_key(g_ptr_array_index(group, 0)), g_strdup(path));
	save_choices();
	show_entries();
}


static void show_entries(void) {
	g_ptr_array_sort(index_entries, compare_entries);
	if (!versions) {
		versions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
		choices = load_choices();
	}
	g_autoptr(GPtrArray) shown = merge_versions();
	g_autoptr(GPtrArray) songs = g_ptr_array_new_with_free_func(g_object_unref);
	for (guint index = 0; index < shown->len; index++)
		g_ptr_array_add(songs, create_song(g_ptr_array_index(shown, index)));
	if (replace_songs(library, songs) && on_changed)
		on_changed(changed_data);
}


/* Only a song that has no picture of its own can change when its details arrive, by taking the cover found online */
void update_local_song_info(const char *id) {
	if (!library || !index_entries)
		return;
	for (guint index = 0; index < index_entries->len; index++) {
		Entry *entry = g_ptr_array_index(index_entries, index);
		g_autofree char *uri = g_filename_to_uri(entry->path, NULL, NULL);
		if (g_strcmp0(uri, id) == 0 && !*entry->cover && *get_song_info_value(id, INFO_COVER)) {
			show_entries();
			return;
		}
	}
}


/* Reading files, on a worker thread ========================================== */


static gboolean is_audio_file(const char *name) {
	const char *dot = strrchr(name, '.');
	if (!dot)
		return FALSE;
	g_autofree char *extension = g_ascii_strdown(dot + 1, -1);
	return g_strv_contains(AUDIO_EXTENSIONS, extension);
}


/* An embedded picture is kept once by its contents, so all songs of an album share one file */
static char *save_cover(GstSample *sample) {
	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstMapInfo map;
	if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ))
		return NULL;
	g_autofree char *name = g_compute_checksum_for_data(G_CHECKSUM_SHA1, map.data, map.size);
	g_autofree char *folder = get_cache_file(COVERS_FOLDER);
	char *path = g_build_filename(folder, name, NULL);
	if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
		g_mkdir_with_parents(folder, 0700);
		g_file_set_contents(path, (const char *)map.data, (gssize)map.size, NULL);
	}
	gst_buffer_unmap(buffer, &map);
	return path;
}


static char *find_folder_cover(const char *path) {
	g_autofree char *folder = g_path_get_dirname(path);
	for (guint index = 0; FOLDER_COVERS[index]; index++) {
		char *cover = g_build_filename(folder, FOLDER_COVERS[index], NULL);
		if (g_file_test(cover, G_FILE_TEST_EXISTS))
			return cover;
		g_free(cover);
	}
	return g_strdup("");
}


static char *read_tag(const GstTagList *tags, const char *tag) {
	char *value = NULL;
	return tags && gst_tag_list_get_string(tags, tag, &value) ? value : NULL;
}


/* A file without tags still shows, by its name and the folder it sits in */
static void read_entry(Scan *scan, Entry *entry) {
	g_autofree char *uri = g_filename_to_uri(entry->path, NULL, NULL);
	g_autoptr(GstDiscovererInfo) info = uri ? gst_discoverer_discover_uri(scan->discoverer, uri, NULL) : NULL;
	const GstTagList *tags = info ? gst_discoverer_info_get_tags(info) : NULL;
	entry->seconds = info ? (double)gst_discoverer_info_get_duration(info) / GST_SECOND : 0;
	entry->title = read_tag(tags, GST_TAG_TITLE);
	entry->artist = read_tag(tags, GST_TAG_ARTIST);
	entry->album = read_tag(tags, GST_TAG_ALBUM);
	if (!entry->title) {
		g_autofree char *name = g_path_get_basename(entry->path);
		char *dot = strrchr(name, '.');
		if (dot)
			*dot = '\0';
		entry->title = g_strdup(name);
	}
	if (!entry->artist)
		entry->artist = g_strdup("");
	if (!entry->album)
		entry->album = g_strdup("");
	entry->track = 0;
	if (tags)
		gst_tag_list_get_uint(tags, GST_TAG_TRACK_NUMBER, &entry->track);
	GstSample *sample = NULL;
	if (tags && (gst_tag_list_get_sample(tags, GST_TAG_IMAGE, &sample)
		|| gst_tag_list_get_sample(tags, GST_TAG_PREVIEW_IMAGE, &sample))) {
		entry->cover = save_cover(sample);
		gst_sample_unref(sample);
	}
	if (!entry->cover)
		entry->cover = find_folder_cover(entry->path);
}


static void walk_folder(Scan *scan, GFile *folder) {
	g_autoptr(GFileEnumerator) children = g_file_enumerate_children(folder, ENUMERATE_ATTRIBUTES,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (!children)
		return;
	g_ptr_array_add(scan->directories, g_file_get_path(folder));
	for (;;) {
		GFileInfo *info = NULL;
		GFile *child = NULL;
		if (!g_file_enumerator_iterate(children, &info, &child, NULL, NULL) || !info)
			break;
		if (g_file_info_get_is_hidden(info))
			continue;
		if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
			walk_folder(scan, child);
			continue;
		}
		if (!is_audio_file(g_file_info_get_name(info)))
			continue;
		g_autofree char *path = g_file_get_path(child);
		g_autoptr(GDateTime) time = g_file_info_get_modification_date_time(info);
		gint64 size = g_file_info_get_size(info), modified = time ? g_date_time_to_unix(time) : 0;
		Entry *known = g_hash_table_lookup(scan->known, path);
		if (known && known->size == size && known->modified == modified) {
			g_ptr_array_add(scan->entries, copy_entry(known));
			continue;
		}
		if (known)
			g_ptr_array_add(scan->changed, g_filename_to_uri(path, NULL, NULL));
		Entry *entry = g_new0(Entry, 1);
		entry->path = g_steal_pointer(&path);
		entry->size = size;
		entry->modified = modified;
		read_entry(scan, entry);
		g_ptr_array_add(scan->entries, entry);
	}
}


static void free_scan(gpointer data) {
	Scan *scan = data;
	g_strfreev(scan->folders);
	g_hash_table_unref(scan->known);
	g_ptr_array_unref(scan->entries);
	g_ptr_array_unref(scan->directories);
	g_ptr_array_unref(scan->changed);
	g_clear_object(&scan->discoverer);
	g_free(scan);
}


static void run_scan(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	Scan *scan = data;
	gst_init(NULL, NULL);
	scan->discoverer = gst_discoverer_new(DISCOVER_SECONDS * GST_SECOND, NULL);
	for (guint index = 0; scan->folders[index]; index++) {
		g_autoptr(GFile) folder = g_file_new_for_path(scan->folders[index]);
		walk_folder(scan, folder);
	}
	g_task_return_boolean(task, TRUE);
}


/* Watching ================================================================= */


static void start_scan(void);


static gboolean settle(gpointer data) {
	settle_source = 0;
	start_scan();
	return G_SOURCE_REMOVE;
}


static void notice_change(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data) {
	if (event == G_FILE_MONITOR_EVENT_CHANGED || event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
		return;
	if (settle_source)
		g_source_remove(settle_source);
	settle_source = g_timeout_add(SETTLE_MILLISECONDS, settle, NULL);
}


/* Each folder gets a watcher of its own, since one only sees its own folder and not those inside it */
static void watch_folders(GPtrArray *directories) {
	if (!monitors)
		monitors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	g_autoptr(GHashTable) wanted = g_hash_table_new(g_str_hash, g_str_equal);
	for (guint index = 0; index < directories->len; index++)
		g_hash_table_add(wanted, g_ptr_array_index(directories, index));
	GHashTableIter iter;
	gpointer path;
	g_hash_table_iter_init(&iter, monitors);
	while (g_hash_table_iter_next(&iter, &path, NULL)) {
		if (!g_hash_table_contains(wanted, path))
			g_hash_table_iter_remove(&iter);
	}
	for (guint index = 0; index < directories->len; index++) {
		const char *directory = g_ptr_array_index(directories, index);
		if (g_hash_table_contains(monitors, directory))
			continue;
		g_autoptr(GFile) file = g_file_new_for_path(directory);
		GFileMonitor *monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
		if (!monitor)
			continue;
		g_signal_connect(monitor, "changed", G_CALLBACK(notice_change), NULL);
		g_hash_table_insert(monitors, g_strdup(directory), monitor);
	}
}


static void finish_scan(GObject *source, GAsyncResult *result, gpointer data) {
	Scan *scan = g_task_get_task_data(G_TASK(result));
	is_scanning = FALSE;
	for (guint index = 0; index < scan->changed->len; index++)
		forget_song_info(g_ptr_array_index(scan->changed, index));
	g_ptr_array_unref(index_entries);
	index_entries = g_ptr_array_ref(scan->entries);
	save_index(index_entries);
	show_entries();
	gather_song_info(G_LIST_MODEL(library->songs));
	watch_folders(scan->directories);
	if (is_scan_wanted) {
		is_scan_wanted = FALSE;
		start_scan();
	}
}


/* Only files whose size or time changed are read again, so a check of an unchanged library takes a moment */
static void start_scan(void) {
	if (is_scanning) {
		is_scan_wanted = TRUE;
		return;
	}
	is_scanning = TRUE;
	Scan *scan = g_new0(Scan, 1);
	scan->folders = g_strdupv(get_settings()->media_folders);
	scan->known = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free_entry);
	for (guint index = 0; index < index_entries->len; index++) {
		Entry *copy = copy_entry(g_ptr_array_index(index_entries, index));
		g_hash_table_insert(scan->known, copy->path, copy);
	}
	scan->entries = g_ptr_array_new_with_free_func(free_entry);
	scan->directories = g_ptr_array_new_with_free_func(g_free);
	scan->changed = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GTask) task = g_task_new(NULL, NULL, finish_scan, NULL);
	g_task_set_task_data(task, scan, free_scan);
	g_task_run_in_thread(task, run_scan);
}


void start_local_library(CrimsonPlaylist *playlist, LocalLibraryChanged changed, gpointer data) {
	library = playlist;
	on_changed = changed;
	changed_data = data;
	index_entries = load_index();
	/* Without a saved index the library stays loading until its first check is done */
	if (index_entries->len)
		show_entries();
	start_scan();
}


void rescan_local_library(void) {
	if (library)
		start_scan();
}

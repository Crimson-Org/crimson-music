#include <sqlite3.h>

#include "../lang/en.h"
#include "common/identity.h"
#include "discover.h"

#define DATABASE_FILE "discover.db"
/* A song is stored once however many mixes hold it, and each mix keeps the order of its songs */
#define SCHEMA "CREATE TABLE IF NOT EXISTS mixes " \
	"(id INTEGER PRIMARY KEY, made TEXT, artwork TEXT, subtitle TEXT, signature TEXT UNIQUE);" \
	"CREATE TABLE IF NOT EXISTS songs (id TEXT PRIMARY KEY, song BLOB);" \
	"CREATE TABLE IF NOT EXISTS mix_songs (mix INTEGER, position INTEGER, song TEXT, PRIMARY KEY (mix, position))"

/* A kept mix with the words its card shows under the title */
typedef struct {
	gint64 id;
	char *made, *artwork, *signature, *detail;
	/* Which mix with this subtitle it is, counting from the first */
	guint number;
} Mix;

static sqlite3 *database;


static void free_mix(gpointer data) {
	Mix *mix = data;
	g_free(mix->made);
	g_free(mix->artwork);
	g_free(mix->signature);
	g_free(mix->detail);
	g_free(mix);
}


static gboolean open_database(void) {
	if (database)
		return TRUE;
	g_autofree char *folder = g_build_filename(g_get_user_data_dir(), get_application_identity()->slug, NULL);
	g_mkdir_with_parents(folder, 0700);
	g_autofree char *path = g_build_filename(folder, DATABASE_FILE, NULL);
	char *error = NULL;
	if (sqlite3_open(path, &database) != SQLITE_OK || sqlite3_exec(database, SCHEMA, NULL, NULL, &error) != SQLITE_OK) {
		g_warning("Discover mixes did not open: %s", error ? error : sqlite3_errmsg(database));
		sqlite3_free(error);
		sqlite3_close(database);
		database = NULL;
		return FALSE;
	}
	return TRUE;
}


static const char *read_column(sqlite3_stmt *row, int column) {
	const char *text = (const char *)sqlite3_column_text(row, column);
	return text ? text : "";
}


/* The same songs in another order are the same mix */
static char *sign_songs(GPtrArray *songs) {
	g_autoptr(GPtrArray) ids = g_ptr_array_new();
	for (guint index = 0; index < songs->len; index++)
		g_ptr_array_add(ids, ((CrimsonSong *)g_ptr_array_index(songs, index))->id);
	g_ptr_array_sort_values(ids, (GCompareFunc)g_strcmp0);
	g_ptr_array_add(ids, NULL);
	return g_strjoinv(" ", (char **)ids->pdata);
}


static void store_mix(GPtrArray *songs, const Card *card, const char *signature) {
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autofree char *made = g_date_time_format_iso8601(now);
	sqlite3_exec(database, "BEGIN", NULL, NULL, NULL);
	sqlite3_stmt *mix = NULL;
	sqlite3_prepare_v2(database, "INSERT INTO mixes (made, artwork, subtitle, signature) VALUES (?, ?, ?, ?)", -1,
		&mix, NULL);
	sqlite3_bind_text(mix, 1, made, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(mix, 2, card->artwork, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(mix, 3, card->subtitle, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(mix, 4, signature, -1, SQLITE_TRANSIENT);
	sqlite3_step(mix);
	sqlite3_finalize(mix);
	gint64 id = sqlite3_last_insert_rowid(database);
	sqlite3_stmt *song = NULL, *place = NULL;
	sqlite3_prepare_v2(database, "INSERT OR REPLACE INTO songs VALUES (?, ?)", -1, &song, NULL);
	sqlite3_prepare_v2(database, "INSERT INTO mix_songs VALUES (?, ?, ?)", -1, &place, NULL);
	for (guint index = 0; index < songs->len; index++) {
		CrimsonSong *kept = g_ptr_array_index(songs, index);
		g_autoptr(GBytes) bytes = save_song_bytes(kept);
		gsize size = 0;
		gconstpointer data = g_bytes_get_data(bytes, &size);
		sqlite3_bind_text(song, 1, kept->id, -1, SQLITE_TRANSIENT);
		sqlite3_bind_blob(song, 2, data, (int)size, SQLITE_TRANSIENT);
		sqlite3_step(song);
		sqlite3_reset(song);
		sqlite3_bind_int64(place, 1, id);
		sqlite3_bind_int(place, 2, (int)index);
		sqlite3_bind_text(place, 3, kept->id, -1, SQLITE_TRANSIENT);
		sqlite3_step(place);
		sqlite3_reset(place);
	}
	sqlite3_finalize(song);
	sqlite3_finalize(place);
	sqlite3_exec(database, "COMMIT", NULL, NULL, NULL);
}


static gboolean has_mix(const char *signature) {
	sqlite3_stmt *row = NULL;
	sqlite3_prepare_v2(database, "SELECT 1 FROM mixes WHERE signature = ?", -1, &row, NULL);
	sqlite3_bind_text(row, 1, signature, -1, SQLITE_TRANSIENT);
	gboolean is_kept = sqlite3_step(row) == SQLITE_ROW;
	sqlite3_finalize(row);
	return is_kept;
}


/* Every kept mix, oldest first, under the subtitle YouTube gave it, numbered from the first when several share one */
static GPtrArray *list_mixes(void) {
	GPtrArray *mixes = g_ptr_array_new_with_free_func(free_mix);
	g_autoptr(GHashTable) counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sqlite3_stmt *row = NULL;
	sqlite3_prepare_v2(database, "SELECT id, made, artwork, signature, subtitle FROM mixes ORDER BY made", -1, &row,
		NULL);
	while (row && sqlite3_step(row) == SQLITE_ROW) {
		Mix *mix = g_new0(Mix, 1);
		mix->id = sqlite3_column_int64(row, 0);
		mix->made = g_strdup(read_column(row, 1));
		mix->artwork = g_strdup(read_column(row, 2));
		mix->signature = g_strdup(read_column(row, 3));
		mix->detail = g_strdup(read_column(row, 4));
		mix->number = GPOINTER_TO_UINT(g_hash_table_lookup(counts, mix->detail)) + 1;
		g_hash_table_insert(counts, g_strdup(mix->detail), GUINT_TO_POINTER(mix->number));
		g_ptr_array_add(mixes, mix);
	}
	sqlite3_finalize(row);
	for (guint index = 0; index < mixes->len; index++) {
		Mix *mix = g_ptr_array_index(mixes, index);
		if (GPOINTER_TO_UINT(g_hash_table_lookup(counts, mix->detail)) < 2)
			continue;
		char *numbered = g_strdup_printf("%s #%u", mix->detail, mix->number);
		g_free(mix->detail);
		mix->detail = numbered;
	}
	return mixes;
}


static Card *create_mix_card(Mix *mix) {
	Card *card = g_new0(Card, 1);
	card->id = g_strdup_printf("%s%" G_GINT64_FORMAT, DISCOVER_MIX_PREFIX, mix->id);
	card->title = g_strdup(TEXT_DISCOVER_MIX);
	card->subtitle = g_strdup(mix->detail);
	card->artwork = g_strdup(mix->artwork);
	return card;
}


GPtrArray *keep_discover_mix(GPtrArray *songs, const Card *card) {
	GPtrArray *cards = g_ptr_array_new_with_free_func(free_card);
	g_autofree char *signature = songs->len ? sign_songs(songs) : NULL;
	if (signature && open_database() && !has_mix(signature))
		store_mix(songs, card, signature);
	g_autoptr(GPtrArray) mixes = open_database() ? list_mixes() : g_ptr_array_new();
	/* Without any kept mix YouTube's own card stands for the mix */
	if (!mixes->len) {
		g_ptr_array_add(cards, copy_card(card));
		return cards;
	}
	/* The mix it has now leads, and without its songs the newest kept mix stands for it */
	Mix *newest = g_ptr_array_index(mixes, mixes->len - 1);
	const char *current = signature ? signature : newest->signature;
	for (guint index = mixes->len; index > 0; index--) {
		Mix *mix = g_ptr_array_index(mixes, index - 1);
		if (g_str_equal(mix->signature, current))
			g_ptr_array_insert(cards, 0, create_mix_card(mix));
		else
			g_ptr_array_add(cards, create_mix_card(mix));
	}
	return cards;
}


GPtrArray *load_discover_mix(const char *id, char **detail, char **artwork) {
	GPtrArray *songs = g_ptr_array_new_with_free_func(g_object_unref);
	if (!open_database())
		return songs;
	gint64 wanted = g_ascii_strtoll(id + strlen(DISCOVER_MIX_PREFIX), NULL, 10);
	g_autoptr(GPtrArray) mixes = list_mixes();
	for (guint index = 0; index < mixes->len; index++) {
		Mix *mix = g_ptr_array_index(mixes, index);
		if (mix->id != wanted)
			continue;
		*detail = g_strdup(mix->detail);
		*artwork = g_strdup(mix->artwork);
	}
	sqlite3_stmt *row = NULL;
	sqlite3_prepare_v2(database, "SELECT songs.song FROM mix_songs JOIN songs ON songs.id = mix_songs.song "
		"WHERE mix_songs.mix = ? ORDER BY mix_songs.position", -1, &row, NULL);
	sqlite3_bind_int64(row, 1, wanted);
	while (row && sqlite3_step(row) == SQLITE_ROW) {
		g_autoptr(GBytes) bytes = g_bytes_new(sqlite3_column_blob(row, 0), (gsize)sqlite3_column_bytes(row, 0));
		g_ptr_array_add(songs, load_song_bytes(bytes));
	}
	sqlite3_finalize(row);
	return songs;
}


void forget_discover_mixes(void) {
	if (open_database())
		sqlite3_exec(database, "DELETE FROM mixes; DELETE FROM mix_songs; DELETE FROM songs", NULL, NULL, NULL);
}

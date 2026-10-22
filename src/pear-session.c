#include <sqlite3.h>

#include "pear-session.h"

/* Pear Desktop keeps its Electron profile under its old name, YouTube Music */
#define PEAR_PROFILE "YouTube Music"
#define PEAR_COOKIES "Cookies"
/* Chromium counts cookie expiry in microseconds since 1601, which is this many seconds before 1970 */
#define CHROMIUM_EPOCH_OFFSET 11644473600LL
#define MICROSECONDS_PER_SECOND 1000000LL
/* Only plain text values can be copied, since encrypted ones need the other program's keyring */
#define COOKIE_QUERY "SELECT host_key, name, value, path, expires_utc, is_secure, is_httponly, samesite " \
	"FROM cookies WHERE value != '' AND (host_key LIKE '%youtube.com' OR host_key LIKE '%google.com')"

/* Cookies still being added, so done runs once after the last one */
typedef struct {
	int remaining;
	GDestroyNotify done;
	gpointer data;
} ImportProgress;


static void finish_import(ImportProgress *progress) {
	progress->done(progress->data);
	g_free(progress);
}


static void finish_cookie(GObject *cookies, GAsyncResult *result, gpointer data) {
	ImportProgress *progress = data;
	g_autoptr(GError) error = NULL;
	if (!webkit_cookie_manager_add_cookie_finish(WEBKIT_COOKIE_MANAGER(cookies), result, &error))
		g_warning("A Pear Desktop cookie was not copied: %s", error->message);
	if (--progress->remaining == 0)
		finish_import(progress);
}


static SoupCookie *read_cookie(sqlite3_stmt *row) {
	SoupCookie *cookie = soup_cookie_new((const char *)sqlite3_column_text(row, 1),
		(const char *)sqlite3_column_text(row, 2), (const char *)sqlite3_column_text(row, 0),
		(const char *)sqlite3_column_text(row, 3), -1);
	sqlite3_int64 expiry = sqlite3_column_int64(row, 4);
	if (expiry > 0) {
		g_autoptr(GDateTime) expires = g_date_time_new_from_unix_utc(expiry / MICROSECONDS_PER_SECOND
			- CHROMIUM_EPOCH_OFFSET);
		soup_cookie_set_expires(cookie, expires);
	}
	soup_cookie_set_secure(cookie, sqlite3_column_int(row, 5));
	soup_cookie_set_http_only(cookie, sqlite3_column_int(row, 6));
	static const SoupSameSitePolicy POLICIES[] = {SOUP_SAME_SITE_POLICY_NONE, SOUP_SAME_SITE_POLICY_LAX,
		SOUP_SAME_SITE_POLICY_STRICT};
	int same_site = sqlite3_column_int(row, 7);
	if (same_site >= 0 && same_site < (int)G_N_ELEMENTS(POLICIES))
		soup_cookie_set_same_site_policy(cookie, POLICIES[same_site]);
	return cookie;
}


/* Reads without locking, since Pear Desktop may be running and holding its own database open */
static GPtrArray *read_pear_cookies(void) {
	GPtrArray *found = g_ptr_array_new_with_free_func((GDestroyNotify)soup_cookie_free);
	g_autofree char *path = g_build_filename(g_get_user_config_dir(), PEAR_PROFILE, PEAR_COOKIES, NULL);
	if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
		return found;
	g_autofree char *address = g_strconcat("file:", path, "?immutable=1", NULL);
	sqlite3 *database = NULL;
	sqlite3_stmt *row = NULL;
	if (sqlite3_open_v2(address, &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) == SQLITE_OK
		&& sqlite3_prepare_v2(database, COOKIE_QUERY, -1, &row, NULL) == SQLITE_OK) {
		while (sqlite3_step(row) == SQLITE_ROW)
			g_ptr_array_add(found, read_cookie(row));
	}
	sqlite3_finalize(row);
	sqlite3_close(database);
	return found;
}


void import_pear_session(WebKitCookieManager *cookies, GDestroyNotify done, gpointer data) {
	g_autoptr(GPtrArray) found = read_pear_cookies();
	ImportProgress *progress = g_new0(ImportProgress, 1);
	*progress = (ImportProgress){(int)found->len, done, data};
	if (found->len == 0) {
		finish_import(progress);
		return;
	}
	for (guint index = 0; index < found->len; index++)
		webkit_cookie_manager_add_cookie(cookies, g_ptr_array_index(found, index), NULL, finish_cookie, progress);
}

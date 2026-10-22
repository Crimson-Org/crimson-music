#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <math.h>

#include "collage.h"
#include "common/identity.h"

#define COLLAGE_FOLDER "collages"
#define COLLAGE_SIZE 600
#define COVER_COUNT 3
/* The front cover and the two behind it, which lean out to the sides */
#define FRONT_SIZE 310.0
#define BACK_SIZE 236.0
#define BACK_OFFSET 148.0
#define BACK_ANGLE 13.0
#define COVER_RADIUS 22.0
/* The wash behind is the first cover shrunk to a few pixels and grown back in doubling steps, since one big
 * stretch shows the pixels as blocks */
#define WASH_PIXELS 12
#define WASH_DARKNESS 0.45
#define SHADOW_STEPS 10
#define SHADOW_REACH 26.0
#define SHADOW_ALPHA 0.05
/* Covers are asked for large, since the cached list covers are too small for this size */
#define SIZE_PATTERN "=w\\d+-h\\d+"
#define SIZE_REPLACEMENT "=w544-h544"
#define VIDEO_PATTERN "/(mqdefault|hqdefault|sddefault|maxresdefault)\\.jpg.*$"
#define VIDEO_REPLACEMENT "/hq720.jpg"

typedef struct {
	char *path;
	cairo_surface_t *covers[COVER_COUNT];
	int count, waiting;
	CollageReady ready;
	gpointer data;
} Collage;

static SoupSession *session;


static char *get_collage_path(GPtrArray *addresses) {
	g_autoptr(GString) joined = g_string_new(NULL);
	for (guint index = 0; index < addresses->len; index++)
		g_string_append_printf(joined, "%s\n", (char *)g_ptr_array_index(addresses, index));
	g_autofree char *name = g_compute_checksum_for_string(G_CHECKSUM_SHA1, joined->str, -1);
	g_autofree char *file = g_strconcat(name, ".png", NULL);
	return g_build_filename(g_get_user_cache_dir(), get_application_identity()->slug, COLLAGE_FOLDER, file, NULL);
}


static char *enlarge_address(const char *address) {
	static GRegex *size, *video;
	if (!size) {
		size = g_regex_new(SIZE_PATTERN, G_REGEX_OPTIMIZE, 0, NULL);
		video = g_regex_new(VIDEO_PATTERN, G_REGEX_OPTIMIZE, 0, NULL);
	}
	g_autofree char *sized = g_regex_replace_literal(size, address, -1, 0, SIZE_REPLACEMENT, 0, NULL);
	return g_regex_replace_literal(video, sized, -1, 0, VIDEO_REPLACEMENT, 0, NULL);
}


/* A cover as a cairo image, cut to the middle square, since video covers are wide */
static cairo_surface_t *read_cover(GBytes *bytes) {
	g_autoptr(GdkTexture) texture = gdk_texture_new_from_bytes(bytes, NULL);
	if (!texture)
		return NULL;
	int width = gdk_texture_get_width(texture), height = gdk_texture_get_height(texture);
	cairo_surface_t *full = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cairo_surface_flush(full);
	gdk_texture_download(texture, cairo_image_surface_get_data(full), (gsize)cairo_image_surface_get_stride(full));
	cairo_surface_mark_dirty(full);
	int side = MIN(width, height);
	cairo_surface_t *square = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side);
	cairo_t *cairo = cairo_create(square);
	cairo_set_source_surface(cairo, full, -(width - side) / 2.0, -(height - side) / 2.0);
	cairo_paint(cairo);
	cairo_destroy(cairo);
	cairo_surface_destroy(full);
	return square;
}


static void rounded_square(cairo_t *cairo, double left, double top, double size, double radius) {
	cairo_new_sub_path(cairo);
	cairo_arc(cairo, left + size - radius, top + radius, radius, -G_PI / 2, 0);
	cairo_arc(cairo, left + size - radius, top + size - radius, radius, 0, G_PI / 2);
	cairo_arc(cairo, left + radius, top + size - radius, radius, G_PI / 2, G_PI);
	cairo_arc(cairo, left + radius, top + radius, radius, G_PI, 3 * G_PI / 2);
	cairo_close_path(cairo);
}


static void draw_wash(cairo_t *cairo, cairo_surface_t *cover) {
	cairo_surface_t *tiny = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, WASH_PIXELS, WASH_PIXELS);
	cairo_t *shrink = cairo_create(tiny);
	double side = cairo_image_surface_get_width(cover);
	cairo_scale(shrink, WASH_PIXELS / side, WASH_PIXELS / side);
	cairo_set_source_surface(shrink, cover, 0, 0);
	cairo_paint(shrink);
	cairo_destroy(shrink);
	int size = WASH_PIXELS;
	while (size < COLLAGE_SIZE) {
		int next = MIN(size * 2, COLLAGE_SIZE);
		cairo_surface_t *larger = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, next, next);
		cairo_t *grow = cairo_create(larger);
		cairo_scale(grow, (double)next / size, (double)next / size);
		cairo_set_source_surface(grow, tiny, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(grow), CAIRO_FILTER_GOOD);
		cairo_pattern_set_extend(cairo_get_source(grow), CAIRO_EXTEND_PAD);
		cairo_paint(grow);
		cairo_destroy(grow);
		cairo_surface_destroy(tiny);
		tiny = larger;
		size = next;
	}
	cairo_set_source_surface(cairo, tiny, 0, 0);
	cairo_paint(cairo);
	cairo_surface_destroy(tiny);
	cairo_pattern_t *shade = cairo_pattern_create_radial(COLLAGE_SIZE / 2.0, COLLAGE_SIZE / 2.0, COLLAGE_SIZE * 0.15,
		COLLAGE_SIZE / 2.0, COLLAGE_SIZE / 2.0, COLLAGE_SIZE * 0.75);
	cairo_pattern_add_color_stop_rgba(shade, 0, 0, 0, 0, WASH_DARKNESS * 0.4);
	cairo_pattern_add_color_stop_rgba(shade, 1, 0, 0, 0, WASH_DARKNESS);
	cairo_set_source(cairo, shade);
	cairo_paint(cairo);
	cairo_pattern_destroy(shade);
}


/* One cover, turned about its middle, over a soft shadow made of a few widening translucent layers */
static void draw_cover(cairo_t *cairo, cairo_surface_t *cover, double middle_x, double middle_y, double size,
	double degrees) {
	cairo_save(cairo);
	cairo_translate(cairo, middle_x, middle_y);
	cairo_rotate(cairo, degrees * G_PI / 180);
	for (int step = SHADOW_STEPS; step > 0; step--) {
		double reach = SHADOW_REACH * step / SHADOW_STEPS;
		rounded_square(cairo, -size / 2 - reach / 2, -size / 2 - reach / 4, size + reach, COVER_RADIUS + reach / 2);
		cairo_set_source_rgba(cairo, 0, 0, 0, SHADOW_ALPHA);
		cairo_fill(cairo);
	}
	rounded_square(cairo, -size / 2, -size / 2, size, COVER_RADIUS);
	cairo_clip(cairo);
	double side = cairo_image_surface_get_width(cover);
	cairo_scale(cairo, size / side, size / side);
	cairo_set_source_surface(cairo, cover, -side / 2, -side / 2);
	cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_BEST);
	cairo_paint(cairo);
	cairo_restore(cairo);
	cairo_save(cairo);
	cairo_translate(cairo, middle_x, middle_y);
	cairo_rotate(cairo, degrees * G_PI / 180);
	rounded_square(cairo, -size / 2 + 0.5, -size / 2 + 0.5, size - 1, COVER_RADIUS);
	cairo_set_source_rgba(cairo, 1, 1, 1, 0.12);
	cairo_set_line_width(cairo, 1.5);
	cairo_stroke(cairo);
	cairo_restore(cairo);
}


static void finish_collage(Collage *collage) {
	if (!collage->count) {
		collage->ready(NULL, collage->data);
	} else {
		cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, COLLAGE_SIZE, COLLAGE_SIZE);
		cairo_t *cairo = cairo_create(image);
		draw_wash(cairo, collage->covers[0]);
		double middle = COLLAGE_SIZE / 2.0;
		if (collage->count >= 3)
			draw_cover(cairo, collage->covers[2], middle + BACK_OFFSET, middle - 10, BACK_SIZE, BACK_ANGLE);
		if (collage->count >= 2)
			draw_cover(cairo, collage->covers[1], middle - BACK_OFFSET, middle - 10, BACK_SIZE, -BACK_ANGLE);
		draw_cover(cairo, collage->covers[0], middle, middle + 12, FRONT_SIZE, 0);
		cairo_destroy(cairo);
		g_autofree char *folder = g_path_get_dirname(collage->path);
		g_mkdir_with_parents(folder, 0700);
		gboolean is_saved = cairo_surface_write_to_png(image, collage->path) == CAIRO_STATUS_SUCCESS;
		cairo_surface_destroy(image);
		collage->ready(is_saved ? collage->path : NULL, collage->data);
	}
	for (int index = 0; index < COVER_COUNT; index++) {
		if (collage->covers[index])
			cairo_surface_destroy(collage->covers[index]);
	}
	g_free(collage->path);
	g_free(collage);
}


typedef struct {
	Collage *collage;
	int index;
	/* The smaller video cover every video has, tried when the large one is missing */
	char *fallback;
} CoverRequest;


static void receive_cover(GObject *source, GAsyncResult *result, gpointer data);


static void request_cover(CoverRequest *request, const char *address) {
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, address);
	if (!message) {
		g_clear_pointer(&request->fallback, g_free);
		receive_cover(NULL, NULL, request);
		return;
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_LOW, NULL, receive_cover, request);
}


/* Covers that failed leave gaps, which close up so the stack always starts at the front */
static void receive_cover(GObject *source, GAsyncResult *result, gpointer data) {
	CoverRequest *request = data;
	Collage *collage = request->collage;
	g_autoptr(GBytes) bytes = source ? soup_session_send_and_read_finish(SOUP_SESSION(source), result, NULL) : NULL;
	SoupMessage *message = source ? soup_session_get_async_result_message(SOUP_SESSION(source), result) : NULL;
	gboolean is_ok = bytes && message && SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(message));
	if (!is_ok && request->fallback) {
		g_autofree char *fallback = g_steal_pointer(&request->fallback);
		request_cover(request, fallback);
		return;
	}
	collage->covers[request->index] = is_ok ? read_cover(bytes) : NULL;
	g_free(request->fallback);
	g_free(request);
	if (--collage->waiting > 0)
		return;
	int kept = 0;
	for (int index = 0; index < COVER_COUNT; index++) {
		if (collage->covers[index])
			collage->covers[kept++] = collage->covers[index];
	}
	for (int index = kept; index < COVER_COUNT; index++)
		collage->covers[index] = NULL;
	collage->count = kept;
	finish_collage(collage);
}


typedef struct {
	char *path;
	CollageReady ready;
	gpointer data;
} KnownCollage;


static gboolean give_known_collage(gpointer data) {
	KnownCollage *known = data;
	known->ready(known->path, known->data);
	g_free(known->path);
	g_free(known);
	return G_SOURCE_REMOVE;
}


void make_collage(GPtrArray *addresses, CollageReady ready, gpointer data) {
	g_autofree char *path = get_collage_path(addresses);
	if (g_file_test(path, G_FILE_TEST_IS_REGULAR) || !addresses->len) {
		KnownCollage *known = g_new0(KnownCollage, 1);
		known->path = addresses->len ? g_steal_pointer(&path) : NULL;
		known->ready = ready;
		known->data = data;
		g_idle_add(give_known_collage, known);
		return;
	}
	if (!session)
		session = soup_session_new();
	Collage *collage = g_new0(Collage, 1);
	collage->path = g_steal_pointer(&path);
	collage->ready = ready;
	collage->data = data;
	collage->waiting = (int)MIN(addresses->len, COVER_COUNT);
	for (int index = 0; index < collage->waiting; index++) {
		const char *original = g_ptr_array_index(addresses, index);
		g_autofree char *address = enlarge_address(original);
		CoverRequest *request = g_new0(CoverRequest, 1);
		request->collage = collage;
		request->index = index;
		if (!g_str_equal(address, original))
			request->fallback = g_strdup(original);
		request_cover(request, address);
	}
}

#include <adwaita.h>
#include <libsoup/soup.h>

#include "ambience.h"
#include "artwork.h"
#include "common/identity.h"

#define CORNER_RADIUS 6
/* The clear pixels around a tilted cover's picture, which its edges blend into */
#define TILTED_BORDER 2
/* The glow reaches this far past the cover's middle, as a share of its size, and starts this opaque */
#define GLOW_REACH 0.9f
#define GLOW_ALPHA 0.55f
/* A spinning cover makes one turn while it grows from this share of its size and fades in */
#define SPIN_MILLISECONDS 1100
#define SPIN_DEGREES 360.0f
#define SPIN_START_SCALE 0.3f

#define ARTWORK_FOLDER "artwork"
#define ADDRESS_KEY "artwork-address"
#define LOADED_CLASS "loaded"
/* Covers kept decoded in memory, which at 120 pixels is about 60 KB each */
#define MEMORY_LIMIT 600
/* Google sizes its covers through the address, so every cover is asked for at one size that suits both lists
 * and the player bar on high density screens */
#define SIZE_PATTERN "=w\\d+-h\\d+"
#define SIZE_REPLACEMENT "=w120-h120"
/* Video stills come from i.ytimg.com in their own sizes, where the medium one is 320 pixels wide */
#define VIDEO_PATTERN "/(hq720|hqdefault|sddefault|maxresdefault)\\.jpg.*$"
#define VIDEO_REPLACEMENT "/mqdefault.jpg"

/* A cover on its way, with every cover widget waiting for it */
typedef struct {
	char *address;
	char *path;
	GPtrArray *covers;
} ArtworkRequest;

/* A square cover that crops its picture to fill, whatever shape the picture has */
#define PLACEHOLDER_ICON "audio-x-generic-symbolic"
#define EMBLEM_PREFIX "emblem:"
/* An emblem's symbol takes this share of the cover */
#define EMBLEM_ICON_SCALE 0.5f

/* An accent emblem runs from the accent lightened by this share at the top left, as YouTube Music's own covers do,
 * to the accent darkened by the other at the bottom right */
#define EMBLEM_LIGHT 0.35f
#define EMBLEM_SHADE 0.15f

/* A cover drawn as a white symbol on the system's accent colour, darkening toward the bottom right */
typedef struct {
	const char *name, *icon;
} Emblem;

static const Emblem EMBLEMS[] = {
	{"local-library", "folder-music-symbolic"},
	{"home", "go-home-symbolic"},
	{"history", "document-open-recent-symbolic"},
};
#define PLACEHOLDER_SHADE 0.2f
#define PLACEHOLDER_ICON_SCALE 0.45f
#define PLACEHOLDER_ICON_ALPHA 0.45f

struct _CrimsonCover {
	GtkWidget parent;
	int size;
	GdkTexture *texture;
	gboolean has_glow;
	GdkRGBA glow;
	/* How far the spin in has come, from 0 to 1, which is 1 for a cover that does not spin */
	double spin;
	AdwAnimation *spinning;
	/* The pixels the cover asks for, or 0 for the small size most covers use */
	int resolution;
	/* The picture drawn for a tilted cover and the screen scale it was drawn at */
	GdkTexture *tilted;
	int tilted_scale;
	/* The song has no picture at all, such as a local file without one, so a music note stands in */
	gboolean is_blank;
	/* The drawn emblem the cover shows instead of a picture, or NULL */
	const Emblem *emblem;
};

G_DEFINE_FINAL_TYPE(CrimsonCover, crimson_cover, GTK_TYPE_WIDGET)

enum {
	COVER_PROPERTY_TEXTURE = 1,
	COVER_PROPERTY_COUNT,
};

static GParamSpec *cover_properties[COVER_PROPERTY_COUNT];

static SoupSession *session;
static GHashTable *textures;
static GQueue *texture_order;
static GHashTable *requests;


static void measure_cover(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
	int *minimum_baseline, int *natural_baseline) {
	*minimum = *natural = CRIMSON_COVER(widget)->size;
}


/* A round glow drawn by the cover itself looks the same at any turn, where a CSS shadow breaks up while the cover
 * spins */
static void draw_glow(GtkSnapshot *snapshot, GdkRGBA color, float size) {
	float reach = size * GLOW_REACH;
	GdkRGBA clear = color;
	color.alpha = GLOW_ALPHA;
	clear.alpha = 0;
	GskColorStop stops[] = {{0.5f, color}, {1, clear}};
	graphene_rect_t area = GRAPHENE_RECT_INIT(size / 2 - reach, size / 2 - reach, reach * 2, reach * 2);
	gtk_snapshot_append_radial_gradient(snapshot, &area, &GRAPHENE_POINT_INIT(size / 2, size / 2), reach, reach, 0,
		1, stops, G_N_ELEMENTS(stops));
}


static void draw_symbol(GtkWidget *widget, GtkSnapshot *snapshot, const char *name, float size, float scale,
	GdkRGBA color) {
	int icon_size = (int)(size * scale);
	g_autoptr(GtkIconPaintable) icon = gtk_icon_theme_lookup_icon(
		gtk_icon_theme_get_for_display(gtk_widget_get_display(widget)), name, NULL, icon_size,
		gtk_widget_get_scale_factor(widget), gtk_widget_get_direction(widget), 0);
	gtk_snapshot_save(snapshot);
	gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT((size - icon_size) / 2, (size - icon_size) / 2));
	gtk_symbolic_paintable_snapshot_symbolic(GTK_SYMBOLIC_PAINTABLE(icon), snapshot, icon_size, icon_size, &color, 1);
	gtk_snapshot_restore(snapshot);
}


static void draw_placeholder(GtkWidget *widget, GtkSnapshot *snapshot, float size) {
	GdkRGBA color;
	gtk_widget_get_color(widget, &color);
	/* A solid grey a little toward the text colour, so stacked covers never show through each other */
	GdkRGBA fill = {
		color.red * PLACEHOLDER_SHADE + (1 - color.red) * (1 - PLACEHOLDER_SHADE),
		color.green * PLACEHOLDER_SHADE + (1 - color.green) * (1 - PLACEHOLDER_SHADE),
		color.blue * PLACEHOLDER_SHADE + (1 - color.blue) * (1 - PLACEHOLDER_SHADE),
		1,
	};
	GskRoundedRect shape;
	gsk_rounded_rect_init_from_rect(&shape, &GRAPHENE_RECT_INIT(0, 0, size, size),
		gtk_widget_has_css_class(widget, "round") ? size / 2 : CORNER_RADIUS);
	gtk_snapshot_push_rounded_clip(snapshot, &shape);
	gtk_snapshot_append_color(snapshot, &fill, &GRAPHENE_RECT_INIT(0, 0, size, size));
	gtk_snapshot_pop(snapshot);
	color.alpha *= PLACEHOLDER_ICON_ALPHA;
	draw_symbol(widget, snapshot, PLACEHOLDER_ICON, size, PLACEHOLDER_ICON_SCALE, color);
}


static void trace_rounded_square(cairo_t *context, double left, double side, double radius) {
	cairo_new_sub_path(context);
	cairo_arc(context, left + side - radius, left + radius, radius, -G_PI / 2, 0);
	cairo_arc(context, left + side - radius, left + side - radius, radius, 0, G_PI / 2);
	cairo_arc(context, left + radius, left + side - radius, radius, G_PI / 2, G_PI);
	cairo_arc(context, left + radius, left + radius, radius, G_PI, 3 * G_PI / 2);
	cairo_close_path(context);
}


/* The picture with its rounded corners drawn in and a clear border around it, at the screen's pixels */
static GdkTexture *draw_tilted_picture(CrimsonCover *cover, int scale) {
	int width = gdk_texture_get_width(cover->texture);
	int height = gdk_texture_get_height(cover->texture);
	cairo_surface_t *picture = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	gdk_texture_download(cover->texture, cairo_image_surface_get_data(picture), cairo_image_surface_get_stride(picture));
	cairo_surface_mark_dirty(picture);
	int pixels = cover->size * scale;
	int full = pixels + 2 * TILTED_BORDER;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, full, full);
	cairo_t *context = cairo_create(surface);
	trace_rounded_square(context, TILTED_BORDER, pixels, CORNER_RADIUS * scale);
	cairo_clip(context);
	double fit = MAX((double)pixels / width, (double)pixels / height);
	cairo_translate(context, TILTED_BORDER + (pixels - width * fit) / 2, TILTED_BORDER + (pixels - height * fit) / 2);
	cairo_scale(context, fit, fit);
	cairo_set_source_surface(context, picture, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(context), CAIRO_FILTER_GOOD);
	cairo_paint(context);
	cairo_destroy(context);
	cairo_surface_flush(surface);
	int stride = cairo_image_surface_get_stride(surface);
	g_autoptr(GBytes) bytes = g_bytes_new(cairo_image_surface_get_data(surface), (gsize)stride * full);
	GdkTexture *drawn = gdk_memory_texture_new(full, full, GDK_MEMORY_DEFAULT, bytes, stride);
	cairo_surface_destroy(surface);
	cairo_surface_destroy(picture);
	return drawn;
}


/* A tilted cover is drawn from a picture whose corners and edges are already smooth, since GTK leaves the
 * edges of a clip jagged once it is turned, while a picture with a clear border blends into its surroundings */
static void draw_tilted_cover(CrimsonCover *cover, GtkSnapshot *snapshot) {
	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(cover));
	if (!cover->tilted || cover->tilted_scale != scale) {
		g_set_object(&cover->tilted, NULL);
		cover->tilted = draw_tilted_picture(cover, scale);
		cover->tilted_scale = scale;
	}
	float border = (float)TILTED_BORDER / (float)scale;
	float side = (float)cover->size + 2 * border;
	gtk_snapshot_append_scaled_texture(snapshot, cover->tilted, GSK_SCALING_FILTER_LINEAR,
		&GRAPHENE_RECT_INIT(-border, -border, side, side));
}


static void draw_emblem(GtkWidget *widget, GtkSnapshot *snapshot, const Emblem *emblem, float size) {
	g_autofree GdkRGBA *accent = adw_style_manager_get_accent_color_rgba(adw_style_manager_get_default());
	GdkRGBA shade = {accent->red * (1 - EMBLEM_SHADE), accent->green * (1 - EMBLEM_SHADE),
		accent->blue * (1 - EMBLEM_SHADE), 1};
	GdkRGBA light = {accent->red + (1 - accent->red) * EMBLEM_LIGHT, accent->green + (1 - accent->green) * EMBLEM_LIGHT,
		accent->blue + (1 - accent->blue) * EMBLEM_LIGHT, 1};
	GskColorStop stops[] = {{0, light}, {1, shade}};
	graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, size, size);
	GskRoundedRect shape;
	gsk_rounded_rect_init_from_rect(&shape, &bounds, CORNER_RADIUS);
	gtk_snapshot_push_rounded_clip(snapshot, &shape);
	gtk_snapshot_append_linear_gradient(snapshot, &bounds, &GRAPHENE_POINT_INIT(0, 0),
		&GRAPHENE_POINT_INIT(size, size), stops, G_N_ELEMENTS(stops));
	gtk_snapshot_pop(snapshot);
	draw_symbol(widget, snapshot, emblem->icon, size, EMBLEM_ICON_SCALE, (GdkRGBA){1, 1, 1, 1});
}


static void draw_cover(GtkWidget *widget, GtkSnapshot *snapshot) {
	CrimsonCover *cover = CRIMSON_COVER(widget);
	if (cover->emblem) {
		draw_emblem(widget, snapshot, cover->emblem, (float)cover->size);
		return;
	}
	if (!cover->texture && cover->is_blank)
		draw_placeholder(widget, snapshot, (float)cover->size);
	if (!cover->texture)
		return;
	float size = (float)cover->size;
	float width = (float)gdk_texture_get_width(cover->texture);
	float height = (float)gdk_texture_get_height(cover->texture);
	float scale = MAX(size / width, size / height);
	if (gtk_widget_has_css_class(widget, "tilted")) {
		draw_tilted_cover(cover, snapshot);
		return;
	}
	float progress = (float)cover->spin;
	gtk_snapshot_push_opacity(snapshot, progress);
	gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(size / 2, size / 2));
	gtk_snapshot_rotate(snapshot, (progress - 1) * SPIN_DEGREES);
	float grown = SPIN_START_SCALE + (1 - SPIN_START_SCALE) * progress;
	gtk_snapshot_scale(snapshot, grown, grown);
	gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(-size / 2, -size / 2));
	if (cover->has_glow)
		draw_glow(snapshot, cover->glow, size);
	graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, size, size);
	GskRoundedRect clip;
	gsk_rounded_rect_init_from_rect(&clip, &bounds, gtk_widget_has_css_class(widget, "round") ? size / 2 : CORNER_RADIUS);
	gtk_snapshot_push_rounded_clip(snapshot, &clip);
	graphene_rect_t area = GRAPHENE_RECT_INIT((size - width * scale) / 2, (size - height * scale) / 2,
		width * scale, height * scale);
	gtk_snapshot_append_texture(snapshot, cover->texture, &area);
	gtk_snapshot_pop(snapshot);
	gtk_snapshot_pop(snapshot);
}


static void dispose_cover(GObject *object) {
	g_clear_object(&CRIMSON_COVER(object)->texture);
	g_clear_object(&CRIMSON_COVER(object)->tilted);
	g_clear_object(&CRIMSON_COVER(object)->spinning);
	G_OBJECT_CLASS(crimson_cover_parent_class)->dispose(object);
}


static void get_cover_property(GObject *object, guint id, GValue *value, GParamSpec *spec) {
	if (id == COVER_PROPERTY_TEXTURE)
		g_value_set_object(value, CRIMSON_COVER(object)->texture);
}


static void crimson_cover_class_init(CrimsonCoverClass *type) {
	G_OBJECT_CLASS(type)->dispose = dispose_cover;
	G_OBJECT_CLASS(type)->get_property = get_cover_property;
	cover_properties[COVER_PROPERTY_TEXTURE] = g_param_spec_object("texture", NULL, NULL, GDK_TYPE_TEXTURE,
		G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(G_OBJECT_CLASS(type), COVER_PROPERTY_COUNT, cover_properties);
	GTK_WIDGET_CLASS(type)->measure = measure_cover;
	GTK_WIDGET_CLASS(type)->snapshot = draw_cover;
	gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(type), "cover");
}


static void crimson_cover_init(CrimsonCover *cover) {
	cover->spin = 1;
}


GtkWidget *create_cover(int size) {
	CrimsonCover *cover = g_object_new(CRIMSON_TYPE_COVER, NULL);
	cover->size = size;
	return GTK_WIDGET(cover);
}


static void set_cover_texture(CrimsonCover *cover, GdkTexture *texture) {
	if (!g_set_object(&cover->texture, texture))
		return;
	g_clear_object(&cover->tilted);
	if (texture && cover->has_glow)
		cover->glow = read_cover_accent(texture);
	gtk_widget_queue_draw(GTK_WIDGET(cover));
	g_object_notify_by_pspec(G_OBJECT(cover), cover_properties[COVER_PROPERTY_TEXTURE]);
}


GdkTexture *get_cover_texture(GtkWidget *cover) {
	return CRIMSON_COVER(cover)->texture;
}


static void free_request(ArtworkRequest *request) {
	g_free(request->address);
	g_free(request->path);
	g_ptr_array_unref(request->covers);
	g_free(request);
}


static void remember_texture(const char *address, GdkTexture *texture) {
	if (g_hash_table_contains(textures, address))
		return;
	char *key = g_strdup(address);
	g_hash_table_insert(textures, key, g_object_ref(texture));
	g_queue_push_tail(texture_order, key);
	if (g_queue_get_length(texture_order) > MEMORY_LIMIT)
		g_hash_table_remove(textures, g_queue_pop_head(texture_order));
}


/* A cover from memory arrives in the same frame the class was removed, so only a waited for cover fades */
static void give_texture(CrimsonCover *cover, const char *address, GdkTexture *texture) {
	if (g_strcmp0(g_object_get_data(G_OBJECT(cover), ADDRESS_KEY), address) != 0)
		return;
	set_cover_texture(cover, texture);
	gtk_widget_add_css_class(GTK_WIDGET(cover), LOADED_CLASS);
}


static void finish_request(ArtworkRequest *request, GdkTexture *texture) {
	g_hash_table_steal(requests, request->address);
	if (texture) {
		remember_texture(request->address, texture);
		for (guint index = 0; index < request->covers->len; index++)
			give_texture(g_ptr_array_index(request->covers, index), request->address, texture);
	}
	free_request(request);
}


/* One cover for a worker thread, with downloaded bytes to save and decode, or none to read them from disk */
typedef struct {
	char *path;
	GBytes *bytes;
	gboolean was_downloaded;
} CoverWork;


static void free_cover_work(CoverWork *work) {
	g_free(work->path);
	g_clear_pointer(&work->bytes, g_bytes_unref);
	g_free(work);
}


/* Covers are read, saved and decoded away from the window, so a page full of new covers never holds up a frame */
static void load_in_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	CoverWork *work = data;
	if (work->was_downloaded) {
		g_autofree char *directory = g_path_get_dirname(work->path);
		g_mkdir_with_parents(directory, 0700);
		g_file_set_contents(work->path, g_bytes_get_data(work->bytes, NULL), (gssize)g_bytes_get_size(work->bytes),
			NULL);
	} else {
		g_autoptr(GFile) file = g_file_new_for_path(work->path);
		work->bytes = g_file_load_bytes(file, NULL, NULL, NULL);
	}
	GdkTexture *texture = work->bytes ? gdk_texture_new_from_bytes(work->bytes, NULL) : NULL;
	g_task_return_pointer(task, texture, texture ? g_object_unref : NULL);
}


static void download_artwork(ArtworkRequest *request);


/* A cover missing from disk is downloaded, and one that fails to decode after downloading is left out */
static void finish_load(GObject *source, GAsyncResult *result, gpointer data) {
	ArtworkRequest *request = data;
	CoverWork *work = g_task_get_task_data(G_TASK(result));
	g_autoptr(GdkTexture) texture = g_task_propagate_pointer(G_TASK(result), NULL);
	if (!texture && !work->was_downloaded)
		download_artwork(request);
	else
		finish_request(request, texture);
}


static void load_artwork(ArtworkRequest *request, GBytes *downloaded) {
	CoverWork *work = g_new0(CoverWork, 1);
	work->path = g_strdup(request->path);
	work->bytes = downloaded ? g_bytes_ref(downloaded) : NULL;
	work->was_downloaded = downloaded != NULL;
	GTask *task = g_task_new(NULL, NULL, finish_load, request);
	g_task_set_task_data(task, work, (GDestroyNotify)free_cover_work);
	g_task_run_in_thread(task, load_in_thread);
	g_object_unref(task);
}


static void finish_download(GObject *source, GAsyncResult *result, gpointer data) {
	ArtworkRequest *request = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
	SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(source), result);
	gboolean is_ok = bytes && message && SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(message));
	if (is_ok)
		load_artwork(request, bytes);
	else
		finish_request(request, NULL);
}


static void download_artwork(ArtworkRequest *request) {
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, request->address);
	if (!message) {
		finish_request(request, NULL);
		return;
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_LOW, NULL, finish_download, request);
}


static char *get_artwork_path(const char *address) {
	g_autofree char *name = g_compute_checksum_for_string(G_CHECKSUM_SHA1, address, -1);
	return g_build_filename(g_get_user_cache_dir(), get_application_identity()->slug, ARTWORK_FOLDER, name, NULL);
}


static void prepare_artwork(void) {
	if (session)
		return;
	session = soup_session_new();
	textures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	texture_order = g_queue_new();
	requests = g_hash_table_new(g_str_hash, g_str_equal);
}


/* Small covers ask YouTube for a small picture, and covers that need more ask for that many pixels */
static char *size_address(const char *address, int pixels) {
	static GRegex *size, *video;
	if (!size) {
		size = g_regex_new(SIZE_PATTERN, G_REGEX_OPTIMIZE, 0, NULL);
		video = g_regex_new(VIDEO_PATTERN, G_REGEX_OPTIMIZE, 0, NULL);
	}
	g_autofree char *replacement = pixels > 0 ? g_strdup_printf("=w%d-h%d", pixels, pixels)
		: g_strdup(SIZE_REPLACEMENT);
	g_autofree char *sized = g_regex_replace_literal(size, address, -1, 0, replacement, 0, NULL);
	return pixels > 0 ? g_steal_pointer(&sized) : g_regex_replace_literal(video, sized, -1, 0, VIDEO_REPLACEMENT, 0, NULL);
}


static const Emblem *find_emblem(const char *address) {
	if (!address || !g_str_has_prefix(address, EMBLEM_PREFIX))
		return NULL;
	for (guint index = 0; index < G_N_ELEMENTS(EMBLEMS); index++) {
		if (g_str_equal(address + strlen(EMBLEM_PREFIX), EMBLEMS[index].name))
			return &EMBLEMS[index];
	}
	return NULL;
}


void show_artwork(GtkWidget *widget, const char *address) {
	CrimsonCover *cover = CRIMSON_COVER(widget);
	prepare_artwork();
	cover->emblem = find_emblem(address);
	if (cover->emblem && !g_object_get_data(G_OBJECT(cover), "follows-accent")) {
		g_object_set_data(G_OBJECT(cover), "follows-accent", GINT_TO_POINTER(TRUE));
		g_signal_connect_object(adw_style_manager_get_default(), "notify::accent-color",
			G_CALLBACK(gtk_widget_queue_draw), cover, G_CONNECT_SWAPPED);
	}
	if (cover->emblem) {
		g_object_set_data(G_OBJECT(cover), ADDRESS_KEY, NULL);
		set_cover_texture(cover, NULL);
		gtk_widget_add_css_class(widget, LOADED_CLASS);
		gtk_widget_queue_draw(widget);
		return;
	}
	g_autofree char *sized = address && *address ? size_address(address, cover->resolution) : NULL;
	g_object_set_data_full(G_OBJECT(cover), ADDRESS_KEY, g_strdup(sized), g_free);
	set_cover_texture(cover, NULL);
	gtk_widget_remove_css_class(widget, LOADED_CLASS);
	cover->is_blank = address && !*address;
	if (cover->is_blank)
		gtk_widget_add_css_class(widget, LOADED_CLASS);
	gtk_widget_queue_draw(widget);
	if (!sized)
		return;
	GdkTexture *texture = g_hash_table_lookup(textures, sized);
	if (texture) {
		give_texture(cover, sized, texture);
		return;
	}
	ArtworkRequest *request = g_hash_table_lookup(requests, sized);
	if (!request) {
		request = g_new0(ArtworkRequest, 1);
		request->address = g_strdup(sized);
		request->path = sized[0] == '/' ? g_strdup(sized) : get_artwork_path(sized);
		request->covers = g_ptr_array_new_with_free_func(g_object_unref);
		g_hash_table_insert(requests, request->address, request);
		load_artwork(request, NULL);
	}
	g_ptr_array_add(request->covers, g_object_ref(cover));
}


void set_cover_resolution(GtkWidget *cover, int pixels) {
	CRIMSON_COVER(cover)->resolution = pixels;
}


void add_cover_glow(GtkWidget *cover) {
	CRIMSON_COVER(cover)->has_glow = TRUE;
}


static void set_spin(double value, gpointer data) {
	CrimsonCover *cover = data;
	cover->spin = value;
	gtk_widget_queue_draw(GTK_WIDGET(cover));
}


/* Turns the cover once while it grows and fades in, slowing steadily to rest without a long crawl at the end */
void spin_cover_in(GtkWidget *widget) {
	CrimsonCover *cover = CRIMSON_COVER(widget);
	if (!cover->spinning) {
		AdwAnimationTarget *target = adw_callback_animation_target_new(set_spin, cover, NULL);
		cover->spinning = adw_timed_animation_new(widget, 0, 1, SPIN_MILLISECONDS, target);
		adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(cover->spinning), ADW_EASE_OUT_CUBIC);
	}
	adw_animation_reset(cover->spinning);
	adw_animation_play(cover->spinning);
}

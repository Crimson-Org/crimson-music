#include <math.h>
#include <string.h>

#include <adwaita.h>

#include "ambience.h"

/* The veil over the colours, near black or near white with the style, keeps text readable and the colours calm */
#define DARK_VEIL ((GdkRGBA){0.07f, 0.07f, 0.08f, 0.6f})
#define LIGHT_VEIL ((GdkRGBA){0.98f, 0.98f, 0.99f, 0.6f})
#define DARK_EDGE ((GdkRGBA){0, 0, 0, 0.45f})
#define LIGHT_EDGE ((GdkRGBA){1, 1, 1, 0.45f})
#define EDGE_WIDTH 270
#define GRAIN_SIZE 128
/* How far the grain strays, as a share of full brightness either way */
#define GRAIN_DEPTH (12.0 / 255)
#define CROSSFADE_MILLISECONDS 900
/* The backdrop moves slowly enough that twelve pictures a second look smooth on any screen and spare the processor */
#define FRAME_MILLISECONDS (1000 / 12)
/* Colours are averaged over this many pixels along each side of the cover */
#define SAMPLE_SIZE 24
/* The cover is read as this many columns and rows, one colour from each */
#define PALETTE_COLUMNS 3
#define PALETTE_ROWS 2
#define PALETTE_SIZE (PALETTE_COLUMNS * PALETTE_ROWS)
/* Patches are sized against the backdrop's height, so a wide bar shows several distinct shapes */
#define PATCH_HEIGHT_SCALE 1.05
#define PATCH_WIDTH_SCALE 0.045
/* Each colour drifts as this many patches, apart on their paths, so many small shapes show rather than one wash */
#define PATCH_COPIES 2
#define PATCH_COUNT (PALETTE_SIZE * PATCH_COPIES)
/* The colours change slowly across the backdrop, so they are worked out once per square of this many pixels and
 * stretched smoothly over it as one small picture */
#define COLOR_CELL 8
/* The player's patches are this much more saturated and bright, and drawn at this strength */
#define VIVID_SATURATION 1.5f
#define VIVID_VALUE 1.2f
#define VIVID_PATCH_ALPHA 0.5f
/* How much each patch swells and shrinks as it drifts */
#define PATCH_BREATH 0.25
/* Keeps the accent bright enough for white icons on it and dark enough for its own colour on a light page */
#define ACCENT_VALUE_MINIMUM 0.55
#define ACCENT_VALUE_MAXIMUM 0.8
#define ACCENT_SATURATION_MINIMUM 0.45
/* Keeps the backdrop colours rich enough to show through the veil without glaring */
#define BLOB_SATURATION_MINIMUM 0.35
#define BLOB_VALUE_MINIMUM 0.35
#define BLOB_VALUE_MAXIMUM 0.75
/* Covers are read at up to this many pixels, counted as colours of four bits a channel */
#define DISTINCT_PIXELS 60000
#define QUANTIZED_COLORS 4096
/* Material's scoring constants, from material-color-utilities */
#define CHROMA_SCALE 300.0
#define DISTINCT_COLORS 2
/* Shades step this share of the hue circle and this much brightness away from their colour, and a lone colour
 * turns this far for its partner */
#define SHADE_TURN 0.03
#define SHADE_LIGHTEN 0.12
#define HUE_TURN 0.08
/* Material's scoring constants, from material-color-utilities */
#define TARGET_CHROMA 48.0
#define WEIGHT_SHARE 0.7
#define WEIGHT_CHROMA_ABOVE 0.3
#define WEIGHT_CHROMA_BELOW 0.1
#define CUTOFF_CHROMA 5.0
#define CUTOFF_SHARE 0.01

/* How strongly the grain darkens and lightens, which matches soft light blending over the veil of each style without
 * the offscreen passes a blend costs on every frame */
typedef struct {
	double darken, lighten;
} GrainStrength;

static const GrainStrength DARK_GRAIN = {1.6, 0.6};
static const GrainStrength LIGHT_GRAIN = {0.4, 0.95};

/* One drifting patch of colour, with speeds in turns per minute, a starting point on its path and its size */
typedef struct {
	double horizontal_speed, vertical_speed, breath_speed, phase, scale;
} Drift;

/* Speeds that share no common beat, so the patches weave around each other without settling into a loop */
static const Drift DRIFTS[PALETTE_SIZE] = {
	{2.3, 3.1, 1.7, 0.0, 1.0},
	{3.7, 2.2, 2.3, 1.1, 0.8},
	{1.9, 4.1, 1.3, 2.3, 1.1},
	{4.3, 2.9, 2.9, 3.4, 0.7},
	{2.9, 3.7, 1.9, 4.6, 0.9},
	{3.3, 1.7, 2.6, 5.7, 0.85},
};

/* The colours read from one cover, the first being the whole cover's and the rest one per part of it */
typedef struct {
	GdkRGBA base, parts[PALETTE_SIZE];
} Palette;

/* Where one patch of colour sits at the moment, how far it reaches and its colour */
typedef struct {
	float x, y, radius;
	GdkRGBA color;
} Patch;

/* Of the part of a tall picture that does not fit, the share cut from its top drifts slowly between these two and
 * back, starting halfway between them, so the picture rises and sinks a little as if breathing */
#define PICTURE_TOP_START 0.4
#define PICTURE_TOP_END 0.1
#define PICTURE_DRIFT_SECONDS 48.0

/* The picture across a heading: how much of the width it covers, how much of it stays solid, how it blends, how
 * much shows as itself over the blend, and how long it takes to fade in */
#define PICTURE_SHARE 0.6f
#define PICTURE_SOLID 0.4f
#define PICTURE_CLEAR 0.85f
#define PICTURE_TILT 0.6f
#define PICTURE_BLEND GSK_BLEND_MODE_OVERLAY
#define PICTURE_PLAIN_SHARE 0.5f
#define REVEAL_MILLISECONDS 600

struct _CrimsonAmbience {
	GtkWidget parent;
	GtkWidget *child;
	GdkTexture *current;
	Palette palettes[2];
	/* How many palettes are known, the newest last, crossfading over the one before */
	guint palette_count;
	double mix;
	AdwAnimation *crossfade;
	GtkCssProvider *accent;
	/* When the colours last moved, so they resume where they stopped, and the timer moving them */
	gint64 last_move_time;
	guint timer;
	/* The song plays, so the colours drift whenever the window has focus */
	gboolean is_playing;
	GtkWindow *window;
	double seconds;
	/* The colours, veil and edges painted as one small picture, painted again once anything in them changed */
	GdkTexture *colors;
	gboolean are_colors_stale;
	gboolean has_faded_edges;
	/* The listener turned the cover colours off, so a plain background and the usual accent show instead */
	gboolean is_plain;
	/* Each backdrop styles only itself through a class of its own, and each accent through a class of that */
	char *style_class, *accent_class;
	guint accent_count;
	/* A picture across the left of the backdrop, fading in once loaded */
	GdkTexture *picture;
	AdwAnimation *reveal;
	double revealed;
	/* How long the picture has drifted, which carries on whenever the window has focus */
	double picture_seconds;
};

G_DEFINE_FINAL_TYPE(CrimsonAmbience, crimson_ambience, GTK_TYPE_WIDGET)

static GdkTexture *dark_grain, *light_grain;

GdkRGBA read_cover_accent(GdkTexture *texture);


/* A tile of faint black and white specks, laid over the colours as they are */
static GdkTexture *create_grain(GrainStrength strength) {
	gsize length = GRAIN_SIZE * GRAIN_SIZE * 4;
	guchar *pixels = g_malloc(length);
	for (gsize index = 0; index < length; index += 4) {
		double stray = g_random_double_range(-GRAIN_DEPTH, GRAIN_DEPTH);
		double alpha = stray < 0 ? -stray * strength.darken : stray * strength.lighten;
		guchar opacity = (guchar)lround(alpha * 255);
		pixels[index] = pixels[index + 1] = pixels[index + 2] = stray < 0 ? 0 : opacity;
		pixels[index + 3] = opacity;
	}
	g_autoptr(GBytes) bytes = g_bytes_new_take(pixels, length);
	return gdk_memory_texture_new(GRAIN_SIZE, GRAIN_SIZE, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED, bytes, GRAIN_SIZE * 4);
}


static GdkTexture *get_grain(gboolean is_dark) {
	GdkTexture **grain = is_dark ? &dark_grain : &light_grain;
	if (!*grain)
		*grain = create_grain(is_dark ? DARK_GRAIN : LIGHT_GRAIN);
	return *grain;
}


/* The grain stays put while the colours breathe beneath it, since grain that moves reads as flicker */
static void draw_grain(GtkSnapshot *snapshot, float width, float height, gboolean is_dark) {
	graphene_rect_t tile = GRAPHENE_RECT_INIT(0, 0, GRAIN_SIZE, GRAIN_SIZE);
	gtk_snapshot_push_repeat(snapshot, &GRAPHENE_RECT_INIT(0, 0, width, height), &tile);
	gtk_snapshot_append_texture(snapshot, get_grain(is_dark), &tile);
	gtk_snapshot_pop(snapshot);
}


/* Soft patches of the cover's colours drift over its overall colour, which stay smooth at any size where a
 * stretched and blurred cover shows smudges */
/* The colour made more vivid, for the player, whose patches show through more of the veil */
static GdkRGBA make_vivid(GdkRGBA color) {
	float hue, saturation, value;
	gtk_rgb_to_hsv(color.red, color.green, color.blue, &hue, &saturation, &value);
	gtk_hsv_to_rgb(hue, MIN(1, saturation * VIVID_SATURATION), MIN(1, value * VIVID_VALUE), &color.red, &color.green,
		&color.blue);
	return color;
}


/* The player's patches are vivid but faint, so their colour is clear while they stay in the background */
static void place_patches(const Palette *palette, float width, float height, double seconds, gboolean is_vivid,
	Patch *patches) {
	double minutes = seconds / 60;
	for (guint patch = 0; patch < PATCH_COUNT; patch++) {
		guint index = patch % PALETTE_SIZE;
		const Drift *drift = &DRIFTS[index];
		double phase = drift->phase + G_PI * (patch / PALETTE_SIZE);
		double breath = 1 + PATCH_BREATH * sin(2 * G_PI * minutes * drift->breath_speed + phase);
		patches[patch].radius = (float)((height * PATCH_HEIGHT_SCALE + width * PATCH_WIDTH_SCALE) * drift->scale
			* breath);
		/* Each patch keeps to its own share of the width, so all of them stay in view as they weave */
		double lane = (((patch * 7) % PATCH_COUNT) + 0.5) / PATCH_COUNT;
		patches[patch].x = (float)(width * (lane + 0.12 * sin(2 * G_PI * minutes * drift->horizontal_speed + phase)));
		patches[patch].y = (float)(height * (0.5 + 0.45 * cos(2 * G_PI * minutes * drift->vertical_speed + phase)));
		patches[patch].color = is_vivid ? make_vivid(palette->parts[index]) : palette->parts[index];
		if (is_vivid)
			patches[patch].color.alpha = VIVID_PATCH_ALPHA;
	}
}


/* Lays a colour of the given strength over a premultiplied one */
static GdkRGBA cover_color(GdkRGBA below, GdkRGBA above, float strength) {
	float rest = 1 - strength;
	return (GdkRGBA){below.red * rest + above.red * strength, below.green * rest + above.green * strength,
		below.blue * rest + above.blue * strength, below.alpha * rest + strength};
}


/* Each patch fades from its colour at its middle to nothing at its reach */
static GdkRGBA read_palette_color(const Palette *palette, const Patch *patches, float x, float y) {
	GdkRGBA color = palette->base;
	for (guint patch = 0; patch < PATCH_COUNT; patch++) {
		float across = x - patches[patch].x, down = y - patches[patch].y, radius = patches[patch].radius;
		float distance_squared = across * across + down * down;
		if (distance_squared >= radius * radius)
			continue;
		float reach = sqrtf(distance_squared) / radius;
		color = cover_color(color, patches[patch].color, patches[patch].color.alpha * (1 - reach));
	}
	return color;
}


/* The newest palette fades in over the one before, then the veil and the darkened edges lie over both */
static GdkRGBA read_backdrop_color(CrimsonAmbience *ambience, Patch patches[][PATCH_COUNT], float x, float y,
	float width, gboolean is_dark) {
	GdkRGBA color = {0, 0, 0, 0};
	for (guint index = 0; index < ambience->palette_count; index++) {
		gboolean is_newest = index == ambience->palette_count - 1;
		color = cover_color(color, read_palette_color(&ambience->palettes[index], patches[index], x, y),
			is_newest ? (float)ambience->mix : 1);
	}
	GdkRGBA veil = is_dark ? DARK_VEIL : LIGHT_VEIL;
	color = cover_color(color, veil, veil.alpha);
	if (!ambience->has_faded_edges)
		return color;
	GdkRGBA edge = is_dark ? DARK_EDGE : LIGHT_EDGE;
	float edge_width = MIN((float)EDGE_WIDTH, width / 2);
	float nearest = MIN(x, width - x);
	if (nearest < edge_width)
		color = cover_color(color, edge, edge.alpha * (1 - nearest / edge_width));
	return color;
}


static GdkTexture *paint_colors(CrimsonAmbience *ambience, int columns, int rows, float width, float height) {
	Patch patches[G_N_ELEMENTS(ambience->palettes)][PATCH_COUNT];
	for (guint index = 0; index < ambience->palette_count; index++)
		place_patches(&ambience->palettes[index], width, height, ambience->seconds, ambience->has_faded_edges,
			patches[index]);
	gboolean is_dark = adw_style_manager_get_dark(adw_style_manager_get_default());
	gsize stride = (gsize)columns * 4;
	guchar *pixels = g_malloc(stride * rows);
	for (int row = 0; row < rows; row++) {
		for (int column = 0; column < columns; column++) {
			GdkRGBA color = read_backdrop_color(ambience, patches, (column + 0.5f) * COLOR_CELL,
				(row + 0.5f) * COLOR_CELL, width, is_dark);
			guchar *pixel = pixels + row * stride + column * 4;
			pixel[0] = (guchar)lroundf(CLAMP(color.red, 0, 1) * 255);
			pixel[1] = (guchar)lroundf(CLAMP(color.green, 0, 1) * 255);
			pixel[2] = (guchar)lroundf(CLAMP(color.blue, 0, 1) * 255);
			pixel[3] = (guchar)lroundf(CLAMP(color.alpha, 0, 1) * 255);
		}
	}
	g_autoptr(GBytes) bytes = g_bytes_new_take(pixels, stride * rows);
	return gdk_memory_texture_new(columns, rows, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED, bytes, stride);
}


static void refresh_colors(CrimsonAmbience *ambience) {
	ambience->are_colors_stale = TRUE;
	gtk_widget_queue_draw(GTK_WIDGET(ambience));
}


/* The picture, cropped to fill the area with most of what is cut taken from its bottom, since artists' faces sit high
 * in their photos, shown through a fade from the bottom left that leaves the top right clear, and fading in as a whole
 * once it has loaded */
/* A cosine eases the drift, so it slows to a stop at each end before turning, and a quarter turn ahead it starts in
 * the middle on its way up */
static float read_picture_top(CrimsonAmbience *ambience) {
	double turn = (1 - cos(2 * G_PI * (ambience->picture_seconds / PICTURE_DRIFT_SECONDS + 0.25))) / 2;
	return (float)(PICTURE_TOP_START + (PICTURE_TOP_END - PICTURE_TOP_START) * turn);
}


static void draw_picture(CrimsonAmbience *ambience, GtkSnapshot *snapshot, float width, float height, float strength) {
	float picture_width = (float)gdk_texture_get_width(ambience->picture);
	float picture_height = (float)gdk_texture_get_height(ambience->picture);
	float scale = MAX(width / picture_width, height / picture_height);
	graphene_rect_t area = GRAPHENE_RECT_INIT(0, 0, width, height);
	float alpha = strength * (float)ambience->revealed;
	gtk_snapshot_push_mask(snapshot, GSK_MASK_MODE_ALPHA);
	GskColorStop stops[] = {
		{0, {0, 0, 0, alpha}},
		{PICTURE_SOLID, {0, 0, 0, alpha}},
		{PICTURE_CLEAR, {0, 0, 0, 0}},
		{1, {0, 0, 0, 0}},
	};
	/* Tilted only partly upward, so the fade reaches both the top right and the right edge, never ending in a cut */
	gtk_snapshot_append_linear_gradient(snapshot, &area, &GRAPHENE_POINT_INIT(0, height),
		&GRAPHENE_POINT_INIT(width, height * (1 - PICTURE_TILT)), stops, G_N_ELEMENTS(stops));
	gtk_snapshot_pop(snapshot);
	gtk_snapshot_append_texture(snapshot, ambience->picture, &GRAPHENE_RECT_INIT((width - picture_width * scale) / 2,
		(height - picture_height * scale) * read_picture_top(ambience), picture_width * scale, picture_height * scale));
	gtk_snapshot_pop(snapshot);
}


static void draw_background(CrimsonAmbience *ambience, GtkSnapshot *snapshot, float width, float height);


/* A picture lies half blended into the colours beneath it, which take its light and shade, and half as itself */
static void draw_ambience(GtkWidget *widget, GtkSnapshot *snapshot) {
	CrimsonAmbience *ambience = CRIMSON_AMBIENCE(widget);
	float width = (float)gtk_widget_get_width(widget);
	float height = (float)gtk_widget_get_height(widget);
	if (!ambience->picture || ambience->revealed <= 0) {
		draw_background(ambience, snapshot, width, height);
		gtk_widget_snapshot_child(widget, ambience->child, snapshot);
		return;
	}
	float picture_width = width * PICTURE_SHARE;
	gtk_snapshot_push_blend(snapshot, PICTURE_BLEND);
	draw_background(ambience, snapshot, width, height);
	gtk_snapshot_pop(snapshot);
	gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(0, 0, picture_width, height));
	draw_picture(ambience, snapshot, picture_width, height, 1);
	gtk_snapshot_pop(snapshot);
	gtk_snapshot_pop(snapshot);
	gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(0, 0, picture_width, height));
	draw_picture(ambience, snapshot, picture_width, height, PICTURE_PLAIN_SHARE);
	gtk_snapshot_pop(snapshot);
	gtk_widget_snapshot_child(widget, ambience->child, snapshot);
}


static void draw_background(CrimsonAmbience *ambience, GtkSnapshot *snapshot, float width, float height) {
	if (!ambience->palette_count || ambience->is_plain)
		return;
	int columns = MAX(1, (int)ceilf(width / COLOR_CELL));
	int rows = MAX(1, (int)ceilf(height / COLOR_CELL));
	gboolean is_resized = ambience->colors && (gdk_texture_get_width(ambience->colors) != columns
		|| gdk_texture_get_height(ambience->colors) != rows);
	if (!ambience->colors || ambience->are_colors_stale || is_resized) {
		g_clear_object(&ambience->colors);
		ambience->colors = paint_colors(ambience, columns, rows, width, height);
		ambience->are_colors_stale = FALSE;
	}
	gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(0, 0, width, height));
	gtk_snapshot_append_scaled_texture(snapshot, ambience->colors, GSK_SCALING_FILTER_LINEAR,
		&GRAPHENE_RECT_INIT(0, 0, columns * COLOR_CELL, rows * COLOR_CELL));
	draw_grain(snapshot, width, height, adw_style_manager_get_dark(adw_style_manager_get_default()));
	gtk_snapshot_pop(snapshot);
}


/* A timer rather than the frame clock moves the colours, so a fast screen wakes the app only when they repaint */
static gboolean move_ambience(gpointer data) {
	CrimsonAmbience *ambience = data;
	gint64 now = g_get_monotonic_time();
	gint64 elapsed = now - ambience->last_move_time;
	ambience->last_move_time = now;
	if (!ambience->palette_count && !ambience->picture)
		return G_SOURCE_CONTINUE;
	if (ambience->is_playing && ambience->palette_count) {
		ambience->seconds += (double)elapsed / G_USEC_PER_SEC;
		ambience->are_colors_stale = TRUE;
	}
	if (ambience->picture)
		ambience->picture_seconds += (double)elapsed / G_USEC_PER_SEC;
	gtk_widget_queue_draw(GTK_WIDGET(ambience));
	return G_SOURCE_CONTINUE;
}


static void measure_ambience(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
	int *natural, int *minimum_baseline, int *natural_baseline) {
	gtk_widget_measure(CRIMSON_AMBIENCE(widget)->child, orientation, for_size, minimum, natural, minimum_baseline,
		natural_baseline);
}


static void allocate_ambience(GtkWidget *widget, int width, int height, int baseline) {
	gtk_widget_allocate(CRIMSON_AMBIENCE(widget)->child, width, height, baseline, NULL);
}


static void dispose_ambience(GObject *object) {
	CrimsonAmbience *ambience = CRIMSON_AMBIENCE(object);
	g_clear_handle_id(&ambience->timer, g_source_remove);
	g_clear_pointer(&ambience->child, gtk_widget_unparent);
	g_clear_object(&ambience->current);
	g_clear_object(&ambience->crossfade);
	g_clear_object(&ambience->reveal);
	g_clear_object(&ambience->picture);
	g_clear_object(&ambience->colors);
	if (ambience->accent)
		gtk_style_context_remove_provider_for_display(gdk_display_get_default(),
			GTK_STYLE_PROVIDER(ambience->accent));
	g_clear_object(&ambience->accent);
	g_clear_pointer(&ambience->style_class, g_free);
	g_clear_pointer(&ambience->accent_class, g_free);
	G_OBJECT_CLASS(crimson_ambience_parent_class)->dispose(object);
}


/* Animations off in the system settings leave the backdrop still, while the crossfade still eases, and the colours
 * also rest while the window is in the background, since nobody watches them there */
static void update_motion(CrimsonAmbience *ambience) {
	gboolean is_animating = TRUE;
	g_object_get(gtk_settings_get_default(), "gtk-enable-animations", &is_animating, NULL);
	gboolean is_moving = (ambience->is_playing || ambience->picture) && is_animating && ambience->window
		&& gtk_window_is_active(ambience->window);
	if (is_moving == (ambience->timer != 0))
		return;
	if (is_moving) {
		ambience->last_move_time = g_get_monotonic_time();
		ambience->timer = g_timeout_add(FRAME_MILLISECONDS, move_ambience, ambience);
	} else {
		g_clear_handle_id(&ambience->timer, g_source_remove);
	}
}


static void root_ambience(GtkWidget *widget) {
	CrimsonAmbience *ambience = CRIMSON_AMBIENCE(widget);
	GTK_WIDGET_CLASS(crimson_ambience_parent_class)->root(widget);
	GtkRoot *root = gtk_widget_get_root(widget);
	ambience->window = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
	if (ambience->window)
		g_signal_connect_object(ambience->window, "notify::is-active", G_CALLBACK(update_motion), ambience,
			G_CONNECT_SWAPPED);
	update_motion(ambience);
}


static void unroot_ambience(GtkWidget *widget) {
	CrimsonAmbience *ambience = CRIMSON_AMBIENCE(widget);
	if (ambience->window)
		g_signal_handlers_disconnect_by_data(ambience->window, ambience);
	ambience->window = NULL;
	update_motion(ambience);
	GTK_WIDGET_CLASS(crimson_ambience_parent_class)->unroot(widget);
}


static void crimson_ambience_class_init(CrimsonAmbienceClass *type) {
	GTK_WIDGET_CLASS(type)->root = root_ambience;
	GTK_WIDGET_CLASS(type)->unroot = unroot_ambience;
	G_OBJECT_CLASS(type)->dispose = dispose_ambience;
	GTK_WIDGET_CLASS(type)->measure = measure_ambience;
	GTK_WIDGET_CLASS(type)->size_allocate = allocate_ambience;
	GTK_WIDGET_CLASS(type)->snapshot = draw_ambience;
	gtk_widget_class_set_css_name(GTK_WIDGET_CLASS(type), "ambience");
}


static void crimson_ambience_init(CrimsonAmbience *ambience) {
}


static void set_revealed(double value, gpointer data) {
	CrimsonAmbience *ambience = data;
	ambience->revealed = value;
	gtk_widget_queue_draw(GTK_WIDGET(ambience));
}


void set_ambience_picture(GtkWidget *widget, GdkTexture *texture) {
	CrimsonAmbience *ambience = CRIMSON_AMBIENCE(widget);
	if (ambience->picture == texture)
		return;
	g_set_object(&ambience->picture, texture);
	ambience->revealed = 0;
	ambience->picture_seconds = 0;
	update_motion(ambience);
	if (!ambience->reveal) {
		AdwAnimationTarget *target = adw_callback_animation_target_new(set_revealed, ambience, NULL);
		ambience->reveal = adw_timed_animation_new(widget, 0, 1, REVEAL_MILLISECONDS, target);
		adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(ambience->reveal), ADW_EASE_OUT_CUBIC);
	}
	adw_animation_reset(ambience->reveal);
	if (texture)
		adw_animation_play(ambience->reveal);
	gtk_widget_queue_draw(widget);
}


static void set_mix(double value, gpointer data) {
	CrimsonAmbience *ambience = data;
	ambience->mix = value;
	refresh_colors(ambience);
}


/* The new colours now cover the old ones completely, so the old ones stop being drawn */
static void finish_crossfade(CrimsonAmbience *ambience) {
	if (ambience->palette_count < G_N_ELEMENTS(ambience->palettes))
		return;
	ambience->palettes[0] = ambience->palettes[1];
	ambience->palette_count = 1;
	refresh_colors(ambience);
}


GtkWidget *create_ambience(GtkWidget *child) {
	CrimsonAmbience *ambience = g_object_new(CRIMSON_TYPE_AMBIENCE, NULL);
	static guint count;
	ambience->child = child;
	ambience->style_class = g_strdup_printf("ambience-%u", ++count);
	gtk_widget_add_css_class(GTK_WIDGET(ambience), ambience->style_class);
	gtk_widget_set_parent(child, GTK_WIDGET(ambience));
	AdwAnimationTarget *target = adw_callback_animation_target_new(set_mix, ambience, NULL);
	ambience->crossfade = adw_timed_animation_new(GTK_WIDGET(ambience), 0, 1, CROSSFADE_MILLISECONDS, target);
	adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(ambience->crossfade), ADW_EASE_IN_OUT_CUBIC);
	g_signal_connect_swapped(ambience->crossfade, "done", G_CALLBACK(finish_crossfade), ambience);
	g_signal_connect_object(adw_style_manager_get_default(), "notify::dark", G_CALLBACK(refresh_colors), ambience,
		G_CONNECT_SWAPPED);
	return GTK_WIDGET(ambience);
}


void set_ambience_moving(GtkWidget *widget, gboolean is_moving) {
	CRIMSON_AMBIENCE(widget)->is_playing = is_moving;
	update_motion(CRIMSON_AMBIENCE(widget));
}


static guchar *read_pixels(GdkTexture *texture, int *width, int *height) {
	*width = gdk_texture_get_width(texture);
	*height = gdk_texture_get_height(texture);
	guchar *pixels = g_malloc((gsize)*width * (gsize)*height * 4);
	GdkTextureDownloader *downloader = gdk_texture_downloader_new(texture);
	gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);
	gdk_texture_downloader_download_into(downloader, pixels, (gsize)*width * 4);
	gdk_texture_downloader_free(downloader);
	return pixels;
}


/* Averages one part of the cover, given as fractions of its sides, weighting each pixel by its saturation so
 * the colourful parts count most */
static GdkRGBA average_area(const guchar *pixels, int width, int height, double left, double top, double right,
	double bottom) {
	double red = 0, green = 0, blue = 0, total = 0;
	for (int row = 0; row < SAMPLE_SIZE; row++) {
		for (int column = 0; column < SAMPLE_SIZE; column++) {
			int x = (int)((left + (right - left) * column / SAMPLE_SIZE) * width);
			int y = (int)((top + (bottom - top) * row / SAMPLE_SIZE) * height);
			const guchar *pixel = pixels + ((gsize)y * width + x) * 4;
			double highest = MAX(pixel[0], MAX(pixel[1], pixel[2])) / 255.0;
			double lowest = MIN(pixel[0], MIN(pixel[1], pixel[2])) / 255.0;
			double weight = highest - lowest + 0.02;
			red += pixel[0] / 255.0 * weight;
			green += pixel[1] / 255.0 * weight;
			blue += pixel[2] / 255.0 * weight;
			total += weight;
		}
	}
	return (GdkRGBA){(float)(red / total), (float)(green / total), (float)(blue / total), 1};
}


static double to_linear(double channel) {
	return channel <= 0.04045 ? channel / 12.92 : pow((channel + 0.055) / 1.055, 2.4);
}


/* Chroma and hue in degrees in the Oklab colour space, where they follow what the eye sees, with chroma scaled to the
 * range Material's own colour space uses so its constants apply */
static void read_chroma_hue(double red, double green, double blue, double *chroma, double *hue) {
	red = to_linear(red);
	green = to_linear(green);
	blue = to_linear(blue);
	double long_cone = cbrt(0.4122214708 * red + 0.5363325363 * green + 0.0514459929 * blue);
	double middle_cone = cbrt(0.2119034982 * red + 0.6806995451 * green + 0.1073969566 * blue);
	double short_cone = cbrt(0.0883024619 * red + 0.2817188376 * green + 0.6299787005 * blue);
	double a = 1.9779984951 * long_cone - 2.4285922050 * middle_cone + 0.4505937099 * short_cone;
	double b = 0.0259040371 * long_cone + 0.7827717662 * middle_cone - 0.8086757660 * short_cone;
	*chroma = hypot(a, b) * CHROMA_SCALE;
	*hue = fmod(atan2(b, a) * 180 / G_PI + 360, 360);
}


static double hue_distance(double one, double other) {
	double difference = fabs(one - other);
	return MIN(difference, 360 - difference);
}


typedef struct {
	GdkRGBA color;
	double hue, score;
} ScoredColor;


static int compare_scores(gconstpointer one, gconstpointer other) {
	double first = ((const ScoredColor *)one)->score, second = ((const ScoredColor *)other)->score;
	return first < second ? 1 : first > second ? -1 : 0;
}


/* The cover's dominant colour and then its accents after Material's colour scoring (material-color-utilities, score.ts): colours are
 * counted, each hue's share is spread over its neighbours, greyish and rare hues are dropped, the rest are scored by
 * their hue's share and how vivid they are, and the best are taken as far apart in hue as they can be, so a blue
 * cover with yellow lettering gives yellow too */
static guint pick_distinct_colors(const guchar *pixels, int width, int height, GdkRGBA *picked, guint wanted) {
	g_autofree guint *population = g_new0(guint, QUANTIZED_COLORS);
	int stride = MAX(1, (int)sqrt((double)width * height / DISTINCT_PIXELS));
	double total = 0;
	for (int row = 0; row < height; row += stride) {
		for (int column = 0; column < width; column += stride) {
			const guchar *pixel = pixels + ((gsize)row * width + column) * 4;
			population[(pixel[0] >> 4) << 8 | (pixel[1] >> 4) << 4 | pixel[2] >> 4]++;
			total++;
		}
	}
	double hue_share[360] = {0}, excited[360] = {0};
	g_autofree double *chromas = g_new0(double, QUANTIZED_COLORS);
	g_autofree double *hues = g_new0(double, QUANTIZED_COLORS);
	for (guint key = 0; key < QUANTIZED_COLORS; key++) {
		if (!population[key])
			continue;
		read_chroma_hue((key >> 8) * 17 / 255.0, (key >> 4 & 15) * 17 / 255.0, (key & 15) * 17 / 255.0, &chromas[key],
			&hues[key]);
		hue_share[(int)hues[key] % 360] += population[key] / total;
	}
	for (int hue = 0; hue < 360; hue++) {
		for (int near = hue - 14; near < hue + 16; near++)
			excited[(near + 360) % 360] += hue_share[hue];
	}
	g_autoptr(GArray) scored = g_array_new(FALSE, FALSE, sizeof(ScoredColor));
	for (guint key = 0; key < QUANTIZED_COLORS; key++) {
		double share = excited[(int)round(hues[key]) % 360];
		if (!population[key] || chromas[key] < CUTOFF_CHROMA || share <= CUTOFF_SHARE)
			continue;
		double chroma_weight = chromas[key] < TARGET_CHROMA ? WEIGHT_CHROMA_BELOW : WEIGHT_CHROMA_ABOVE;
		ScoredColor color = {{(key >> 8) * 17 / 255.0f, (key >> 4 & 15) * 17 / 255.0f, (key & 15) * 17 / 255.0f, 1},
			hues[key], share * 100 * WEIGHT_SHARE + (chromas[key] - TARGET_CHROMA) * chroma_weight};
		g_array_append_val(scored, color);
	}
	g_array_sort(scored, compare_scores);
	/* The dominant colour comes first: the one covering most of the cover, greys aside. The rest are the best scored
	 * colours as far from it and each other in hue as they can be */
	guint dominant = QUANTIZED_COLORS;
	for (guint key = 0; key < QUANTIZED_COLORS; key++) {
		if (population[key] && chromas[key] >= CUTOFF_CHROMA
			&& (dominant == QUANTIZED_COLORS || population[key] > population[dominant]))
			dominant = key;
	}
	if (dominant == QUANTIZED_COLORS || !wanted)
		return 0;
	GdkRGBA first = {(dominant >> 8) * 17 / 255.0f, (dominant >> 4 & 15) * 17 / 255.0f, (dominant & 15) * 17 / 255.0f, 1};
	guint count = 1;
	for (int spacing = 90; spacing >= 15 && count < wanted; spacing--) {
		count = 1;
		picked[0] = first;
		for (guint index = 0; index < scored->len && count < wanted; index++) {
			const ScoredColor *color = &g_array_index(scored, ScoredColor, index);
			gboolean is_near = FALSE;
			for (guint chosen = 0; chosen < count && !is_near; chosen++) {
				double chosen_chroma, chosen_hue;
				read_chroma_hue(picked[chosen].red, picked[chosen].green, picked[chosen].blue, &chosen_chroma,
					&chosen_hue);
				is_near = hue_distance(color->hue, chosen_hue) < spacing;
			}
			if (!is_near)
				picked[count++] = color->color;
		}
	}
	return count;
}


/* The colour with its hue turned by a share of the circle, and its brightness and saturation moved, kept within the
 * backdrop's range */
static GdkRGBA shade_color(GdkRGBA color, double turn, double lighten, double saturate) {
	float hue, saturation, value;
	gtk_rgb_to_hsv(color.red, color.green, color.blue, &hue, &saturation, &value);
	hue = (float)fmod(hue + turn + 1, 1);
	value = (float)CLAMP(value + lighten, BLOB_VALUE_MINIMUM, BLOB_VALUE_MAXIMUM);
	saturation = (float)CLAMP(saturation + saturate, BLOB_SATURATION_MINIMUM, 1);
	GdkRGBA shaded = {0, 0, 0, 1};
	gtk_hsv_to_rgb(hue, saturation, value, &shaded.red, &shaded.green, &shaded.blue);
	return shaded;
}


static GdkRGBA adjust_color(GdkRGBA color, double saturation_minimum, double value_minimum, double value_maximum) {
	float hue, saturation, value;
	gtk_rgb_to_hsv(color.red, color.green, color.blue, &hue, &saturation, &value);
	saturation = MAX(saturation, (float)saturation_minimum);
	value = CLAMP(value, (float)value_minimum, (float)value_maximum);
	GdkRGBA adjusted = {0, 0, 0, 1};
	gtk_hsv_to_rgb(hue, saturation, value, &adjusted.red, &adjusted.green, &adjusted.blue);
	return adjusted;
}


static void apply_accent(CrimsonAmbience *ambience, GdkRGBA accent) {
	if (!ambience->accent) {
		ambience->accent = gtk_css_provider_new();
		gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(ambience->accent),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}
	if (ambience->is_plain) {
		gtk_css_provider_load_from_string(ambience->accent, "");
		return;
	}
	/* GTK restyles the buttons inside only when a class of the backdrop changes, not for a new colour alone, so each
	 * accent comes with a class of its own */
	GtkWidget *widget = GTK_WIDGET(ambience);
	if (ambience->accent_class)
		gtk_widget_remove_css_class(widget, ambience->accent_class);
	g_free(ambience->accent_class);
	ambience->accent_class = g_strdup_printf("%s-%u", ambience->style_class, ++ambience->accent_count);
	g_autofree char *color = gdk_rgba_to_string(&accent);
	g_autofree char *css = g_strdup_printf("ambience.%s { --accent-bg-color: %s; --accent-color: %s; }",
		ambience->accent_class, color, color);
	gtk_css_provider_load_from_string(ambience->accent, css);
	gtk_widget_add_css_class(widget, ambience->accent_class);
}


void set_ambience_plain(GtkWidget *widget, gboolean is_plain) {
	CrimsonAmbience *ambience = CRIMSON_AMBIENCE(widget);
	ambience->is_plain = is_plain;
	if (is_plain)
		gtk_widget_add_css_class(widget, "plain");
	else
		gtk_widget_remove_css_class(widget, "plain");
	if (ambience->current)
		apply_accent(ambience, read_cover_accent(ambience->current));
	else if (ambience->accent)
		gtk_css_provider_load_from_string(ambience->accent, "");
	gtk_widget_queue_draw(widget);
}


/* Reads the cover's overall colour, one colour per part and the accent, then fades the new colours in over the
 * old ones */
void set_ambience_texture(GtkWidget *widget, GdkTexture *texture) {
	CrimsonAmbience *ambience = CRIMSON_AMBIENCE(widget);
	if (!texture || texture == ambience->current)
		return;
	g_set_object(&ambience->current, texture);
	int width, height;
	g_autofree guchar *pixels = read_pixels(texture, &width, &height);
	GdkRGBA whole = average_area(pixels, width, height, 0, 0, 1, 1);
	apply_accent(ambience, read_cover_accent(texture));
	if (ambience->palette_count == G_N_ELEMENTS(ambience->palettes))
		ambience->palettes[0] = ambience->palettes[1];
	else
		ambience->palette_count++;
	Palette *palette = &ambience->palettes[ambience->palette_count - 1];
	palette->base = adjust_color(whole, BLOB_SATURATION_MINIMUM, BLOB_VALUE_MINIMUM, BLOB_VALUE_MAXIMUM);
	for (guint index = 0; index < PALETTE_SIZE; index++) {
		double left = (double)(index % PALETTE_COLUMNS) / PALETTE_COLUMNS;
		double top = (double)(index / PALETTE_COLUMNS) / PALETTE_ROWS;
		GdkRGBA part = average_area(pixels, width, height, left, top, left + 1.0 / PALETTE_COLUMNS,
			top + 1.0 / PALETTE_ROWS);
		palette->parts[index] = adjust_color(part, BLOB_SATURATION_MINIMUM, BLOB_VALUE_MINIMUM, BLOB_VALUE_MAXIMUM);
	}
	/* The cover's dominant colour and its accent make the palette, the dominant one in the middle of the bars and the
	 * accent at their ends with two shades between, so the colours stay calm. A cover with one colour turns it for the
	 * accent, and a cover with none keeps the colours of its parts */
	GdkRGBA distinct[DISTINCT_COLORS];
	guint distinct_count = pick_distinct_colors(pixels, width, height, distinct, DISTINCT_COLORS);
	if (distinct_count == 1)
		distinct[distinct_count++] = shade_color(distinct[0], HUE_TURN, 0, 0);
	if (distinct_count == DISTINCT_COLORS) {
		GdkRGBA first = adjust_color(distinct[0], BLOB_SATURATION_MINIMUM, BLOB_VALUE_MINIMUM, BLOB_VALUE_MAXIMUM);
		GdkRGBA second = adjust_color(distinct[1], BLOB_SATURATION_MINIMUM, BLOB_VALUE_MINIMUM, BLOB_VALUE_MAXIMUM);
		GdkRGBA shades[] = {shade_color(first, SHADE_TURN, SHADE_LIGHTEN, 0),
			shade_color(second, -SHADE_TURN, -SHADE_LIGHTEN, 0)};
		GdkRGBA parts[PALETTE_SIZE] = {first, first, shades[0], shades[1], second, second};
		memcpy(palette->parts, parts, sizeof parts);
	}
	ambience->mix = 0;
	adw_animation_reset(ambience->crossfade);
	adw_animation_play(ambience->crossfade);
}


void fade_ambience_edges(GtkWidget *widget) {
	CRIMSON_AMBIENCE(widget)->has_faded_edges = TRUE;
	refresh_colors(CRIMSON_AMBIENCE(widget));
}


GdkRGBA read_cover_accent(GdkTexture *texture) {
	int width, height;
	g_autofree guchar *pixels = read_pixels(texture, &width, &height);
	GdkRGBA whole = average_area(pixels, width, height, 0, 0, 1, 1);
	return adjust_color(whole, ACCENT_SATURATION_MINIMUM, ACCENT_VALUE_MINIMUM, ACCENT_VALUE_MAXIMUM);
}

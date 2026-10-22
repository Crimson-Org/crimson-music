#include "../lang/en.h"
#include "artwork.h"
#include "dialogs.h"
#include "dynamic-editor.h"
#include "info-dialog.h"
#include "song-info.h"

/* Without a search only the most common values of a facet show as chips, along with any already chosen */
#define CHIPS_SHOWN 30
#define PREVIEW_SONGS 60
#define PREVIEW_ARTWORK_SIZE 40
#define PREVIEW_WIDTH 320
/* The preview waits this long after a change, so a quick run of clicks fills it once */
#define PREVIEW_DELAY_MS 120
/* Below this width the preview moves under the filters */
#define NARROW_WIDTH "max-width: 720sp"
/* A dialog with breakpoints has no minimum size of its own, so it takes the smallest size GNOME phones show */
#define MINIMUM_WIDTH 360
#define MINIMUM_HEIGHT 294

typedef enum {
	CHIP_NEUTRAL,
	CHIP_INCLUDED,
	CHIP_EXCLUDED,
} ChipState;

static const char *const FACET_TITLES[FACET_COUNT] = {TEXT_GENRES, TEXT_MOOD, TEXT_ARTISTS, TEXT_YOUR_TAGS};
static const char *const FACET_DETAILS[FACET_COUNT] = {
	TEXT_GENRES_DETAIL, TEXT_MOODS_DETAIL, NULL, TEXT_TAGS_DETAIL,
};
/* The limits come first and the artists last, since the artists grow with every liked song and would push the
 * rest out of sight */
static const Facet FACET_ORDER[] = {FACET_MOOD, FACET_GENRE, FACET_TAG, FACET_ARTIST};
static const char *const MOOD_KEYS[] = {
	"danceable", "energetic", "calm", "happy", "sad", "acoustic", "instrumental",
};
static const char *const MOOD_TITLES[] = {
	TEXT_DANCEABLE, TEXT_ENERGETIC, TEXT_CALM, TEXT_HAPPY, TEXT_SAD, TEXT_ACOUSTIC, TEXT_INSTRUMENTAL,
};

static const struct {
	const char *title, *detail, *unit;
	double lowest, highest, from, to, step;
} RANGES[RANGE_COUNT] = {
	{TEXT_TEMPO, TEXT_TEMPO_DETAIL, TEXT_BPM, 40, 240, 90, 130, 1},
	{TEXT_YEAR, TEXT_YEAR_DETAIL, NULL, 1900, 2100, 2000, 2030, 1},
	{TEXT_LENGTH, TEXT_LENGTH_DETAIL, TEXT_MINUTES, 0, 600, 2, 8, 1},
};

typedef struct {
	DynamicRules *rules;
	GListModel *pool;
	DynamicSaved saved;
	gpointer data;
	AdwDialog *dialog;
	GtkWidget *name, *save, *chip_boxes[FACET_COUNT], *searches[FACET_COUNT];
	GtkWidget *count, *note, *preview;
	guint preview_source;
	/* The name was made from the filters, so it follows them until the listener types one */
	gboolean is_name_automatic, is_naming;
} Editor;

static Editor *open_editor;


/* Chips ==================================================================== */


/* Words of a genre start with capitals, so "synth-pop" shows as "Synth-Pop" */
static char *title_case(const char *text) {
	g_autofree char *lower = g_utf8_strdown(text, -1);
	GString *result = g_string_new(NULL);
	gboolean is_start = TRUE;
	for (const char *letter = lower; *letter; letter = g_utf8_next_char(letter)) {
		gunichar character = g_utf8_get_char(letter);
		g_string_append_unichar(result, is_start ? g_unichar_totitle(character) : character);
		is_start = !g_unichar_isalnum(character);
	}
	return g_string_free(result, FALSE);
}


static char *show_value(Facet facet, const char *value, CrimsonSong *song) {
	if (facet == FACET_MOOD) {
		for (guint index = 0; index < G_N_ELEMENTS(MOOD_KEYS); index++) {
			if (g_str_equal(value, MOOD_KEYS[index]))
				return g_strdup(MOOD_TITLES[index]);
		}
	}
	if (facet == FACET_ARTIST && song) {
		g_auto(GStrv) artists = g_strsplit(song->artist, ",", -1);
		for (char **artist = artists; *artist; artist++) {
			g_autofree char *lower = g_utf8_strdown(g_strstrip(*artist), -1);
			if (g_str_equal(lower, value))
				return g_strdup(*artist);
		}
	}
	if (facet == FACET_GENRE)
		return title_case(value);
	return g_strdup(value);
}


static ChipState get_chip_state(Editor *editor, const char *key) {
	if (g_hash_table_contains(editor->rules->included, key))
		return CHIP_INCLUDED;
	return g_hash_table_contains(editor->rules->excluded, key) ? CHIP_EXCLUDED : CHIP_NEUTRAL;
}


static void show_chip_state(GtkWidget *chip, ChipState state) {
	gtk_widget_remove_css_class(chip, "included");
	gtk_widget_remove_css_class(chip, "excluded");
	if (state == CHIP_INCLUDED)
		gtk_widget_add_css_class(chip, "included");
	else if (state == CHIP_EXCLUDED)
		gtk_widget_add_css_class(chip, "excluded");
	const char *icon = state == CHIP_INCLUDED ? "object-select-symbolic" : state == CHIP_EXCLUDED
		? "action-unavailable-symbolic" : NULL;
	GtkWidget *image = g_object_get_data(G_OBJECT(chip), "icon");
	gtk_image_set_from_icon_name(GTK_IMAGE(image), icon);
	gtk_widget_set_visible(image, icon != NULL);
	gtk_widget_set_tooltip_text(chip, state == CHIP_INCLUDED ? TEXT_CHIP_INCLUDED : state == CHIP_EXCLUDED
		? TEXT_CHIP_EXCLUDED : TEXT_CHIP_NEUTRAL);
}


static void schedule_preview(Editor *editor);
static void name_from_filters(Editor *editor);


/* Each click moves a chip on from neutral to included to left out and back */
static void click_chip(GtkButton *button, Editor *editor) {
	const char *key = g_object_get_data(G_OBJECT(button), "key");
	ChipState state = (get_chip_state(editor, key) + 1) % 3;
	g_hash_table_remove(editor->rules->included, key);
	g_hash_table_remove(editor->rules->excluded, key);
	if (state == CHIP_INCLUDED)
		g_hash_table_add(editor->rules->included, g_strdup(key));
	else if (state == CHIP_EXCLUDED)
		g_hash_table_add(editor->rules->excluded, g_strdup(key));
	show_chip_state(GTK_WIDGET(button), state);
	name_from_filters(editor);
	schedule_preview(editor);
}


static GtkWidget *create_chip(Editor *editor, Facet facet, const char *value, const char *title, guint count,
	guint rank) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *icon = gtk_image_new();
	gtk_box_append(GTK_BOX(box), icon);
	gtk_box_append(GTK_BOX(box), gtk_label_new(title));
	g_autofree char *number = g_strdup_printf("%u", count);
	GtkWidget *count_label = gtk_label_new(number);
	gtk_widget_add_css_class(count_label, "chip-count");
	gtk_box_append(GTK_BOX(box), count_label);
	GtkWidget *chip = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(chip), box);
	gtk_widget_add_css_class(chip, "filter-chip");
	g_object_set_data(G_OBJECT(chip), "icon", icon);
	g_object_set_data_full(G_OBJECT(chip), "key", make_facet_key(facet, value), g_free);
	g_object_set_data_full(G_OBJECT(chip), "title", g_utf8_strdown(title, -1), g_free);
	g_object_set_data(G_OBJECT(chip), "rank", GUINT_TO_POINTER(rank));
	show_chip_state(chip, get_chip_state(editor, g_object_get_data(G_OBJECT(chip), "key")));
	g_signal_connect(chip, "clicked", G_CALLBACK(click_chip), editor);
	return chip;
}


/* A search shows every chip it finds, and otherwise the most common ones and any already chosen show */
static void filter_chips(GtkWidget *chips, GtkWidget *search) {
	const char *text = search ? gtk_editable_get_text(GTK_EDITABLE(search)) : "";
	g_autofree char *lower = g_utf8_strdown(text, -1);
	for (GtkWidget *chip = gtk_widget_get_first_child(chips); chip; chip = gtk_widget_get_next_sibling(chip)) {
		gboolean is_chosen = gtk_widget_has_css_class(chip, "included") || gtk_widget_has_css_class(chip, "excluded");
		gboolean is_common = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(chip), "rank")) < CHIPS_SHOWN;
		gboolean is_found = strstr(g_object_get_data(G_OBJECT(chip), "title"), lower) != NULL;
		gtk_widget_set_visible(chip, *lower ? is_found : is_chosen || is_common);
	}
}


static void search_chips(GtkEditable *search, GtkWidget *chips) {
	filter_chips(chips, GTK_WIDGET(search));
}


typedef struct {
	char *value, *title;
	guint count;
} ChipValue;


static void free_chip_value(gpointer data) {
	ChipValue *value = data;
	g_free(value->value);
	g_free(value->title);
	g_free(value);
}


static int compare_chip_values(gconstpointer one, gconstpointer other) {
	const ChipValue *a = *(ChipValue *const *)one, *b = *(ChipValue *const *)other;
	if (a->count != b->count)
		return a->count > b->count ? -1 : 1;
	return g_utf8_collate(a->title, b->title);
}


/* Counts every value of the facet over the pool, so the chips show the most common first with their counts, and
 * values the rules hold but no song has any more stay choosable */
static GPtrArray *count_values(Editor *editor, Facet facet) {
	g_autoptr(GHashTable) found = g_hash_table_new(g_str_hash, g_str_equal);
	GPtrArray *values = g_ptr_array_new_with_free_func(free_chip_value);
	guint songs = g_list_model_get_n_items(editor->pool);
	for (guint index = 0; index < songs; index++) {
		g_autoptr(CrimsonSong) song = g_list_model_get_item(editor->pool, index);
		g_autoptr(GPtrArray) song_values = get_song_facet_values(song, facet);
		for (guint each = 0; each < song_values->len; each++) {
			const char *value = g_ptr_array_index(song_values, each);
			ChipValue *chip = g_hash_table_lookup(found, value);
			if (!chip) {
				chip = g_new0(ChipValue, 1);
				chip->value = g_strdup(value);
				chip->title = show_value(facet, value, song);
				g_hash_table_insert(found, chip->value, chip);
				g_ptr_array_add(values, chip);
			}
			chip->count++;
		}
	}
	GHashTable *chosen[] = {editor->rules->included, editor->rules->excluded};
	g_autofree char *prefix = make_facet_key(facet, "");
	for (guint table = 0; table < G_N_ELEMENTS(chosen); table++) {
		GHashTableIter iter;
		gpointer key;
		g_hash_table_iter_init(&iter, chosen[table]);
		while (g_hash_table_iter_next(&iter, &key, NULL)) {
			const char *value = (const char *)key + strlen(prefix);
			if (!g_str_has_prefix(key, prefix) || g_hash_table_contains(found, value))
				continue;
			ChipValue *chip = g_new0(ChipValue, 1);
			chip->value = g_strdup(value);
			chip->title = show_value(facet, value, NULL);
			g_hash_table_insert(found, chip->value, chip);
			g_ptr_array_add(values, chip);
		}
	}
	g_ptr_array_sort(values, compare_chip_values);
	return values;
}


static void fill_chips(Editor *editor, Facet facet) {
	GtkWidget *chips = editor->chip_boxes[facet];
	adw_wrap_box_remove_all(ADW_WRAP_BOX(chips));
	g_autoptr(GPtrArray) values = count_values(editor, facet);
	for (guint index = 0; index < values->len; index++) {
		ChipValue *value = g_ptr_array_index(values, index);
		adw_wrap_box_append(ADW_WRAP_BOX(chips), create_chip(editor, facet, value->value, value->title, value->count,
			index));
	}
	filter_chips(chips, editor->searches[facet]);
	GtkWidget *group = gtk_widget_get_ancestor(chips, ADW_TYPE_PREFERENCES_GROUP);
	gtk_widget_set_visible(group, values->len > 0);
}


static GtkWidget *create_facet(Editor *editor, Facet facet) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), FACET_TITLES[facet]);
	if (FACET_DETAILS[facet])
		adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), FACET_DETAILS[facet]);
	GtkWidget *chips = adw_wrap_box_new();
	editor->chip_boxes[facet] = chips;
	adw_wrap_box_set_child_spacing(ADW_WRAP_BOX(chips), 6);
	adw_wrap_box_set_line_spacing(ADW_WRAP_BOX(chips), 6);
	gtk_widget_add_css_class(chips, "filter-chips");
	if (facet == FACET_GENRE || facet == FACET_ARTIST) {
		GtkWidget *search = gtk_search_entry_new();
		gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), facet == FACET_GENRE ? TEXT_FIND_GENRE
			: TEXT_FIND_ARTIST);
		gtk_widget_set_valign(search, GTK_ALIGN_CENTER);
		g_signal_connect(search, "search-changed", G_CALLBACK(search_chips), chips);
		adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group), search);
		editor->searches[facet] = search;
	}
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), chips);
	return group;
}


/* Ranges =================================================================== */


static void change_range(GObject *object, GParamSpec *spec, Editor *editor) {
	RangeKind kind = GPOINTER_TO_INT(g_object_get_data(object, "kind"));
	GtkWidget *row = g_object_get_data(object, "row");
	Range *range = &editor->rules->ranges[kind];
	range->is_on = adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(row));
	range->minimum = adw_spin_row_get_value(ADW_SPIN_ROW(g_object_get_data(G_OBJECT(row), "from")));
	range->maximum = adw_spin_row_get_value(ADW_SPIN_ROW(g_object_get_data(G_OBJECT(row), "to")));
	name_from_filters(editor);
	schedule_preview(editor);
}


static GtkWidget *create_spin(GtkWidget *row, RangeKind kind, const char *title, double value) {
	GtkWidget *spin = adw_spin_row_new_with_range(RANGES[kind].lowest, RANGES[kind].highest, RANGES[kind].step);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(spin), title);
	if (RANGES[kind].unit)
		adw_action_row_set_subtitle(ADW_ACTION_ROW(spin), RANGES[kind].unit);
	adw_spin_row_set_value(ADW_SPIN_ROW(spin), value);
	g_object_set_data(G_OBJECT(spin), "kind", GINT_TO_POINTER(kind));
	g_object_set_data(G_OBJECT(spin), "row", row);
	adw_expander_row_add_row(ADW_EXPANDER_ROW(row), spin);
	return spin;
}


static GtkWidget *create_ranges(Editor *editor) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), TEXT_LIMITS);
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), TEXT_LIMITS_DETAIL);
	for (int kind = 0; kind < RANGE_COUNT; kind++) {
		Range *range = &editor->rules->ranges[kind];
		gboolean is_set = range->is_on || range->maximum > 0;
		GtkWidget *row = adw_expander_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), RANGES[kind].title);
		adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row), RANGES[kind].detail);
		adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(row), TRUE);
		GtkWidget *from = create_spin(row, kind, TEXT_FROM, is_set ? range->minimum : RANGES[kind].from);
		GtkWidget *to = create_spin(row, kind, TEXT_TO, is_set ? range->maximum : RANGES[kind].to);
		g_object_set_data(G_OBJECT(row), "from", from);
		g_object_set_data(G_OBJECT(row), "to", to);
		g_object_set_data(G_OBJECT(row), "kind", GINT_TO_POINTER(kind));
		g_object_set_data(G_OBJECT(row), "row", row);
		adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(row), range->is_on);
		adw_expander_row_set_expanded(ADW_EXPANDER_ROW(row), range->is_on);
		g_signal_connect(row, "notify::enable-expansion", G_CALLBACK(change_range), editor);
		g_signal_connect(from, "notify::value", G_CALLBACK(change_range), editor);
		g_signal_connect(to, "notify::value", G_CALLBACK(change_range), editor);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
	}
	return group;
}


/* Preview ================================================================== */


static GtkWidget *create_preview_row(CrimsonSong *song) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_add_css_class(box, "preview-song");
	gtk_widget_set_tooltip_text(box, TEXT_SHOW_INFORMATION);
	g_object_set_data_full(G_OBJECT(box), "song", g_object_ref(song), g_object_unref);
	GtkWidget *cover = create_cover(PREVIEW_ARTWORK_SIZE);
	show_artwork(cover, song->artwork);
	gtk_box_append(GTK_BOX(box), cover);
	GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(labels, TRUE);
	GtkWidget *title = gtk_label_new(song->title);
	GtkWidget *artist = gtk_label_new(song->artist);
	gtk_widget_add_css_class(artist, "dimmed");
	gtk_widget_add_css_class(artist, "caption");
	for (GtkWidget *label = title; label; label = label == title ? artist : NULL) {
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_box_append(GTK_BOX(labels), label);
	}
	gtk_box_append(GTK_BOX(box), labels);
	return box;
}


static void fill_preview(Editor *editor) {
	g_autoptr(GPtrArray) songs = filter_dynamic_songs(editor->pool, editor->rules);
	guint total = g_list_model_get_n_items(editor->pool);
	g_autofree char *count = g_strdup_printf(g_dngettext(NULL, TEXT_SONG_COUNT_ONE, TEXT_SONG_COUNT, songs->len),
		songs->len);
	gtk_label_set_text(GTK_LABEL(editor->count), count);
	guint waiting = count_waiting_song_info();
	g_autofree char *note = waiting ? g_strdup_printf(TEXT_PREVIEW_GATHERING, waiting, total)
		: g_strdup_printf(TEXT_PREVIEW_OF_LIKED, total);
	gtk_label_set_text(GTK_LABEL(editor->note), note);
	gtk_list_box_remove_all(GTK_LIST_BOX(editor->preview));
	for (guint index = 0; index < MIN(songs->len, PREVIEW_SONGS); index++)
		gtk_list_box_append(GTK_LIST_BOX(editor->preview), create_preview_row(g_ptr_array_index(songs, index)));
	if (songs->len > PREVIEW_SONGS) {
		g_autofree char *more = g_strdup_printf(TEXT_PREVIEW_MORE, songs->len - PREVIEW_SONGS);
		GtkWidget *label = gtk_label_new(more);
		gtk_widget_add_css_class(label, "dimmed");
		gtk_widget_set_margin_top(label, 6);
		gtk_widget_set_margin_bottom(label, 6);
		gtk_list_box_append(GTK_LIST_BOX(editor->preview), label);
	}
	if (!songs->len) {
		GtkWidget *empty = adw_status_page_new();
		adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "edit-find-symbolic");
		adw_status_page_set_title(ADW_STATUS_PAGE(empty), TEXT_PREVIEW_EMPTY);
		adw_status_page_set_description(ADW_STATUS_PAGE(empty), TEXT_PREVIEW_EMPTY_DETAIL);
		gtk_widget_add_css_class(empty, "compact");
		gtk_list_box_append(GTK_LIST_BOX(editor->preview), empty);
	}
}


static gboolean fill_preview_later(gpointer data) {
	Editor *editor = data;
	editor->preview_source = 0;
	fill_preview(editor);
	return G_SOURCE_REMOVE;
}


static void schedule_preview(Editor *editor) {
	if (!editor->preview_source)
		editor->preview_source = g_timeout_add(PREVIEW_DELAY_MS, fill_preview_later, editor);
}


/* A song in the preview opens its information over the editor, to see why it matches */
static void open_preview_song(GtkListBox *list, GtkListBoxRow *row, Editor *editor) {
	CrimsonSong *song = g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(row)), "song");
	if (song)
		show_info_dialog(GTK_WIDGET(editor->dialog), song);
}


static GtkWidget *create_preview(Editor *editor) {
	GtkWidget *pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(pane, "preview-pane");
	gtk_widget_set_size_request(pane, PREVIEW_WIDTH, -1);
	GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_add_css_class(heading, "preview-heading");
	GtkWidget *label = gtk_label_new(TEXT_PREVIEW);
	gtk_widget_add_css_class(label, "caption-heading");
	gtk_widget_add_css_class(label, "dimmed");
	editor->count = gtk_label_new(NULL);
	gtk_widget_add_css_class(editor->count, "title-2");
	editor->note = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(editor->note), TRUE);
	gtk_widget_add_css_class(editor->note, "dimmed");
	gtk_widget_add_css_class(editor->note, "caption");
	for (GtkWidget *child = label; child; child = child == label ? editor->count : child == editor->count
		? editor->note : NULL) {
		gtk_label_set_xalign(GTK_LABEL(child), 0);
		gtk_box_append(GTK_BOX(heading), child);
	}
	gtk_box_append(GTK_BOX(pane), heading);
	editor->preview = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(editor->preview), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(editor->preview, "preview-list");
	g_signal_connect(editor->preview, "row-activated", G_CALLBACK(open_preview_song), editor);
	GtkWidget *scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scrolled, TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), editor->preview);
	gtk_box_append(GTK_BOX(pane), scrolled);
	return pane;
}


/* Naming =================================================================== */


/* Until the listener names it, the playlist is named after its first few included values, as in
 * "Synthwave, Happy" */
static void name_from_filters(Editor *editor) {
	if (!editor->is_name_automatic)
		return;
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	for (int facet = 0; facet < FACET_COUNT && names->len < 3; facet++) {
		g_autofree char *prefix = make_facet_key(facet, "");
		g_autofree gpointer *keys = g_hash_table_get_keys_as_array(editor->rules->included, NULL);
		for (char **key = (char **)keys; *key && names->len < 3; key++) {
			if (g_str_has_prefix(*key, prefix))
				g_ptr_array_add(names, show_value(facet, *key + strlen(prefix), NULL));
		}
	}
	g_ptr_array_add(names, NULL);
	g_autofree char *name = names->len > 1 ? g_strjoinv(", ", (char **)names->pdata) : g_strdup("");
	editor->is_naming = TRUE;
	gtk_editable_set_text(GTK_EDITABLE(editor->name), name);
	editor->is_naming = FALSE;
}


static void type_name(GtkEditable *entry, Editor *editor) {
	if (!editor->is_naming)
		editor->is_name_automatic = !*gtk_editable_get_text(entry);
}


/* Dialog =================================================================== */


static void free_editor(Editor *editor) {
	g_clear_handle_id(&editor->preview_source, g_source_remove);
	g_clear_pointer(&editor->rules, free_dynamic_rules);
	g_clear_object(&editor->pool);
	g_free(editor);
}


static void close_editor(AdwDialog *dialog, Editor *editor) {
	if (open_editor == editor)
		open_editor = NULL;
	free_editor(editor);
}


static void save_editor(GtkButton *button, Editor *editor) {
	const char *name = gtk_editable_get_text(GTK_EDITABLE(editor->name));
	g_free(editor->rules->name);
	editor->rules->name = g_strdup(*name ? name : TEXT_DYNAMIC_PLAYLIST);
	editor->saved(g_steal_pointer(&editor->rules), editor->data);
	editor->rules = create_dynamic_rules();
	adw_dialog_close(editor->dialog);
}


static void cancel_editor(GtkButton *button, Editor *editor) {
	adw_dialog_close(editor->dialog);
}


static GtkWidget *create_filters(Editor *editor) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
	gtk_widget_add_css_class(box, "filters");
	GtkWidget *naming = adw_preferences_group_new();
	editor->name = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(editor->name), TEXT_PLAYLIST_NAME);
	gtk_editable_set_text(GTK_EDITABLE(editor->name), editor->rules->name ? editor->rules->name : "");
	g_signal_connect(editor->name, "changed", G_CALLBACK(type_name), editor);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(naming), editor->name);
	gtk_box_append(GTK_BOX(box), naming);
	GtkWidget *help = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_add_css_class(help, "filter-help");
	GtkWidget *icon = gtk_image_new_from_icon_name("dialog-information-symbolic");
	GtkWidget *text = gtk_label_new(TEXT_FILTER_HELP);
	gtk_label_set_wrap(GTK_LABEL(text), TRUE);
	gtk_label_set_xalign(GTK_LABEL(text), 0);
	gtk_widget_set_hexpand(text, TRUE);
	gtk_box_append(GTK_BOX(help), icon);
	gtk_box_append(GTK_BOX(help), text);
	gtk_box_append(GTK_BOX(box), help);
	gtk_box_append(GTK_BOX(box), create_ranges(editor));
	for (guint index = 0; index < G_N_ELEMENTS(FACET_ORDER); index++)
		gtk_box_append(GTK_BOX(box), create_facet(editor, FACET_ORDER[index]));
	for (int facet = 0; facet < FACET_COUNT; facet++)
		fill_chips(editor, facet);
	GtkWidget *clamp = adw_clamp_new();
	adw_clamp_set_child(ADW_CLAMP(clamp), box);
	GtkWidget *scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_hexpand(scrolled, TRUE);
	gtk_widget_set_vexpand(scrolled, TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), clamp);
	return scrolled;
}


void show_dynamic_editor(GtkWidget *parent, const DynamicRules *rules, GListModel *pool, DynamicSaved saved,
	gpointer data) {
	Editor *editor = g_new0(Editor, 1);
	editor->rules = rules ? copy_dynamic_rules(rules) : create_dynamic_rules();
	editor->pool = pool ? g_object_ref(pool) : G_LIST_MODEL(g_list_store_new(CRIMSON_TYPE_SONG));
	editor->saved = saved;
	editor->data = data;
	editor->is_name_automatic = !rules;
	editor->dialog = adw_dialog_new();
	adw_dialog_set_title(editor->dialog, rules ? TEXT_EDIT_DYNAMIC : TEXT_NEW_DYNAMIC);
	adw_dialog_set_content_width(editor->dialog, 980);
	adw_dialog_set_content_height(editor->dialog, 720);
	gtk_widget_set_size_request(GTK_WIDGET(editor->dialog), MINIMUM_WIDTH, MINIMUM_HEIGHT);
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
	GtkWidget *cancel = gtk_button_new_with_label(TEXT_CANCEL);
	g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_editor), editor);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
	editor->save = gtk_button_new_with_label(rules ? TEXT_SAVE : TEXT_CREATE);
	gtk_widget_add_css_class(editor->save, "suggested-action");
	g_signal_connect(editor->save, "clicked", G_CALLBACK(save_editor), editor);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), editor->save);
	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(content), create_filters(editor));
	gtk_box_append(GTK_BOX(content), create_preview(editor));
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);
	adw_dialog_set_child(editor->dialog, toolbar);
	AdwBreakpoint *narrow = adw_breakpoint_new(adw_breakpoint_condition_parse(NARROW_WIDTH));
	GValue vertical = G_VALUE_INIT;
	g_value_init(&vertical, GTK_TYPE_ORIENTATION);
	g_value_set_enum(&vertical, GTK_ORIENTATION_VERTICAL);
	adw_breakpoint_add_setter(narrow, G_OBJECT(content), "orientation", &vertical);
	adw_dialog_add_breakpoint(editor->dialog, narrow);
	g_signal_connect(editor->dialog, "closed", G_CALLBACK(close_editor), editor);
	open_editor = editor;
	fill_preview(editor);
	close_on_outside_click(editor->dialog);
	adw_dialog_present(editor->dialog, parent);
	gtk_widget_grab_focus(editor->name);
}


void update_dynamic_editor(void) {
	if (open_editor)
		schedule_preview(open_editor);
}

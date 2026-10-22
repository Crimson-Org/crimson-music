#include "../lang/en.h"
#include "artwork.h"
#include "dialogs.h"
#include "info-dialog.h"
#include "lastfm.h"
#include "settings.h"
#include "song-info.h"

/* A mood counts as part of the song from this likelihood in percent on */
#define MOOD_LIKELY 60
#define DIALOG_WIDTH 560
#define DIALOG_HEIGHT 680
#define COVER_SIZE 112
#define FACT_CHARACTERS 18
/* Longer texts show this many lines until opened, and a text this short shows whole */
#define TEXT_LINES 3
#define SHORT_TEXT 280
#define YOUTUBE_ADDRESS "https://music.youtube.com/watch?v="

static const char *const MOOD_NAMES[] = {
	TEXT_DANCEABLE, TEXT_ENERGETIC, TEXT_CALM, TEXT_HAPPY, TEXT_SAD, TEXT_ACOUSTIC, TEXT_INSTRUMENTAL,
};

/* Only one information dialog is open at a time, and it fills in again when its song is gathered */
static AdwDialog *open_dialog;
static CrimsonSong *open_song;
/* What Last.fm said about the open song and its artist, fetched when the dialog opens */
static GHashTable *lastfm_details;
static GtkWidget *open_chips;
/* What the page was filled from, so a change of the listener's tags alone leaves the page and its scrolling be */
static const SongInfo *shown_info;
static gboolean was_gathering;

/* Digits in groups of three, as in 881 763 012 */
static char *format_count(const char *digits) {
	GString *text = g_string_new(NULL);
	size_t length = strlen(digits);
	for (size_t index = 0; index < length; index++) {
		if (index > 0 && (length - index) % 3 == 0)
			g_string_append_unichar(text, 0x202F);
		g_string_append_c(text, digits[index]);
	}
	return g_string_free(text, FALSE);
}


/* Pieces ================================================================== */


static GtkWidget *add_section(GtkWidget *page, const char *title) {
	GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(section, "info-section");
	if (title) {
		GtkWidget *heading = gtk_label_new(title);
		gtk_label_set_xalign(GTK_LABEL(heading), 0);
		gtk_widget_add_css_class(heading, "info-heading");
		gtk_box_append(GTK_BOX(section), heading);
	}
	gtk_box_append(GTK_BOX(page), section);
	return section;
}


static GtkWidget *create_wrap(void) {
	GtkWidget *wrap = adw_wrap_box_new();
	adw_wrap_box_set_child_spacing(ADW_WRAP_BOX(wrap), 6);
	adw_wrap_box_set_line_spacing(ADW_WRAP_BOX(wrap), 6);
	return wrap;
}


/* One small tile of the facts grid, a dim name over its value */
static void add_fact(GtkWidget *facts, const char *title, const char *value) {
	if (!value || !*value)
		return;
	GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_add_css_class(tile, "info-fact");
	GtkWidget *name = gtk_label_new(title);
	gtk_widget_add_css_class(name, "caption");
	gtk_widget_add_css_class(name, "dimmed");
	GtkWidget *text = gtk_label_new(value);
	gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(text), FACT_CHARACTERS);
	gtk_widget_set_tooltip_text(text, value);
	gtk_widget_add_css_class(text, "info-fact-value");
	for (GtkWidget *label = name; label; label = label == name ? text : NULL) {
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_box_append(GTK_BOX(tile), label);
	}
	adw_wrap_box_append(ADW_WRAP_BOX(facts), tile);
}


static void add_count_fact(GtkWidget *facts, const char *title, const char *digits) {
	g_autofree char *count = digits && *digits && !g_str_equal(digits, "0") ? format_count(digits) : NULL;
	add_fact(facts, title, count);
}


/* Chips of words, each kept once across all the chips of the dialog, so a genre several sources name shows once */
static void add_chips(GtkWidget *wrap, const char *list, const char *style, GHashTable *shown) {
	g_auto(GStrv) words = g_strsplit(list ? list : "", ",", -1);
	for (char **word = words; *word; word++) {
		g_autofree char *clean = g_utf8_strdown(g_strstrip(*word), -1);
		if (!*clean || g_hash_table_contains(shown, clean))
			continue;
		g_hash_table_add(shown, g_strdup(clean));
		GtkWidget *chip = gtk_label_new(g_strstrip(*word));
		gtk_widget_add_css_class(chip, "info-chip");
		gtk_widget_add_css_class(chip, style);
		adw_wrap_box_append(ADW_WRAP_BOX(wrap), chip);
	}
}


/* The first few lines of a text, and no more than so many letters, ending in an ellipsis when anything was left
 * out */
static char *shorten_text(const char *text) {
	g_auto(GStrv) lines = g_strsplit(text, "\n", TEXT_LINES + 1);
	guint count = MIN(g_strv_length(lines), TEXT_LINES);
	g_autoptr(GString) shown = g_string_new(NULL);
	for (guint index = 0; index < count; index++)
		g_string_append_printf(shown, "%s%s", index ? "\n" : "", lines[index]);
	gboolean is_cut = g_strv_length(lines) > TEXT_LINES;
	if (g_utf8_strlen(shown->str, -1) > SHORT_TEXT) {
		g_string_truncate(shown, (gsize)(g_utf8_offset_to_pointer(shown->str, SHORT_TEXT) - shown->str));
		is_cut = TRUE;
	}
	if (is_cut) {
		g_strchomp(shown->str);
		g_string_set_size(shown, strlen(shown->str));
		g_string_append(shown, "…");
	}
	return g_string_free(g_steal_pointer(&shown), FALSE);
}


static void toggle_more(GtkButton *button, GtkLabel *label) {
	gboolean is_open = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(label), "open"));
	const char *text = g_object_get_data(G_OBJECT(label), "text");
	g_autofree char *short_text = shorten_text(text);
	gtk_label_set_text(label, is_open ? short_text : text);
	g_object_set_data(G_OBJECT(label), "open", GINT_TO_POINTER(!is_open));
	gtk_button_set_label(button, is_open ? TEXT_SHOW_MORE : TEXT_SHOW_LESS);
}


/* A longer text shows its first few lines, and the rest on asking, and its selection starts nowhere */
static void add_text(GtkWidget *page, const char *title, const char *text) {
	if (!text || !*text)
		return;
	GtkWidget *section = add_section(page, title);
	g_autofree char *short_text = shorten_text(text);
	GtkWidget *label = gtk_label_new(short_text);
	g_object_set_data_full(G_OBJECT(label), "text", g_strdup(text), g_free);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "info-text");
	gtk_box_append(GTK_BOX(section), label);
	if (g_str_equal(short_text, text))
		return;
	GtkWidget *more = gtk_button_new_with_label(TEXT_SHOW_MORE);
	gtk_widget_add_css_class(more, "flat");
	gtk_widget_add_css_class(more, "info-more");
	gtk_widget_set_halign(more, GTK_ALIGN_START);
	g_signal_connect(more, "clicked", G_CALLBACK(toggle_more), label);
	gtk_box_append(GTK_BOX(section), more);
}


static const char *get_detail(const char *name) {
	const char *value = lastfm_details ? g_hash_table_lookup(lastfm_details, name) : NULL;
	return value ? value : "";
}


static void add_pill(GtkWidget *pills, const char *text, const char *style) {
	if (!text || !*text)
		return;
	GtkWidget *pill = gtk_label_new(text);
	gtk_widget_add_css_class(pill, "info-pill");
	if (style)
		gtk_widget_add_css_class(pill, style);
	adw_wrap_box_append(ADW_WRAP_BOX(pills), pill);
}


/* Parts ==================================================================== */


/* The cover beside the title, artist and album, with the song's length, year, tempo and key as pills below */
static void fill_header(GtkWidget *page, const char *id) {
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	gtk_widget_add_css_class(header, "info-header");
	GtkWidget *cover = create_cover(COVER_SIZE);
	show_artwork(cover, open_song->artwork);
	gtk_widget_set_valign(cover, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(header), cover);
	GtkWidget *words = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_hexpand(words, TRUE);
	gtk_widget_set_valign(words, GTK_ALIGN_CENTER);
	GtkWidget *title = gtk_label_new(open_song->title);
	gtk_widget_add_css_class(title, "title-3");
	gtk_label_set_wrap(GTK_LABEL(title), TRUE);
	gtk_label_set_lines(GTK_LABEL(title), 2);
	gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
	GtkWidget *artist = gtk_label_new(open_song->artist);
	gtk_label_set_ellipsize(GTK_LABEL(artist), PANGO_ELLIPSIZE_END);
	const char *album = *get_song_info_value(id, INFO_ALBUM) ? get_song_info_value(id, INFO_ALBUM) : open_song->album;
	GtkWidget *album_label = gtk_label_new(album);
	gtk_widget_add_css_class(album_label, "dimmed");
	gtk_label_set_ellipsize(GTK_LABEL(album_label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_visible(album_label, album && *album);
	for (GtkWidget *label = title; label; label = label == title ? artist : label == artist ? album_label : NULL) {
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_box_append(GTK_BOX(words), label);
	}
	GtkWidget *pills = create_wrap();
	gtk_widget_set_margin_top(pills, 6);
	double seconds = get_song_info_number(id, INFO_SECONDS);
	g_autofree char *length = seconds > 0 ? format_song_length(seconds) : g_strdup(open_song->duration);
	add_pill(pills, length, NULL);
	const char *released = get_song_info_value(id, INFO_RELEASED);
	const char *date = *released ? released : get_song_info_value(id, INFO_UPLOADED);
	g_autofree char *year = strlen(date) >= 4 ? g_strndup(date, 4) : NULL;
	add_pill(pills, year, NULL);
	double bpm = get_song_info_number(id, INFO_BPM);
	g_autofree char *tempo = bpm > 0 ? g_strdup_printf(TEXT_BPM_FORMAT, (int)bpm) : NULL;
	add_pill(pills, tempo, NULL);
	add_pill(pills, get_song_info_value(id, INFO_KEY), NULL);
	if (get_song_info_number(id, INFO_EXPLICIT) > 0)
		add_pill(pills, TEXT_EXPLICIT_MARK, "explicit");
	if (g_str_equal(get_detail("loved"), "1"))
		add_pill(pills, TEXT_LOVED, "loved");
	gtk_box_append(GTK_BOX(words), pills);
	gtk_box_append(GTK_BOX(header), words);
	gtk_box_append(GTK_BOX(page), header);
}


/* Everything known that fits in a word or a number, as small tiles side by side */
static void fill_facts(GtkWidget *page, const char *id) {
	GtkWidget *facts = create_wrap();
	adw_wrap_box_set_justify(ADW_WRAP_BOX(facts), ADW_JUSTIFY_FILL);
	gtk_widget_add_css_class(facts, "info-facts");
	add_fact(facts, TEXT_RELEASED, get_song_info_value(id, INFO_RELEASED));
	add_fact(facts, TEXT_LABEL, get_song_info_value(id, INFO_LABEL));
	const char *gain = get_song_info_value(id, INFO_GAIN);
	g_autofree char *loudness = *gain ? g_strdup_printf(TEXT_DECIBEL_FORMAT, gain) : NULL;
	add_fact(facts, TEXT_LOUDNESS, loudness);
	add_fact(facts, TEXT_CHANNEL, get_song_info_value(id, INFO_CHANNEL));
	add_fact(facts, TEXT_UPLOADED, get_song_info_value(id, INFO_UPLOADED));
	add_count_fact(facts, TEXT_VIEWS, get_song_info_value(id, INFO_VIEWS));
	add_count_fact(facts, TEXT_YOUR_PLAYS, get_detail("yourPlays"));
	add_count_fact(facts, TEXT_LISTENERS, get_detail("listeners"));
	add_count_fact(facts, TEXT_SCROBBLES, get_detail("plays"));
	g_autoptr(GPtrArray) chapters = get_song_chapters(id);
	g_autofree char *chapter_count = chapters->len ? g_strdup_printf("%u", chapters->len) : NULL;
	add_fact(facts, TEXT_CHAPTERS, chapter_count);
	add_fact(facts, TEXT_ISRC, get_song_info_value(id, INFO_ISRC));
	if (gtk_widget_get_first_child(facts))
		gtk_box_append(GTK_BOX(page), facts);
}


/* Genres from every source as one row of chips, then moods and Last.fm's own tags in their own colours */
static void fill_tags(GtkWidget *page, const char *id) {
	g_autoptr(GHashTable) shown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GtkWidget *wrap = create_wrap();
	add_chips(wrap, get_song_info_value(id, INFO_GENRES), "genre", shown);
	add_chips(wrap, get_song_info_value(id, INFO_TAGS), "genre", shown);
	add_chips(wrap, get_song_info_value(id, INFO_STYLES), "genre", shown);
	g_autoptr(GStrvBuilder) moods = g_strv_builder_new();
	for (guint index = 0; index < G_N_ELEMENTS(MOOD_NAMES); index++) {
		if (get_song_info_number(id, INFO_FIRST_MOOD + index) >= MOOD_LIKELY)
			g_strv_builder_add(moods, MOOD_NAMES[index]);
	}
	g_auto(GStrv) mood_list = g_strv_builder_end(moods);
	g_autofree char *mood_text = g_strjoinv(",", mood_list);
	add_chips(wrap, mood_text, "mood", shown);
	add_chips(wrap, get_song_info_value(id, INFO_LASTFM), "lastfm", shown);
	if (!gtk_widget_get_first_child(wrap))
		return;
	GtkWidget *section = add_section(page, TEXT_TAGS);
	gtk_box_append(GTK_BOX(section), wrap);
}


static void open_link(GtkButton *button, gpointer data) {
	g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(g_object_get_data(G_OBJECT(button), "address"));
	gtk_uri_launcher_launch(launcher, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))), NULL, NULL, NULL);
}


static void add_link(GtkWidget *links, const char *label, const char *address) {
	if (!address || !*address)
		return;
	GtkWidget *button = gtk_button_new_with_label(label);
	gtk_widget_add_css_class(button, "info-link");
	g_object_set_data_full(G_OBJECT(button), "address", g_strdup(address), g_free);
	g_signal_connect(button, "clicked", G_CALLBACK(open_link), NULL);
	adw_wrap_box_append(ADW_WRAP_BOX(links), button);
}


/* Links to where the information came from, which Last.fm also asks for, and the sources named below them */
static void fill_footer(GtkWidget *page, const char *id) {
	GtkWidget *links = create_wrap();
	gtk_widget_add_css_class(links, "info-links");
	add_link(links, TEXT_SONG_ON_LASTFM, get_detail("url"));
	add_link(links, TEXT_ARTIST_ON_LASTFM, get_detail("artistUrl"));
	g_autofree char *youtube = g_strconcat(YOUTUBE_ADDRESS, id, NULL);
	add_link(links, TEXT_ON_YOUTUBE, youtube);
	gtk_box_append(GTK_BOX(page), links);
	const char *sources = get_song_info_value(id, INFO_SOURCES);
	if (!*sources)
		return;
	g_autofree char *credit = g_strdup_printf(TEXT_GATHERED_FROM, sources);
	GtkWidget *label = gtk_label_new(credit);
	gtk_widget_add_css_class(label, "caption");
	gtk_widget_add_css_class(label, "dimmed");
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_box_append(GTK_BOX(page), label);
}


static void fill_page(void);


static void receive_lastfm_details(const char *video_id, char **answers, gpointer data) {
	if (!open_dialog || g_strcmp0(video_id, open_song->id) != 0)
		return;
	g_clear_pointer(&lastfm_details, g_hash_table_unref);
	lastfm_details = read_lastfm_details(answers);
	fill_page();
}


/* Own tags ================================================================ */


static void remove_tag(GtkButton *button, gpointer data) {
	const char *removed = g_object_get_data(G_OBJECT(button), "tag");
	g_auto(GStrv) tags = get_custom_tags(open_song->id);
	g_autoptr(GStrvBuilder) kept = g_strv_builder_new();
	for (char **tag = tags; tag && *tag; tag++) {
		if (!g_str_equal(*tag, removed))
			g_strv_builder_add(kept, *tag);
	}
	g_auto(GStrv) result = g_strv_builder_end(kept);
	if (get_settings()->shares_tags)
		tag_on_lastfm(open_song, removed, FALSE);
	set_custom_tags(open_song->id, (const char *const *)result);
}


/* Several tags can be typed at once with commas between them */
static void add_tags(GtkEntry *entry, gpointer data) {
	g_auto(GStrv) typed = g_strsplit(gtk_editable_get_text(GTK_EDITABLE(entry)), ",", -1);
	g_auto(GStrv) tags = get_custom_tags(open_song->id);
	g_autoptr(GStrvBuilder) result = g_strv_builder_new();
	g_strv_builder_addv(result, (const char **)(tags ? tags : (char *[]){NULL}));
	for (char **tag = typed; *tag; tag++) {
		g_strstrip(*tag);
		if (**tag && !(tags && g_strv_contains((const char *const *)tags, *tag))) {
			g_strv_builder_add(result, *tag);
			if (get_settings()->shares_tags)
				tag_on_lastfm(open_song, *tag, TRUE);
		}
	}
	g_auto(GStrv) joined = g_strv_builder_end(result);
	gtk_editable_set_text(GTK_EDITABLE(entry), "");
	set_custom_tags(open_song->id, (const char *const *)joined);
}


static void fill_chips(void) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(open_chips)))
		adw_wrap_box_remove(ADW_WRAP_BOX(open_chips), child);
	g_auto(GStrv) tags = get_custom_tags(open_song->id);
	for (char **tag = tags; tag && *tag; tag++) {
		GtkWidget *content = adw_button_content_new();
		adw_button_content_set_label(ADW_BUTTON_CONTENT(content), *tag);
		adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), "window-close-symbolic");
		GtkWidget *button = gtk_button_new();
		gtk_button_set_child(GTK_BUTTON(button), content);
		gtk_widget_set_tooltip_text(button, TEXT_REMOVE_TAG);
		gtk_widget_add_css_class(button, "tag-chip");
		g_object_set_data_full(G_OBJECT(button), "tag", g_strdup(*tag), g_free);
		g_signal_connect(button, "clicked", G_CALLBACK(remove_tag), NULL);
		adw_wrap_box_append(ADW_WRAP_BOX(open_chips), button);
	}
}


/* The listener's own tags as chips that remove themselves on a click, and a field that adds more */
static void fill_own_tags(GtkWidget *page) {
	GtkWidget *section = add_section(page, TEXT_YOUR_TAGS);
	open_chips = create_wrap();
	gtk_box_append(GTK_BOX(section), open_chips);
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), TEXT_ADD_TAGS);
	gtk_widget_set_tooltip_text(entry, TEXT_YOUR_TAGS_DETAIL);
	g_signal_connect(entry, "activate", G_CALLBACK(add_tags), NULL);
	gtk_box_append(GTK_BOX(section), entry);
	fill_chips();
}


/* The page is made again whenever more is known, keeping its place in the scrolling */
static void fill_page(void) {
	const char *id = open_song->id;
	GtkWidget *toolbar = adw_dialog_get_child(open_dialog);
	GtkWidget *old = adw_toolbar_view_get_content(ADW_TOOLBAR_VIEW(toolbar));
	double place = old ? gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(old))) : 0;
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
	gtk_widget_add_css_class(page, "info-page");
	fill_header(page, id);
	gboolean is_known = get_song_info(id) != NULL;
	if (is_gathering_song_info(id) || !is_known || (is_lastfm_available() && !lastfm_details)) {
		GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_box_append(GTK_BOX(status), adw_spinner_new());
		GtkWidget *label = gtk_label_new(is_known ? TEXT_GATHERING_AGAIN : TEXT_GATHERING);
		gtk_widget_add_css_class(label, "dimmed");
		gtk_box_append(GTK_BOX(status), label);
		gtk_box_append(GTK_BOX(page), status);
	}
	fill_facts(page, id);
	fill_tags(page, id);
	fill_own_tags(page);
	add_text(page, TEXT_ABOUT_SONG_WIKI, get_detail("wiki"));
	add_text(page, TEXT_ABOUT_ARTIST, get_detail("artistBio"));
	add_text(page, TEXT_DESCRIPTION, get_song_info_value(id, INFO_DESCRIPTION));
	fill_footer(page, id);
	shown_info = get_song_info(id);
	was_gathering = is_gathering_song_info(id);
	GtkWidget *clamp = adw_clamp_new();
	adw_clamp_set_child(ADW_CLAMP(clamp), page);
	GtkWidget *scrolled = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), clamp);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scrolled);
	gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled)), place);
}


static void forget_dialog(AdwDialog *dialog, gpointer data) {
	open_dialog = NULL;
	g_clear_pointer(&lastfm_details, g_hash_table_unref);
	g_clear_object(&open_song);
}


void show_info_dialog(GtkWidget *parent, CrimsonSong *song) {
	if (open_dialog)
		adw_dialog_force_close(open_dialog);
	open_song = g_object_ref(song);
	open_dialog = adw_dialog_new();
	adw_dialog_set_title(open_dialog, song->title);
	adw_dialog_set_content_width(open_dialog, DIALOG_WIDTH);
	adw_dialog_set_content_height(open_dialog, DIALOG_HEIGHT);
	GtkWidget *toolbar = adw_toolbar_view_new();
	GtkWidget *header = adw_header_bar_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_dialog_set_child(open_dialog, toolbar);
	g_signal_connect(open_dialog, "closed", G_CALLBACK(forget_dialog), NULL);
	gather_song_info_now(song);
	g_clear_pointer(&lastfm_details, g_hash_table_unref);
	fetch_lastfm_details(song, receive_lastfm_details, NULL);
	fill_page();
	adw_dialog_present(open_dialog, parent);
	close_on_outside_click(open_dialog);
}


void update_info_dialog(const char *video_id) {
	if (!open_dialog || g_strcmp0(video_id, open_song->id) != 0)
		return;
	if (get_song_info(video_id) != shown_info || is_gathering_song_info(video_id) != was_gathering)
		fill_page();
	else
		fill_chips();
}

#include <adwaita.h>

#include "../lang/en.h"
#include "dialogs.h"
#include "lastfm.h"
#include "local-library.h"
#include "settings-dialog.h"
#include "settings.h"
#include "window.h"

#define SETTINGS_WIDTH 640
#define SETTINGS_HEIGHT 520


static void change_style(AdwComboRow *row, GParamSpec *spec, gpointer data) {
	get_settings()->style = (Style)adw_combo_row_get_selected(row);
	save_settings();
	apply_style();
}


static void change_resume(AdwSwitchRow *row, GParamSpec *spec, gpointer data) {
	get_settings()->resumes_songs = adw_switch_row_get_active(row);
	save_settings();
}


static void change_visuals(AdwComboRow *row, GParamSpec *spec, gpointer data) {
	get_settings()->visuals = (PlayerVisuals)adw_combo_row_get_selected(row);
	save_settings();
	apply_player_visuals();
}


static GtkWidget *create_appearance_group(void) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), TEXT_APPEARANCE);
	static const char *const STYLE_NAMES[] = {TEXT_FOLLOW_SYSTEM, TEXT_LIGHT, TEXT_DARK, NULL};
	g_autoptr(GtkStringList) styles = gtk_string_list_new(STYLE_NAMES);
	GtkWidget *style = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(style), TEXT_STYLE);
	adw_combo_row_set_model(ADW_COMBO_ROW(style), G_LIST_MODEL(styles));
	adw_combo_row_set_selected(ADW_COMBO_ROW(style), get_settings()->style);
	g_signal_connect(style, "notify::selected", G_CALLBACK(change_style), NULL);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), style);
	static const char *const VISUALS_TITLES[] = {TEXT_VISUALS_PLAIN, TEXT_VISUALS_COLORS, NULL};
	g_autoptr(GtkStringList) visuals_list = gtk_string_list_new(VISUALS_TITLES);
	GtkWidget *visuals = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(visuals), TEXT_PLAYER_VISUALS);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(visuals), TEXT_PLAYER_VISUALS_DETAIL);
	adw_combo_row_set_model(ADW_COMBO_ROW(visuals), G_LIST_MODEL(visuals_list));
	adw_combo_row_set_selected(ADW_COMBO_ROW(visuals), get_settings()->visuals);
	g_signal_connect(visuals, "notify::selected", G_CALLBACK(change_visuals), NULL);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), visuals);
	return group;
}


static void add_switch(GtkWidget *group, const char *title, const char *detail, gboolean is_active, GCallback changed) {
	GtkWidget *row = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), detail);
	adw_switch_row_set_active(ADW_SWITCH_ROW(row), is_active);
	g_signal_connect(row, "notify::active", changed, NULL);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
}


static void change_restore(AdwSwitchRow *row, GParamSpec *spec, gpointer data) {
	get_settings()->restores_last_song = adw_switch_row_get_active(row);
	save_settings();
}


static void change_video_autoplay(AdwSwitchRow *row, GParamSpec *spec, gpointer data) {
	get_settings()->autoplays_videos = adw_switch_row_get_active(row);
	save_settings();
}


static void change_suggestions(AdwSwitchRow *row, GParamSpec *spec, gpointer data) {
	get_settings()->shows_suggestions = adw_switch_row_get_active(row);
	save_settings();
	apply_playlist_suggestions();
}


static GtkWidget *create_playback_group(void) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), TEXT_PLAYBACK);
	GtkWidget *resume = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(resume), TEXT_RESUME_SONGS);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(resume), TEXT_RESUME_SONGS_DETAIL);
	adw_switch_row_set_active(ADW_SWITCH_ROW(resume), get_settings()->resumes_songs);
	g_signal_connect(resume, "notify::active", G_CALLBACK(change_resume), NULL);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), resume);
	add_switch(group, TEXT_RESTORE_LAST_SONG, TEXT_RESTORE_LAST_SONG_DETAIL, get_settings()->restores_last_song,
		G_CALLBACK(change_restore));
	add_switch(group, TEXT_AUTOPLAY_VIDEO, TEXT_AUTOPLAY_VIDEO_DETAIL, get_settings()->autoplays_videos,
		G_CALLBACK(change_video_autoplay));
	add_switch(group, TEXT_SHOW_SUGGESTIONS, TEXT_SHOW_SUGGESTIONS_DETAIL, get_settings()->shows_suggestions,
		G_CALLBACK(change_suggestions));
	return group;
}


/* The Last.fm row of an open settings dialog, which follows the connection as it changes */
static GtkWidget *lastfm_row, *lastfm_button, *lastfm_profile;


static void click_lastfm(GtkButton *button, gpointer data) {
	if (is_lastfm_connecting())
		cancel_lastfm();
	else if (get_lastfm_user())
		disconnect_lastfm();
	else
		connect_lastfm(GTK_WIDGET(button));
}


void update_lastfm_settings(void) {
	if (!lastfm_row)
		return;
	const char *user = get_lastfm_user();
	g_autoptr(GString) connected = g_string_new(NULL);
	if (user) {
		g_string_printf(connected, TEXT_LASTFM_CONNECTED, user);
		if (get_lastfm_scrobbles())
			g_string_append_printf(connected, TEXT_LASTFM_SCROBBLES, get_lastfm_scrobbles());
		if (count_waiting_scrobbles())
			g_string_append_printf(connected, TEXT_LASTFM_UNSENT, count_waiting_scrobbles());
	}
	const char *profile = get_lastfm_profile();
	gtk_widget_set_visible(lastfm_profile, profile != NULL);
	if (profile)
		gtk_link_button_set_uri(GTK_LINK_BUTTON(lastfm_profile), profile);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(lastfm_row), is_lastfm_connecting() ? TEXT_LASTFM_WAITING
		: user ? connected->str : TEXT_LASTFM_DETAIL);
	gtk_button_set_label(GTK_BUTTON(lastfm_button), is_lastfm_connecting() ? TEXT_CANCEL : user ? TEXT_DISCONNECT
		: TEXT_CONNECT);
	if (!user && !is_lastfm_connecting())
		gtk_widget_add_css_class(lastfm_button, "suggested-action");
	else
		gtk_widget_remove_css_class(lastfm_button, "suggested-action");
}


static void forget_lastfm_row(gpointer data, GObject *row) {
	lastfm_row = NULL;
	lastfm_button = NULL;
	lastfm_profile = NULL;
}


static void change_sharing(AdwSwitchRow *row, GParamSpec *spec, gpointer data) {
	get_settings()->shares_tags = adw_switch_row_get_active(row);
	save_settings();
}


static void change_reminder(AdwSwitchRow *row, GParamSpec *spec, gpointer data) {
	get_settings()->shows_lastfm_reminder = adw_switch_row_get_active(row);
	save_settings();
	update_lastfm_state();
}




static GtkWidget *create_lastfm_group(void) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), TEXT_LASTFM);
	lastfm_row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(lastfm_row), TEXT_LASTFM_SCROBBLING);
	lastfm_button = gtk_button_new();
	gtk_widget_set_valign(lastfm_button, GTK_ALIGN_CENTER);
	g_signal_connect(lastfm_button, "clicked", G_CALLBACK(click_lastfm), NULL);
	lastfm_profile = gtk_link_button_new_with_label("https://www.last.fm", TEXT_PROFILE);
	gtk_widget_set_valign(lastfm_profile, GTK_ALIGN_CENTER);
	adw_action_row_add_suffix(ADW_ACTION_ROW(lastfm_row), lastfm_profile);
	adw_action_row_add_suffix(ADW_ACTION_ROW(lastfm_row), lastfm_button);
	g_object_weak_ref(G_OBJECT(lastfm_row), forget_lastfm_row, NULL);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), lastfm_row);
	Settings *settings = get_settings();
	add_switch(group, TEXT_SHARE_TAGS, TEXT_SHARE_TAGS_DETAIL, settings->shares_tags, G_CALLBACK(change_sharing));
	add_switch(group, TEXT_LASTFM_REMINDER_SETTING, TEXT_LASTFM_REMINDER_DETAIL, settings->shows_lastfm_reminder,
		G_CALLBACK(change_reminder));
	update_lastfm_settings();
	return group;
}


/* Local media ============================================================== */


static GtkWidget *folder_group;
static GPtrArray *folder_rows;


static void show_folders(void);


static void use_folders(GPtrArray *folders) {
	g_ptr_array_add(folders, NULL);
	g_strfreev(get_settings()->media_folders);
	get_settings()->media_folders = g_strdupv((char **)folders->pdata);
	save_settings();
	rescan_local_library();
	show_folders();
}


static void remove_folder(GtkButton *button, gpointer data) {
	const char *removed = g_object_get_data(G_OBJECT(button), "folder");
	g_autoptr(GPtrArray) folders = g_ptr_array_new();
	for (char **folder = get_settings()->media_folders; *folder; folder++) {
		if (g_strcmp0(*folder, removed) != 0)
			g_ptr_array_add(folders, *folder);
	}
	use_folders(folders);
}


static void finish_adding_folder(GObject *dialog, GAsyncResult *result, gpointer data) {
	g_autoptr(GFile) file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(dialog), result, NULL);
	g_autofree char *path = file ? g_file_get_path(file) : NULL;
	if (!path || g_strv_contains((const char *const *)get_settings()->media_folders, path))
		return;
	g_autoptr(GPtrArray) folders = g_ptr_array_new();
	for (char **folder = get_settings()->media_folders; *folder; folder++)
		g_ptr_array_add(folders, *folder);
	g_ptr_array_add(folders, path);
	use_folders(folders);
}


static void add_folder(GtkButton *button, gpointer data) {
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_ADD_MEDIA_FOLDER);
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));
	gtk_file_dialog_select_folder(dialog, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL, finish_adding_folder,
		NULL);
}


static void show_folders(void) {
	for (guint index = 0; folder_rows && index < folder_rows->len; index++)
		adw_preferences_group_remove(ADW_PREFERENCES_GROUP(folder_group), g_ptr_array_index(folder_rows, index));
	g_clear_pointer(&folder_rows, g_ptr_array_unref);
	folder_rows = g_ptr_array_new();
	for (char **folder = get_settings()->media_folders; *folder; folder++) {
		GtkWidget *row = adw_action_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), *folder);
		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
		gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(remove, "flat");
		gtk_widget_set_tooltip_text(remove, TEXT_REMOVE_MEDIA_FOLDER);
		g_object_set_data_full(G_OBJECT(remove), "folder", g_strdup(*folder), g_free);
		g_signal_connect(remove, "clicked", G_CALLBACK(remove_folder), NULL);
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(folder_group), row);
		g_ptr_array_add(folder_rows, row);
	}
}


static void forget_folder_group(gpointer data, GObject *group) {
	folder_group = NULL;
	g_clear_pointer(&folder_rows, g_ptr_array_unref);
}


static GtkWidget *create_local_group(void) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), TEXT_MEDIA_FOLDERS);
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), TEXT_MEDIA_FOLDERS_DETAIL);
	GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
	gtk_widget_add_css_class(add, "flat");
	gtk_widget_set_tooltip_text(add, TEXT_ADD_MEDIA_FOLDER);
	g_signal_connect(add, "clicked", G_CALLBACK(add_folder), NULL);
	adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group), add);
	folder_group = group;
	g_object_weak_ref(G_OBJECT(group), forget_folder_group, NULL);
	show_folders();
	return group;
}


/* Each page is a tab along the top, as in Crimson Code, and holds few enough rows to need no scrolling */
static void add_page(AdwDialog *dialog, const char *name, const char *title, const char *icon, GtkWidget *group) {
	GtkWidget *page = adw_preferences_page_new();
	adw_preferences_page_set_name(ADW_PREFERENCES_PAGE(page), name);
	adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), title);
	adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page), icon);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
	adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(page));
}


void show_settings(GtkWidget *parent, const char *page) {
	AdwDialog *dialog = adw_preferences_dialog_new();
	adw_dialog_set_title(dialog, TEXT_SETTINGS);
	adw_dialog_set_content_width(dialog, SETTINGS_WIDTH);
	adw_dialog_set_content_height(dialog, SETTINGS_HEIGHT);
	add_page(dialog, SETTINGS_PAGE_PLAYBACK, TEXT_PLAYBACK, "media-playback-start-symbolic", create_playback_group());
	add_page(dialog, SETTINGS_PAGE_APPEARANCE, TEXT_APPEARANCE, "applications-graphics-symbolic",
		create_appearance_group());
	add_page(dialog, SETTINGS_PAGE_LOCAL, TEXT_LOCAL_MEDIA, "folder-music-symbolic", create_local_group());
	if (is_lastfm_available())
		add_page(dialog, SETTINGS_PAGE_LASTFM, TEXT_LASTFM, "crimson-heart-symbolic", create_lastfm_group());
	if (page)
		adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(dialog), page);
	close_on_outside_click(dialog);
	adw_dialog_present(dialog, parent);
}

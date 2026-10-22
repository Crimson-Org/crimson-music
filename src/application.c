#include <adwaita.h>

#include "../lang/en.h"
#include "application.h"
#include "dialogs.h"
#include "settings-dialog.h"
#include "settings.h"
#include "window.h"


static void activate(GApplication *application) {
	GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(application));
	if (!window)
		window = get_gtk_window(create_window(GTK_APPLICATION(application)));
	gtk_window_present(window);
}


static void show_about(GSimpleAction *action, GVariant *parameter, gpointer application) {
	AdwDialog *dialog = adw_about_dialog_new();
	adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(dialog), get_application_identity()->name);
	adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(dialog), get_application_identity()->id);
	adw_about_dialog_set_version(ADW_ABOUT_DIALOG(dialog), get_application_identity()->version);
	close_on_outside_click(dialog);
	adw_dialog_present(dialog, GTK_WIDGET(gtk_application_get_active_window(application)));
}


static void open_settings(GSimpleAction *action, GVariant *parameter, gpointer application) {
	show_settings(GTK_WIDGET(gtk_application_get_active_window(application)), NULL);
}


static void open_lastfm_settings(GSimpleAction *action, GVariant *parameter, gpointer application) {
	show_settings(GTK_WIDGET(gtk_application_get_active_window(application)), SETTINGS_PAGE_LASTFM);
}


static void quit(GSimpleAction *action, GVariant *parameter, gpointer application) {
	GtkWindow *window = gtk_application_get_active_window(application);
	if (window)
		gtk_window_close(window);
}


static void start_up(GApplication *application) {
	static const GActionEntry ENTRIES[] = {
		{"about", show_about, NULL, NULL, NULL, {0}},
		{"settings", open_settings, NULL, NULL, NULL, {0}},
		{"lastfm-settings", open_lastfm_settings, NULL, NULL, NULL, {0}},
		{"quit", quit, NULL, NULL, NULL, {0}},
	};
	static const struct {
		const char *action, *accelerators[3];
	} SHORTCUTS[] = {
		{"win.play-pause", {"<Control>p", "AudioPlay", NULL}},
		{"win.next", {"<Control>Right", "AudioNext", NULL}},
		{"win.previous", {"<Control>Left", "AudioPrev", NULL}},
		{"win.jump-back", {"<Shift>Left", NULL}},
		{"win.jump-forward", {"<Shift>Right", NULL}},
		{"win.shuffle", {"<Control>s", NULL}},
		{"win.search", {"<Control>f", NULL}},
		{"win.new-playlist", {"<Control>n", NULL}},
		{"win.new-dynamic", {"<Control><Shift>n", NULL}},
		{"win.back", {"<Alt>Left", NULL}},
		{"win.forward", {"<Alt>Right", NULL}},
		{"app.settings", {"<Control>comma", NULL}},
		{"app.quit", {"<Control>q", NULL}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(application), ENTRIES, G_N_ELEMENTS(ENTRIES), application);
	g_autofree char *icons = get_resource_path("icons");
	gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(gdk_display_get_default()), icons);
	apply_style();
	for (guint index = 0; index < G_N_ELEMENTS(SHORTCUTS); index++)
		gtk_application_set_accels_for_action(GTK_APPLICATION(application), SHORTCUTS[index].action,
			SHORTCUTS[index].accelerators);
}


/* Crimson Music opens no files, so a second launch only brings the running window forward */
static void open_files(GApplication *application, GFile **files, int count, const char *hint) {
	activate(application);
}


const ApplicationSetup APPLICATION_SETUP = {
	.identity = {"io.github.CrimsonOrg.CrimsonMusic", TEXT_APPLICATION_NAME, "crimson-music", APPLICATION_VERSION},
	.start_up = start_up,
	.activate = activate,
	.open = open_files,
};

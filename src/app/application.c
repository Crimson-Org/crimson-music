#include <adwaita.h>

#include "lang/en.h"
#include "app/application.h"
#include "common/themes/themes.h"
#include "app/settings-dialog.h"
#include "app/settings.h"
#include "window/window.h"


static void activate(GApplication *application) {
	GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(application));
	if (!window)
		window = get_gtk_window(create_window(GTK_APPLICATION(application)));
	gtk_window_present(window);
}


static void open_settings(GtkWidget *window) {
	show_settings(window, NULL);
}


static void open_lastfm_settings(GSimpleAction *action, GVariant *parameter, gpointer application) {
	show_settings(GTK_WIDGET(gtk_application_get_active_window(application)), SETTINGS_PAGE_LASTFM);
}


static void start_up(GApplication *application) {
	static const GActionEntry ENTRIES[] = {
		{"lastfm-settings", open_lastfm_settings, NULL, NULL, NULL, {0}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(application), ENTRIES, G_N_ELEMENTS(ENTRIES), application);
	apply_style();
	/* Only the title bar follows the Crimson themes, so it turns light with the light style */
	use_themes(DEFAULT_LIGHT_THEME, DEFAULT_DARK_THEME, THEME_TITLE_BAR);
}


/* Crimson Music opens no files, so a second launch only brings the running window forward */
static void open_files(GApplication *application, GFile **files, int count, const char *hint) {
	activate(application);
}


static const Shortcut SHORTCUTS[] = {
	{"win.play-pause", {"<Control>p", "AudioPlay", NULL}},
	{"win.next", {"<Control>Right", "AudioNext", NULL}},
	{"win.previous", {"<Control>Left", "AudioPrev", NULL}},
	{"win.jump-back", {"<Shift>Left", NULL}},
	{"win.jump-forward", {"<Shift>Right", NULL}},
	{"win.shuffle", {"<Control>s", NULL}},
	{"win.search", {"<Control>f", NULL}},
	{"win.new-playlist", {"<Control>n", NULL}},
	{"win.new-dynamic", {"<Control><Shift>n", NULL}},
	{"win.back", {"<Alt>Left", "Back", NULL}},
	{"win.forward", {"<Alt>Right", "Forward", NULL}},
};

const ApplicationSetup APPLICATION_SETUP = {
	.identity = {"io.github.CrimsonOrg.CrimsonMusic", TEXT_APPLICATION_NAME, "crimson-music", APPLICATION_VERSION},
	.start_up = start_up,
	.activate = activate,
	.open = open_files,
	.show_settings = open_settings,
	.shortcuts = SHORTCUTS,
	.shortcut_count = G_N_ELEMENTS(SHORTCUTS),
	.closes_dialogs_outside = TRUE,
};

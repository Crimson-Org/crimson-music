#include <string.h>

#include "common/identity.h"
#include "mpris.h"

#define OBJECT_PATH "/org/mpris/MediaPlayer2"
#define BUS_NAME_PREFIX "org.mpris.MediaPlayer2."
#define ROOT_INTERFACE "org.mpris.MediaPlayer2"
#define PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"
#define TRACK_PATH "/io/github/CrimsonOrg/CrimsonMusic/track/%u"
#define NO_TRACK_PATH "/org/mpris/MediaPlayer2/TrackList/NoTrack"

static const char INTERFACES[] =
	"<node>"
	"<interface name='org.mpris.MediaPlayer2'>"
	"<method name='Raise'/>"
	"<method name='Quit'/>"
	"<property name='CanQuit' type='b' access='read'/>"
	"<property name='CanRaise' type='b' access='read'/>"
	"<property name='HasTrackList' type='b' access='read'/>"
	"<property name='Identity' type='s' access='read'/>"
	"<property name='DesktopEntry' type='s' access='read'/>"
	"<property name='SupportedUriSchemes' type='as' access='read'/>"
	"<property name='SupportedMimeTypes' type='as' access='read'/>"
	"</interface>"
	"<interface name='org.mpris.MediaPlayer2.Player'>"
	"<method name='Next'/>"
	"<method name='Previous'/>"
	"<method name='Pause'/>"
	"<method name='PlayPause'/>"
	"<method name='Stop'/>"
	"<method name='Play'/>"
	"<method name='Seek'><arg type='x' direction='in'/></method>"
	"<method name='SetPosition'><arg type='o' direction='in'/><arg type='x' direction='in'/></method>"
	"<method name='OpenUri'><arg type='s' direction='in'/></method>"
	"<signal name='Seeked'><arg type='x'/></signal>"
	"<property name='PlaybackStatus' type='s' access='read'/>"
	"<property name='Rate' type='d' access='read'/>"
	"<property name='Metadata' type='a{sv}' access='read'/>"
	"<property name='Volume' type='d' access='read'/>"
	"<property name='Position' type='x' access='read'/>"
	"<property name='MinimumRate' type='d' access='read'/>"
	"<property name='MaximumRate' type='d' access='read'/>"
	"<property name='CanGoNext' type='b' access='read'/>"
	"<property name='CanGoPrevious' type='b' access='read'/>"
	"<property name='CanPlay' type='b' access='read'/>"
	"<property name='CanPause' type='b' access='read'/>"
	"<property name='CanSeek' type='b' access='read'/>"
	"<property name='CanControl' type='b' access='read'/>"
	"</interface>"
	"</node>";

struct Mpris {
	GtkWindow *window;
	GDBusConnection *bus;
	CrimsonSong *song;
	/* Each song gets a track path of its own, so the desktop sees a change even when the same song plays again */
	guint track;
	gboolean is_playing;
	gint64 position;
	/* The desktop has read the player's state, so it knows the player and will hear a claim */
	gboolean is_known;
};


static void run_window_action(Mpris *mpris, const char *name) {
	gtk_widget_activate_action(GTK_WIDGET(mpris->window), name, NULL);
}


/* Lengths are kept as the minutes and seconds the list shows, such as 3:42 or 1:02:03 */
static gint64 parse_length(const char *duration) {
	if (!duration || !*duration)
		return 0;
	g_auto(GStrv) parts = g_strsplit(duration, ":", -1);
	gint64 seconds = 0;
	for (guint index = 0; parts[index]; index++)
		seconds = seconds * 60 + g_ascii_strtoll(parts[index], NULL, 10);
	return seconds * G_USEC_PER_SEC;
}


/* Covers on disk are named by their path and covers online by their address */
static char *get_art_address(const char *artwork) {
	if (!artwork || !*artwork)
		return NULL;
	if (g_path_is_absolute(artwork))
		return g_filename_to_uri(artwork, NULL, NULL);
	return g_strdup(artwork);
}


static GVariant *create_metadata(Mpris *mpris) {
	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
	if (!mpris->song) {
		g_variant_builder_add(&builder, "{sv}", "mpris:trackid", g_variant_new_object_path(NO_TRACK_PATH));
		return g_variant_builder_end(&builder);
	}
	CrimsonSong *song = mpris->song;
	g_autofree char *track = g_strdup_printf(TRACK_PATH, mpris->track);
	g_variant_builder_add(&builder, "{sv}", "mpris:trackid", g_variant_new_object_path(track));
	g_variant_builder_add(&builder, "{sv}", "xesam:title", g_variant_new_string(song->title ? song->title : ""));
	const char *artists[] = {song->artist ? song->artist : "", NULL};
	g_variant_builder_add(&builder, "{sv}", "xesam:artist", g_variant_new_strv(artists, -1));
	if (song->album && *song->album)
		g_variant_builder_add(&builder, "{sv}", "xesam:album", g_variant_new_string(song->album));
	gint64 length = parse_length(song->duration);
	if (length > 0)
		g_variant_builder_add(&builder, "{sv}", "mpris:length", g_variant_new_int64(length));
	g_autofree char *art = get_art_address(song->artwork);
	if (art)
		g_variant_builder_add(&builder, "{sv}", "mpris:artUrl", g_variant_new_string(art));
	return g_variant_builder_end(&builder);
}


static const char *get_playback_status(Mpris *mpris) {
	return !mpris->song ? "Stopped" : mpris->is_playing ? "Playing" : "Paused";
}


static GVariant *read_root_property(GDBusConnection *bus, const char *sender, const char *path,
	const char *interface, const char *property, GError **error, gpointer data) {
	const char *const none[] = {NULL};
	if (g_str_equal(property, "CanQuit") || g_str_equal(property, "CanRaise"))
		return g_variant_new_boolean(TRUE);
	if (g_str_equal(property, "HasTrackList"))
		return g_variant_new_boolean(FALSE);
	if (g_str_equal(property, "Identity"))
		return g_variant_new_string(get_application_identity()->name);
	if (g_str_equal(property, "DesktopEntry"))
		return g_variant_new_string(get_application_identity()->id);
	if (g_str_equal(property, "SupportedUriSchemes") || g_str_equal(property, "SupportedMimeTypes"))
		return g_variant_new_strv(none, 0);
	return NULL;
}


static gboolean claim_media_keys(gpointer data);


static GVariant *read_player_property(GDBusConnection *bus, const char *sender, const char *path,
	const char *interface, const char *property, GError **error, gpointer data) {
	Mpris *mpris = data;
	if (g_str_equal(property, "PlaybackStatus") && !mpris->is_known) {
		mpris->is_known = TRUE;
		g_idle_add(claim_media_keys, mpris);
	}
	if (g_str_equal(property, "PlaybackStatus"))
		return g_variant_new_string(get_playback_status(mpris));
	if (g_str_equal(property, "Rate") || g_str_equal(property, "MinimumRate")
		|| g_str_equal(property, "MaximumRate") || g_str_equal(property, "Volume"))
		return g_variant_new_double(1);
	if (g_str_equal(property, "Metadata"))
		return create_metadata(mpris);
	if (g_str_equal(property, "Position"))
		return g_variant_new_int64(mpris->position);
	if (g_str_equal(property, "CanPlay"))
		return g_variant_new_boolean(mpris->song != NULL);
	if (g_str_equal(property, "CanSeek"))
		return g_variant_new_boolean(FALSE);
	if (g_str_has_prefix(property, "Can"))
		return g_variant_new_boolean(TRUE);
	return NULL;
}


static void answer_root_call(GDBusConnection *bus, const char *sender, const char *path, const char *interface,
	const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
	Mpris *mpris = data;
	if (g_str_equal(method, "Raise"))
		gtk_window_present(mpris->window);
	else if (g_str_equal(method, "Quit"))
		gtk_window_close(mpris->window);
	g_dbus_method_invocation_return_value(invocation, NULL);
}


/* Play and Pause only switch when the player is the other way, since the window has one action for both */
static void answer_player_call(GDBusConnection *bus, const char *sender, const char *path, const char *interface,
	const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
	Mpris *mpris = data;
	gboolean is_switch = g_str_equal(method, "PlayPause")
		|| (g_str_equal(method, "Play") && !mpris->is_playing && mpris->song)
		|| ((g_str_equal(method, "Pause") || g_str_equal(method, "Stop")) && mpris->is_playing);
	if (is_switch)
		run_window_action(mpris, "win.play-pause");
	else if (g_str_equal(method, "Next"))
		run_window_action(mpris, "win.next");
	else if (g_str_equal(method, "Previous"))
		run_window_action(mpris, "win.previous");
	g_dbus_method_invocation_return_value(invocation, NULL);
}


static void announce_properties(Mpris *mpris, const char *name, GVariant *value) {
	if (!mpris->bus)
		return;
	GVariantBuilder changed;
	g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&changed, "{sv}", name, value);
	if (g_str_equal(name, "Metadata"))
		g_variant_builder_add(&changed, "{sv}", "CanPlay", g_variant_new_boolean(mpris->song != NULL));
	g_dbus_connection_emit_signal(mpris->bus, NULL, OBJECT_PATH, "org.freedesktop.DBus.Properties",
		"PropertiesChanged", g_variant_new("(sa{sv}as)", PLAYER_INTERFACE, &changed, NULL), NULL);
}


/* Plasma sends the media keys to the last player that started playing and keeps it while all are paused, so a
 * paused Crimson Music in the window the listener is using says it started and stopped, which makes it the one the
 * keys reach unless another player is playing */
static gboolean claim_media_keys(gpointer data) {
	Mpris *mpris = data;
	if (!mpris->song || mpris->is_playing || !gtk_window_is_active(mpris->window))
		return G_SOURCE_REMOVE;
	announce_properties(mpris, "PlaybackStatus", g_variant_new_string("Playing"));
	announce_properties(mpris, "PlaybackStatus", g_variant_new_string(get_playback_status(mpris)));
	return G_SOURCE_REMOVE;
}


static void notice_focus(GtkWindow *window, GParamSpec *property, Mpris *mpris) {
	if (mpris->is_known)
		claim_media_keys(mpris);
}


Mpris *create_mpris(GApplication *application, GtkWindow *window) {
	Mpris *mpris = g_new0(Mpris, 1);
	mpris->window = window;
	mpris->bus = g_application_get_dbus_connection(application);
	if (!mpris->bus)
		return mpris;
	g_signal_connect(window, "notify::is-active", G_CALLBACK(notice_focus), mpris);
	g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(INTERFACES, NULL);
	static const GDBusInterfaceVTable ROOT_TABLE = {answer_root_call, read_root_property, NULL, {0}};
	static const GDBusInterfaceVTable PLAYER_TABLE = {answer_player_call, read_player_property, NULL, {0}};
	g_dbus_connection_register_object(mpris->bus, OBJECT_PATH, node->interfaces[0], &ROOT_TABLE, mpris, NULL, NULL);
	g_dbus_connection_register_object(mpris->bus, OBJECT_PATH, node->interfaces[1], &PLAYER_TABLE, mpris, NULL,
		NULL);
	g_autofree char *name = g_strconcat(BUS_NAME_PREFIX, get_application_identity()->slug, NULL);
	g_strdelimit(name + strlen(BUS_NAME_PREFIX), "-", '_');
	g_bus_own_name_on_connection(mpris->bus, name, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, NULL, NULL, NULL);
	return mpris;
}


void set_mpris_song(Mpris *mpris, CrimsonSong *song) {
	if (song == mpris->song)
		return;
	g_set_object(&mpris->song, song);
	mpris->track++;
	mpris->position = 0;
	announce_properties(mpris, "Metadata", create_metadata(mpris));
	announce_properties(mpris, "PlaybackStatus", g_variant_new_string(get_playback_status(mpris)));
}


void set_mpris_playing(Mpris *mpris, gboolean is_playing) {
	if (is_playing == mpris->is_playing)
		return;
	mpris->is_playing = is_playing;
	announce_properties(mpris, "PlaybackStatus", g_variant_new_string(get_playback_status(mpris)));
}


void set_mpris_position(Mpris *mpris, double seconds) {
	mpris->position = (gint64)(seconds * G_USEC_PER_SEC);
}

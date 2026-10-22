#include "../lang/en.h"
#include "common/identity.h"
#include "tray.h"

#define WHEEL_NOTCH 120
#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define ITEM_PATH "/StatusNotifierItem"
#define MENU_PATH "/MenuBar"
#define ICON_SIZE 22
#define SYMBOLIC_FOLDER "icons/hicolor/symbolic/apps"
#define SYMBOLIC_SUFFIX "-symbolic"
#define DBUSMENU_VERSION 3
#define ROOT_ID 0

/* Menu entries by their id in the menu protocol */
enum {
	MENU_PLAY_PAUSE = 1,
	MENU_NEXT,
	MENU_PREVIOUS,
	MENU_SEPARATOR,
	MENU_QUIT,
};

static const char ITEM_INTERFACE[] =
	"<node><interface name='org.kde.StatusNotifierItem'>"
	"<property name='Category' type='s' access='read'/>"
	"<property name='Id' type='s' access='read'/>"
	"<property name='Title' type='s' access='read'/>"
	"<property name='Status' type='s' access='read'/>"
	"<property name='IconName' type='s' access='read'/>"
	"<property name='IconPixmap' type='a(iiay)' access='read'/>"
	"<property name='IconThemePath' type='s' access='read'/>"
	"<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
	"<property name='ItemIsMenu' type='b' access='read'/>"
	"<property name='Menu' type='o' access='read'/>"
	"<method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
	"<method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
	"<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
	"<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
	"<signal name='NewToolTip'/>"
	"</interface></node>";

static const char MENU_INTERFACE[] =
	"<node><interface name='com.canonical.dbusmenu'>"
	"<property name='Version' type='u' access='read'/>"
	"<property name='TextDirection' type='s' access='read'/>"
	"<property name='Status' type='s' access='read'/>"
	"<property name='IconThemePath' type='as' access='read'/>"
	"<method name='GetLayout'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
	"<arg type='as' direction='in'/><arg type='u' direction='out'/><arg type='(ia{sv}av)' direction='out'/></method>"
	"<method name='GetGroupProperties'><arg type='ai' direction='in'/><arg type='as' direction='in'/>"
	"<arg type='a(ia{sv})' direction='out'/></method>"
	"<method name='GetProperty'><arg type='i' direction='in'/><arg type='s' direction='in'/>"
	"<arg type='v' direction='out'/></method>"
	"<method name='Event'><arg type='i' direction='in'/><arg type='s' direction='in'/>"
	"<arg type='v' direction='in'/><arg type='u' direction='in'/></method>"
	"<method name='EventGroup'><arg type='a(isvu)' direction='in'/><arg type='ai' direction='out'/></method>"
	"<method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
	"<method name='AboutToShowGroup'><arg type='ai' direction='in'/><arg type='ai' direction='out'/>"
	"<arg type='ai' direction='out'/></method>"
	"<signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
	"</interface></node>";

struct Tray {
	GtkWindow *window;
	GDBusConnection *bus;
	GVariant *pixmap;
	char *theme_path;
	gboolean is_playing, is_shown;
	guint revision;
};


static void run_window_action(Tray *tray, const char *name) {
	gtk_widget_activate_action(GTK_WIDGET(tray->window), name, NULL);
}


static GVariant *describe_entry(int id, Tray *tray) {
	GVariantBuilder properties;
	g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
	static const char *const LABELS[] = {
		[MENU_NEXT] = TEXT_NEXT, [MENU_PREVIOUS] = TEXT_PREVIOUS, [MENU_QUIT] = TEXT_QUIT_APPLICATION,
	};
	if (id == MENU_SEPARATOR)
		g_variant_builder_add(&properties, "{sv}", "type", g_variant_new_string("separator"));
	else if (id == MENU_PLAY_PAUSE)
		g_variant_builder_add(&properties, "{sv}", "label", g_variant_new_string(tray->is_playing ? TEXT_PAUSE : TEXT_PLAY));
	else if (id != ROOT_ID)
		g_variant_builder_add(&properties, "{sv}", "label", g_variant_new_string(LABELS[id]));
	if (id == ROOT_ID)
		g_variant_builder_add(&properties, "{sv}", "children-display", g_variant_new_string("submenu"));
	GVariantBuilder children;
	g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
	for (int child = MENU_PLAY_PAUSE; id == ROOT_ID && child <= MENU_QUIT; child++)
		g_variant_builder_add(&children, "v", describe_entry(child, tray));
	return g_variant_new("(ia{sv}av)", id, &properties, &children);
}


static void run_menu_entry(Tray *tray, int id) {
	if (id == MENU_PLAY_PAUSE)
		run_window_action(tray, "win.play-pause");
	else if (id == MENU_NEXT)
		run_window_action(tray, "win.next");
	else if (id == MENU_PREVIOUS)
		run_window_action(tray, "win.previous");
	else if (id == MENU_QUIT)
		gtk_window_close(tray->window);
}


static void answer_menu_call(GDBusConnection *bus, const char *sender, const char *path, const char *interface,
	const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
	Tray *tray = data;
	if (g_str_equal(method, "GetLayout")) {
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", tray->revision,
			describe_entry(ROOT_ID, tray)));
	} else if (g_str_equal(method, "Event")) {
		int id = 0;
		const char *event = NULL;
		g_variant_get(parameters, "(i&svu)", &id, &event, NULL, NULL);
		if (g_str_equal(event, "clicked"))
			run_menu_entry(tray, id);
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (g_str_equal(method, "AboutToShow")) {
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
	} else if (g_str_equal(method, "GetGroupProperties")) {
		g_dbus_method_invocation_return_value(invocation, g_variant_new_parsed("(@a(ia{sv}) [],)"));
	} else {
		g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.NotSupported", method);
	}
}


static GVariant *read_menu_property(GDBusConnection *bus, const char *sender, const char *path,
	const char *interface, const char *name, GError **error, gpointer data) {
	if (g_str_equal(name, "Version"))
		return g_variant_new_uint32(DBUSMENU_VERSION);
	if (g_str_equal(name, "TextDirection"))
		return g_variant_new_string("ltr");
	if (g_str_equal(name, "Status"))
		return g_variant_new_string("normal");
	return g_variant_new_strv(NULL, 0);
}


/* A click on the icon brings the window forward, or hides it when it already is */
static void toggle_window(Tray *tray) {
	if (gtk_widget_get_visible(GTK_WIDGET(tray->window)) && gtk_window_is_active(tray->window))
		gtk_widget_set_visible(GTK_WIDGET(tray->window), FALSE);
	else
		gtk_window_present(tray->window);
}


/* Scrolling over the icon turns the volume, one step a notch, where panels send a notch as 120 or as 1 */
static void scroll_volume(Tray *tray, GVariant *parameters) {
	int delta = 0;
	const char *orientation = NULL;
	g_variant_get(parameters, "(i&s)", &delta, &orientation);
	if (!delta || g_ascii_strcasecmp(orientation, "vertical") != 0)
		return;
	int steps = delta / WHEEL_NOTCH;
	if (!steps)
		steps = delta > 0 ? 1 : -1;
	gtk_widget_activate_action(GTK_WIDGET(tray->window), "win.volume-step", "i", steps);
}


static void answer_item_call(GDBusConnection *bus, const char *sender, const char *path, const char *interface,
	const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
	Tray *tray = data;
	if (g_str_equal(method, "Activate"))
		toggle_window(tray);
	else if (g_str_equal(method, "SecondaryActivate"))
		run_window_action(tray, "win.play-pause");
	else if (g_str_equal(method, "Scroll"))
		scroll_volume(tray, parameters);
	g_dbus_method_invocation_return_value(invocation, NULL);
}


static GVariant *read_item_property(GDBusConnection *bus, const char *sender, const char *path,
	const char *interface, const char *name, GError **error, gpointer data) {
	Tray *tray = data;
	const ApplicationIdentity *identity = get_application_identity();
	if (g_str_equal(name, "Category"))
		return g_variant_new_string("ApplicationStatus");
	if (g_str_equal(name, "Id"))
		return g_variant_new_string(identity->slug);
	if (g_str_equal(name, "Title"))
		return g_variant_new_string(identity->name);
	if (g_str_equal(name, "Status"))
		return g_variant_new_string("Active");
	if (g_str_equal(name, "IconName"))
		return g_variant_new_take_string(g_strconcat(identity->id, SYMBOLIC_SUFFIX, NULL));
	if (g_str_equal(name, "IconThemePath"))
		return g_variant_new_string(tray->theme_path);
	if (g_str_equal(name, "IconPixmap"))
		return g_variant_ref(tray->pixmap);
	if (g_str_equal(name, "ToolTip"))
		return g_variant_new("(s@a(iiay)ss)", "", tray->pixmap, identity->name, "");
	if (g_str_equal(name, "ItemIsMenu"))
		return g_variant_new_boolean(FALSE);
	return g_variant_new_object_path(MENU_PATH);
}


static char *get_symbolic_name(void) {
	return g_strconcat(get_application_identity()->id, SYMBOLIC_SUFFIX, ".svg", NULL);
}


/* Panels draw tray icons in one colour that follows their theme, so the tray gets the symbolic icon, written
 * to the cache folder it reads themes from so it works before the program is installed */
static char *write_symbolic_icon(void) {
	g_autofree char *base = g_build_filename(g_get_user_cache_dir(), get_application_identity()->slug, "icons", NULL);
	g_autofree char *folder = g_build_filename(base, "hicolor", "symbolic", "apps", NULL);
	g_autofree char *name = get_symbolic_name();
	g_autofree char *resource = g_strconcat("icons/symbolic/apps/", name, NULL);
	g_autofree char *resource_path = get_resource_path(resource);
	g_autoptr(GBytes) icon = g_resources_lookup_data(resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
	g_autofree char *path = g_build_filename(folder, name, NULL);
	if (icon && g_mkdir_with_parents(folder, 0700) == 0)
		g_file_set_contents(path, g_bytes_get_data(icon, NULL), (gssize)g_bytes_get_size(icon), NULL);
	return g_steal_pointer(&base);
}


/* The same symbolic icon as pixels, in a light grey that suits dark panels, for hosts that ignore the theme path */
static GVariant *create_pixmap(void) {
	g_autofree char *symbolic = get_symbolic_name();
	g_autofree char *name = g_strconcat("icons/symbolic/apps/", symbolic, NULL);
	g_autofree char *path = get_resource_path(name);
	g_autoptr(GdkPixbuf) image = gdk_pixbuf_new_from_resource_at_scale(path, ICON_SIZE, ICON_SIZE, TRUE, NULL);
	GVariantBuilder pixmaps;
	g_variant_builder_init(&pixmaps, G_VARIANT_TYPE("a(iiay)"));
	if (image && gdk_pixbuf_get_has_alpha(image)) {
		int width = gdk_pixbuf_get_width(image), height = gdk_pixbuf_get_height(image);
		int stride = gdk_pixbuf_get_rowstride(image);
		const guchar *source = gdk_pixbuf_read_pixels(image);
		gsize length = (gsize)width * (gsize)height * 4;
		guchar *pixels = g_malloc(length);
		/* The tray protocol wants alpha first, while the image holds it last */
		for (int row = 0; row < height; row++) {
			for (int column = 0; column < width; column++) {
				const guchar *from = source + row * stride + column * 4;
				guchar *to = pixels + ((gsize)row * width + column) * 4;
				to[0] = from[3];
				to[1] = from[0];
				to[2] = from[1];
				to[3] = from[2];
			}
		}
		GVariant *bytes = g_variant_new_from_data(G_VARIANT_TYPE_BYTESTRING, pixels, length, TRUE, g_free, pixels);
		g_variant_builder_add(&pixmaps, "(ii@ay)", width, height, bytes);
	}
	return g_variant_ref_sink(g_variant_builder_end(&pixmaps));
}


static void finish_registering(GObject *bus, GAsyncResult *result, gpointer data) {
	Tray *tray = data;
	g_autoptr(GVariant) answer = g_dbus_connection_call_finish(G_DBUS_CONNECTION(bus), result, NULL);
	tray->is_shown = answer != NULL;
}


static void register_item(GDBusConnection *bus, const char *name, const char *owner, gpointer data) {
	g_dbus_connection_call(bus, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME, "RegisterStatusNotifierItem",
		g_variant_new("(s)", g_dbus_connection_get_unique_name(bus)), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
		finish_registering, data);
}


static void forget_watcher(GDBusConnection *bus, const char *name, gpointer data) {
	Tray *tray = data;
	tray->is_shown = FALSE;
}


static void export_object(Tray *tray, const char *path, const char *xml, const GDBusInterfaceVTable *table) {
	g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(xml, NULL);
	g_dbus_connection_register_object(tray->bus, path, node->interfaces[0], table, tray, NULL, NULL);
}


Tray *create_tray(GApplication *application, GtkWindow *window) {
	Tray *tray = g_new0(Tray, 1);
	tray->window = window;
	tray->bus = g_application_get_dbus_connection(application);
	tray->pixmap = create_pixmap();
	tray->theme_path = write_symbolic_icon();
	if (!tray->bus)
		return tray;
	static const GDBusInterfaceVTable ITEM_TABLE = {answer_item_call, read_item_property, NULL, {0}};
	static const GDBusInterfaceVTable MENU_TABLE = {answer_menu_call, read_menu_property, NULL, {0}};
	export_object(tray, ITEM_PATH, ITEM_INTERFACE, &ITEM_TABLE);
	export_object(tray, MENU_PATH, MENU_INTERFACE, &MENU_TABLE);
	/* The panel can restart, and registering again each time its watcher appears keeps the icon there */
	g_bus_watch_name_on_connection(tray->bus, WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE, register_item,
		forget_watcher, tray, NULL);
	return tray;
}


void set_tray_playing(Tray *tray, gboolean is_playing) {
	if (tray->is_playing == is_playing || !tray->bus)
		return;
	tray->is_playing = is_playing;
	tray->revision++;
	g_dbus_connection_emit_signal(tray->bus, NULL, MENU_PATH, "com.canonical.dbusmenu", "LayoutUpdated",
		g_variant_new("(ui)", tray->revision, ROOT_ID), NULL);
}


gboolean is_tray_shown(Tray *tray) {
	return tray->is_shown;
}

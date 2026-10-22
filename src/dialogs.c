#include "dialogs.h"


/* The dialog spans the dimmed window around its box, so presses on it are watched before its own widgets take them,
 * and one outside the box closes it */
static gboolean notice_press(GtkEventControllerLegacy *controller, GdkEvent *event, gpointer data) {
	AdwDialog *dialog = ADW_DIALOG(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)));
	GtkWidget *content = adw_dialog_get_child(dialog);
	if (gdk_event_get_event_type(event) != GDK_BUTTON_PRESS || !content)
		return FALSE;
	GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(dialog)));
	double x = 0, y = 0, left = 0, top = 0;
	gdk_event_get_position(event, &x, &y);
	gtk_native_get_surface_transform(GTK_NATIVE(root), &left, &top);
	graphene_rect_t bounds;
	graphene_point_t point = GRAPHENE_POINT_INIT((float)(x - left), (float)(y - top));
	if (!gtk_widget_compute_bounds(content, root, &bounds) || graphene_rect_contains_point(&bounds, &point))
		return FALSE;
	adw_dialog_close(dialog);
	return TRUE;
}


void close_on_outside_click(AdwDialog *dialog) {
	GtkEventController *controller = gtk_event_controller_legacy_new();
	gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
	g_signal_connect(controller, "event", G_CALLBACK(notice_press), NULL);
	gtk_widget_add_controller(GTK_WIDGET(dialog), controller);
}

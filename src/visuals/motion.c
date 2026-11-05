#include "common/app/startup.h"
#include "visuals/motion.h"

#define ENTERING_CLASS "entering"


static gboolean add_entering_class(gpointer data) {
	gtk_widget_add_css_class(data, ENTERING_CLASS);
	return G_SOURCE_REMOVE;
}


/* GTK starts a CSS animation only when its class arrives in a later frame than the one it left */
void replay_entrance(GtkWidget *widget) {
	gtk_widget_remove_css_class(widget, ENTERING_CLASS);
	run_after_frame(widget, add_entering_class, widget, NULL);
}


void show_dragged_widget(GtkDragSource *source, GtkWidget *widget, double x, double y) {
	g_autoptr(GdkPaintable) icon = gtk_widget_paintable_new(widget);
	gtk_drag_source_set_icon(source, icon, (int)x, (int)y);
}

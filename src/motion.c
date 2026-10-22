#include "common/startup.h"
#include "motion.h"

#define ENTERING_CLASS "entering"


static gboolean add_entering_class(gpointer data) {
	gtk_widget_add_css_class(data, ENTERING_CLASS);
	g_object_unref(data);
	return G_SOURCE_REMOVE;
}


/* GTK starts a CSS animation only when its class arrives in a later frame than the one it left */
void replay_entrance(GtkWidget *widget) {
	gtk_widget_remove_css_class(widget, ENTERING_CLASS);
	run_after_frame(widget, add_entering_class, g_object_ref(widget));
}

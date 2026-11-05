#include "app/dialog-layout.h"

/* A dialog takes no more than this share of its window's height */
#define WINDOW_SHARE 0.8


void fit_dialog_height(AdwDialog *dialog, GtkWidget *parent, int most) {
	GtkRoot *root = parent ? gtk_widget_get_root(parent) : NULL;
	int window_height = root ? gtk_widget_get_height(GTK_WIDGET(root)) : 0;
	int height = window_height > 0 ? MIN(most, (int)(window_height * WINDOW_SHARE)) : most;
	adw_dialog_set_content_height(dialog, height);
}

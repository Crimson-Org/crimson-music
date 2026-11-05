#pragma once

#include <gtk/gtk.h>

/* Plays the widget's entrance animation from assets/style.css again, such as a slide for a new song */
void replay_entrance(GtkWidget *widget);
/* A drag shows the widget it started on under the pointer, held where it was grabbed */
void show_dragged_widget(GtkDragSource *source, GtkWidget *widget, double x, double y);

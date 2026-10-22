#pragma once

#include <adwaita.h>

#include "dynamic.h"

/* Takes the finished rules, which are the edited copy and belong to the receiver from then on */
typedef void (*DynamicSaved)(DynamicRules *rules, gpointer data);

/* Opens the editor for new rules, or a copy of these, with the pool of liked songs it previews against */
void show_dynamic_editor(GtkWidget *parent, const DynamicRules *rules, GListModel *pool, DynamicSaved saved,
	gpointer data);
/* Shows the preview again after songs were gathered or tagged */
void update_dynamic_editor(void);

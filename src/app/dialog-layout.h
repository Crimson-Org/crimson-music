#pragma once

#include <adwaita.h>

#include "common/widgets/spacing.h"

/* Dialogs whose content scrolls, such as Information and the dynamic playlist editor, grow no taller than this */
#define TALL_DIALOG_HEIGHT 880
/* The gap between the sections of a dialog's page */
#define DIALOG_SECTION_GAP SPACING_LARGE

/* Gives a dialog whose content scrolls a height that leaves its window in sight around it, and grows with a taller
 * window up to most, so it never nearly fills a small window nor wastes a large one */
void fit_dialog_height(AdwDialog *dialog, GtkWidget *parent, int most);

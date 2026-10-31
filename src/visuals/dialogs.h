#pragma once

#include <adwaita.h>

/* A press anywhere outside the dialog's own box closes it, as clicking away from a popover does */
void close_on_outside_click(AdwDialog *dialog);

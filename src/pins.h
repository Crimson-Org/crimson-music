#pragma once

#include "library.h"

/* Artists and albums pinned to the sidebar, kept in the config folder since YouTube Music has no such list */
GPtrArray *load_pins(void);
void save_pins(GPtrArray *pins);

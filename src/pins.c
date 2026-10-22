#include "common/key-store.h"
#include "pins.h"

#define PINS_FILE "pins.ini"
#define GROUP_FORMAT "pin-%u"


GPtrArray *load_pins(void) {
	g_autofree char *path = get_config_file_path(PINS_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	GPtrArray *pins = g_ptr_array_new_with_free_func(free_card);
	for (guint index = 0;; index++) {
		g_autofree char *group = g_strdup_printf(GROUP_FORMAT, index);
		if (!g_key_file_has_group(file, group))
			break;
		Card *pin = g_new0(Card, 1);
		pin->id = g_key_file_get_string(file, group, "id", NULL);
		pin->title = g_key_file_get_string(file, group, "title", NULL);
		pin->subtitle = g_key_file_get_string(file, group, "subtitle", NULL);
		pin->artwork = g_key_file_get_string(file, group, "artwork", NULL);
		if (pin->id && pin->title)
			g_ptr_array_add(pins, pin);
		else
			free_card(pin);
	}
	return pins;
}


void save_pins(GPtrArray *pins) {
	g_autofree char *path = get_config_file_path(PINS_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	for (guint index = 0; index < pins->len; index++) {
		Card *pin = g_ptr_array_index(pins, index);
		g_autofree char *group = g_strdup_printf(GROUP_FORMAT, index);
		g_key_file_set_string(file, group, "id", pin->id);
		g_key_file_set_string(file, group, "title", pin->title);
		g_key_file_set_string(file, group, "subtitle", pin->subtitle ? pin->subtitle : "");
		g_key_file_set_string(file, group, "artwork", pin->artwork ? pin->artwork : "");
	}
	save_key_file(file, path);
}

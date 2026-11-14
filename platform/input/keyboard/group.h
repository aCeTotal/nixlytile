#ifndef PLATFORM_INPUT_KEYBOARD_GROUP_H
#define PLATFORM_INPUT_KEYBOARD_GROUP_H

#include <wayland-server-core.h>
#include "platform/input/keyboard/keyboard.h"

// Merges member keyboards into one.
struct nx_keyboard_group {
	struct nx_keyboard keyboard;
	struct wl_list devices; // keyboard_group_device.link
	struct wl_list keys; // keyboard_group_key.link

	void *data;
};

struct nx_keyboard_group *nx_keyboard_group_create(void);

bool nx_keyboard_group_add_keyboard(struct nx_keyboard_group *group,
	struct nx_keyboard *keyboard);

void nx_keyboard_group_destroy(struct nx_keyboard_group *group);

#endif

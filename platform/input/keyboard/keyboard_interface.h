#ifndef PLATFORM_INPUT_KEYBOARD_KEYBOARD_INTERFACE_H
#define PLATFORM_INPUT_KEYBOARD_KEYBOARD_INTERFACE_H

#include <stdint.h>
#include "platform/input/keyboard/keyboard.h"

struct nx_keyboard_impl {
	const char *name;
	void (*led_update)(struct nx_keyboard *keyboard, uint32_t leds);
};

void nx_keyboard_init(struct nx_keyboard *keyboard,
	const struct nx_keyboard_impl *impl, const char *name);
// Releases held keys, then frees.
void nx_keyboard_finish(struct nx_keyboard *keyboard);

void nx_keyboard_notify_key(struct nx_keyboard *keyboard,
	struct nx_keyboard_key_event *event);
void nx_keyboard_notify_modifiers(struct nx_keyboard *keyboard,
	uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
	uint32_t group);

#endif

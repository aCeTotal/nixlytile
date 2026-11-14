#ifndef PLATFORM_INPUT_KEYBOARD_KEYBOARD_STATE_H
#define PLATFORM_INPUT_KEYBOARD_KEYBOARD_STATE_H

#include "platform/input/keyboard/keyboard.h"

// Evdev to xkb keycode offset.
#define EVDEV_XKB_OFFSET 8

static inline bool input_keyboard_modifiers_equal(const struct nx_keyboard_modifiers *a,
		const struct nx_keyboard_modifiers *b) {
	return a->depressed == b->depressed && a->latched == b->latched &&
		a->locked == b->locked && a->group == b->group;
}

void input_keyboard_key_update(struct nx_keyboard *keyboard,
	struct nx_keyboard_key_event *event);
// True when the modifiers changed.
bool input_keyboard_modifier_update(struct nx_keyboard *keyboard);
void input_keyboard_led_update(struct nx_keyboard *keyboard);
void input_keyboard_unset_keymap(struct nx_keyboard *kb);

#endif

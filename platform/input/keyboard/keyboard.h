#ifndef PLATFORM_INPUT_KEYBOARD_KEYBOARD_H
#define PLATFORM_INPUT_KEYBOARD_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include "platform/input/device/input_device.h"

#define NX_LED_COUNT 5

#define NX_MODIFIER_COUNT 8

enum nx_keyboard_modifier {
	NX_MODIFIER_SHIFT = 1 << 0,
	NX_MODIFIER_CAPS = 1 << 1,
	NX_MODIFIER_CTRL = 1 << 2,
	NX_MODIFIER_ALT = 1 << 3,
	NX_MODIFIER_MOD2 = 1 << 4,
	NX_MODIFIER_MOD3 = 1 << 5,
	NX_MODIFIER_LOGO = 1 << 6,
	NX_MODIFIER_MOD5 = 1 << 7,
};

#define NX_KEYBOARD_KEYS_CAP 32

struct nx_keyboard_impl;

struct nx_keyboard_modifiers {
	xkb_mod_mask_t depressed;
	xkb_mod_mask_t latched;
	xkb_mod_mask_t locked;
	xkb_layout_index_t group;
};

struct nx_keyboard {
	struct nx_input_device base;

	const struct nx_keyboard_impl *impl;
	struct nx_keyboard_group *group;

	size_t keymap_size;
	int keymap_fd;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	xkb_led_index_t led_indexes[NX_LED_COUNT];
	xkb_mod_index_t mod_indexes[NX_MODIFIER_COUNT];

	uint32_t leds;
	uint32_t keycodes[NX_KEYBOARD_KEYS_CAP];
	size_t num_keycodes;
	struct nx_keyboard_modifiers modifiers;

	struct {
		int32_t rate;
		int32_t delay;
	} repeat_info;

	struct {
		struct wl_signal key; // nx_keyboard_key_event, before xkb update
		struct wl_signal modifiers;
		struct wl_signal keymap;
		struct wl_signal repeat_info;
	} events;
};

struct nx_keyboard_key_event {
	uint32_t time_msec;
	uint32_t keycode;
	bool update_state; // apply to xkb state
	enum wl_keyboard_key_state state;
};

// Asserts the device is a keyboard.
struct nx_keyboard *nx_keyboard_from_input_device(
	struct nx_input_device *input_device);

bool nx_keyboard_set_keymap(struct nx_keyboard *kb,
	struct xkb_keymap *keymap);

bool nx_keyboard_keymaps_match(struct xkb_keymap *km1, struct xkb_keymap *km2);

// Rate in hertz, delay in milliseconds.
void nx_keyboard_set_repeat_info(struct nx_keyboard *kb, int32_t rate_hz,
	int32_t delay_ms);

// Bits: num, caps, scroll, compose, kana.
void nx_keyboard_led_update(struct nx_keyboard *keyboard, uint32_t leds);

// Depressed or latched modifier bits.
uint32_t nx_keyboard_get_modifiers(struct nx_keyboard *keyboard);

#endif

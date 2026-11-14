#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "platform/input/keyboard/keyboard_interface.h"
#include "platform/nested/seat/seat.h"
#include "platform/os/clock.h"

static void keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	close(fd);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	struct nx_keyboard *keyboard = data;
	int64_t time_msec = get_current_time_msec();

	uint32_t *keycode_ptr;
	wl_array_for_each(keycode_ptr, keys) {
		struct nx_keyboard_key_event event = {
			.keycode = *keycode_ptr,
			.state = WL_KEYBOARD_KEY_STATE_PRESSED,
			.time_msec = time_msec,
			.update_state = false,
		};
		nx_keyboard_notify_key(keyboard, &event);
	}
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface) {
	struct nx_keyboard *keyboard = data;
	int64_t time_msec = get_current_time_msec();

	while (keyboard->num_keycodes > 0) {
		struct nx_keyboard_key_event event = {
			.keycode = keyboard->keycodes[keyboard->num_keycodes - 1],
			.state = WL_KEYBOARD_KEY_STATE_RELEASED,
			.time_msec = time_msec,
			.update_state = false,
		};
		nx_keyboard_notify_key(keyboard, &event);
	}
}

static void keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	struct nx_keyboard *keyboard = data;
	struct nx_keyboard_key_event event = {
		.keycode = key,
		.state = state,
		.time_msec = time,
		.update_state = false,
	};
	nx_keyboard_notify_key(keyboard, &event);
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group) {
	struct nx_keyboard *keyboard = data;
	nx_keyboard_notify_modifiers(keyboard, mods_depressed, mods_latched,
		mods_locked, group);
}

static void keyboard_handle_repeat_info(void *data,
		struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_handle_keymap,
	.enter = keyboard_handle_enter,
	.leave = keyboard_handle_leave,
	.key = keyboard_handle_key,
	.modifiers = keyboard_handle_modifiers,
	.repeat_info = keyboard_handle_repeat_info,
};

static const struct nx_keyboard_impl keyboard_impl = {
	.name = "wl-keyboard",
};

void nested_keyboard_init(struct nx_wl_seat *seat) {
	assert(seat->wl_keyboard);

	char name[128] = {0};
	snprintf(name, sizeof(name), "wayland-keyboard-%s", seat->name);

	nx_keyboard_init(&seat->nx_keyboard, &keyboard_impl, name);
	wl_keyboard_add_listener(seat->wl_keyboard, &keyboard_listener,
		&seat->nx_keyboard);

	wl_signal_emit_mutable(&seat->backend->backend.events.new_input,
		&seat->nx_keyboard.base);
}

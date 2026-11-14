#ifndef PLATFORM_INPUT_SEAT_KEYBOARD_H
#define PLATFORM_INPUT_SEAT_KEYBOARD_H

#include <wayland-server-core.h>
#include "platform/input/keyboard/keyboard.h"

struct nx_seat;
struct nx_seat_client;
struct nx_seat_keyboard_grab;
struct nx_surface;

struct nx_keyboard_grab_interface {
	void (*enter)(struct nx_seat_keyboard_grab *grab,
			struct nx_surface *surface, const uint32_t keycodes[],
			size_t num_keycodes, const struct nx_keyboard_modifiers *modifiers);
	void (*clear_focus)(struct nx_seat_keyboard_grab *grab);
	void (*key)(struct nx_seat_keyboard_grab *grab, uint32_t time_msec,
			uint32_t key, uint32_t state);
	void (*modifiers)(struct nx_seat_keyboard_grab *grab,
			const struct nx_keyboard_modifiers *modifiers);
	void (*cancel)(struct nx_seat_keyboard_grab *grab);
};

struct nx_seat_keyboard_grab {
	const struct nx_keyboard_grab_interface *interface;
	struct nx_seat *seat;
	void *data;
};

struct nx_seat_keyboard_state {
	struct nx_seat *seat;
	struct nx_keyboard *keyboard;

	struct nx_seat_client *focused_client;
	struct nx_surface *focused_surface;

	struct nx_seat_keyboard_grab *grab;
	struct nx_seat_keyboard_grab *default_grab;

	struct {
		struct wl_signal focus_change; // nx_seat_keyboard_focus_change_event
	} events;

	struct wl_listener keyboard_destroy;
	struct wl_listener keyboard_keymap;
	struct wl_listener keyboard_repeat_info;
	struct wl_listener surface_destroy;
};

struct nx_seat_keyboard_focus_change_event {
	struct nx_seat *seat;
	struct nx_surface *old_surface, *new_surface;
};

void nx_seat_set_keyboard(struct nx_seat *seat, struct nx_keyboard *keyboard);
struct nx_keyboard *nx_seat_get_keyboard(struct nx_seat *seat);

// Direct sends, bypassing the grab.
void nx_seat_keyboard_enter(struct nx_seat *seat,
		struct nx_surface *surface, const uint32_t keycodes[], size_t num_keycodes,
		const struct nx_keyboard_modifiers *modifiers);
void nx_seat_keyboard_clear_focus(struct nx_seat *seat);
void nx_seat_keyboard_send_key(struct nx_seat *seat, uint32_t time_msec,
		uint32_t key, uint32_t state);
void nx_seat_keyboard_send_modifiers(struct nx_seat *seat,
		const struct nx_keyboard_modifiers *modifiers);

// Grab-aware notifications.
void nx_seat_keyboard_notify_enter(struct nx_seat *seat,
		struct nx_surface *surface, const uint32_t keycodes[], size_t num_keycodes,
		const struct nx_keyboard_modifiers *modifiers);
void nx_seat_keyboard_notify_clear_focus(struct nx_seat *seat);
void nx_seat_keyboard_notify_key(struct nx_seat *seat, uint32_t time_msec,
		uint32_t key, uint32_t state);
void nx_seat_keyboard_notify_modifiers(struct nx_seat *seat,
		const struct nx_keyboard_modifiers *modifiers);

void nx_seat_keyboard_start_grab(struct nx_seat *seat,
		struct nx_seat_keyboard_grab *grab);
void nx_seat_keyboard_end_grab(struct nx_seat *seat);

#endif

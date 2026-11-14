#ifndef PLATFORM_NESTED_SEAT_SEAT_H
#define PLATFORM_NESTED_SEAT_SEAT_H

#include "platform/input/keyboard/keyboard.h"
#include "platform/input/pointer/pointer.h"
#include "platform/nested/backend.h"

struct nx_wl_output;

// One per output and host seat.
struct nx_wl_pointer {
	struct nx_pointer nx_pointer;

	struct nx_wl_seat *seat;
	struct nx_wl_output *output;

	enum wl_pointer_axis_source axis_source;
	int32_t axis_discrete;
	uint32_t fingers; // trackpad gesture
	enum wl_pointer_axis_relative_direction axis_relative_direction;

	struct wl_listener output_destroy;

	struct wl_list link; // nx_wl_seat.pointers
};

struct nx_wl_seat {
	char *name;
	struct wl_seat *wl_seat;
	uint32_t global_name;

	struct nx_wl_backend *backend;

	struct wl_keyboard *wl_keyboard;
	struct nx_keyboard nx_keyboard;

	struct wl_pointer *wl_pointer;
	struct nx_wl_pointer *active_pointer;
	struct wl_list pointers; // nx_wl_pointer.link

	struct zwp_pointer_gesture_swipe_v1 *gesture_swipe;
	struct zwp_pointer_gesture_pinch_v1 *gesture_pinch;
	struct zwp_pointer_gesture_hold_v1 *gesture_hold;
	struct zwp_relative_pointer_v1 *relative_pointer;

	struct wl_list link; // nx_wl_backend.seats
};

static inline struct nx_wl_pointer *nested_active_pointer(void *seat_data) {
	struct nx_wl_seat *seat = seat_data;
	return seat->active_pointer;
}

bool nested_seat_create(struct wl_seat *wl_seat, struct nx_wl_backend *wl,
	uint32_t global_name);
void nested_seat_destroy(struct nx_wl_seat *seat);

void nested_keyboard_init(struct nx_wl_seat *seat);

void nested_seat_init_pointer(struct nx_wl_seat *seat);
void nested_seat_finish_pointer(struct nx_wl_seat *seat);
void nested_pointer_create(struct nx_wl_seat *seat, struct nx_wl_output *output);
struct nx_wl_pointer *nested_pointer_for_output(struct nx_wl_output *output,
	const struct wl_pointer *wl_pointer);

extern const struct wl_pointer_listener nested_pointer_listener;
extern const struct zwp_relative_pointer_v1_listener nested_relative_pointer_listener;

void nested_gestures_init(struct nx_wl_seat *seat);
void nested_gestures_finish(struct nx_wl_seat *seat);

#endif

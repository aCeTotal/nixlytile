#ifndef PLATFORM_INPUT_POINTER_GESTURES_H
#define PLATFORM_INPUT_POINTER_GESTURES_H

#include <wayland-server-core.h>
#include "platform/input/seat/seat.h"

struct nx_pointer_gestures_v1 {
	struct wl_global *global;
	struct wl_list swipes; // wl_resource_get_link()
	struct wl_list pinches; // wl_resource_get_link()
	struct wl_list holds; // wl_resource_get_link()

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_pointer_gestures_v1 *nx_pointer_gestures_v1_create(
	struct wl_display *display);

void nx_pointer_gestures_v1_send_swipe_begin(
	struct nx_pointer_gestures_v1 *gestures,
	struct nx_seat *seat,
	uint32_t time_msec,
	uint32_t fingers);
void nx_pointer_gestures_v1_send_swipe_update(
	struct nx_pointer_gestures_v1 *gestures,
	struct nx_seat *seat,
	uint32_t time_msec,
	double dx,
	double dy);
void nx_pointer_gestures_v1_send_swipe_end(
	struct nx_pointer_gestures_v1 *gestures,
	struct nx_seat *seat,
	uint32_t time_msec,
	bool cancelled);

void nx_pointer_gestures_v1_send_pinch_begin(
	struct nx_pointer_gestures_v1 *gestures,
	struct nx_seat *seat,
	uint32_t time_msec,
	uint32_t fingers);
void nx_pointer_gestures_v1_send_pinch_update(
	struct nx_pointer_gestures_v1 *gestures,
	struct nx_seat *seat,
	uint32_t time_msec,
	double dx,
	double dy,
	double scale,
	double rotation);
void nx_pointer_gestures_v1_send_pinch_end(
	struct nx_pointer_gestures_v1 *gestures,
	struct nx_seat *seat,
	uint32_t time_msec,
	bool cancelled);

void nx_pointer_gestures_v1_send_hold_begin(
	struct nx_pointer_gestures_v1 *gestures,
	struct nx_seat *seat,
	uint32_t time_msec,
	uint32_t fingers);
void nx_pointer_gestures_v1_send_hold_end(
	struct nx_pointer_gestures_v1 *gestures,
	struct nx_seat *seat,
	uint32_t time_msec,
	bool cancelled);

#endif

#ifndef PLATFORM_INPUT_SEAT_POINTER_H
#define PLATFORM_INPUT_SEAT_POINTER_H

#include <wayland-server-core.h>
#include "platform/input/pointer/pointer.h"

struct nx_seat;
struct nx_seat_client;
struct nx_seat_pointer_grab;
struct nx_surface;

struct nx_pointer_grab_interface {
	void (*enter)(struct nx_seat_pointer_grab *grab,
			struct nx_surface *surface, double sx, double sy);
	void (*clear_focus)(struct nx_seat_pointer_grab *grab);
	void (*motion)(struct nx_seat_pointer_grab *grab, uint32_t time_msec,
			double sx, double sy);
	uint32_t (*button)(struct nx_seat_pointer_grab *grab, uint32_t time_msec,
			uint32_t button, enum wl_pointer_button_state state);
	void (*axis)(struct nx_seat_pointer_grab *grab, uint32_t time_msec,
			enum wl_pointer_axis orientation, double value,
			int32_t value_discrete, enum wl_pointer_axis_source source,
			enum wl_pointer_axis_relative_direction relative_direction);
	void (*frame)(struct nx_seat_pointer_grab *grab);
	void (*cancel)(struct nx_seat_pointer_grab *grab);
};

struct nx_seat_pointer_grab {
	const struct nx_pointer_grab_interface *interface;
	struct nx_seat *seat;
	void *data;
};

struct nx_seat_pointer_button {
	uint32_t button;
	size_t n_pressed;
};

struct nx_seat_pointer_state {
	struct nx_seat *seat;
	struct nx_seat_client *focused_client;
	struct nx_surface *focused_surface;
	double sx, sy;

	struct nx_seat_pointer_grab *grab;
	struct nx_seat_pointer_grab *default_grab;

	bool sent_axis_source;
	enum wl_pointer_axis_source cached_axis_source;

	struct nx_seat_pointer_button buttons[NX_POINTER_BUTTONS_CAP];
	size_t button_count;

	uint32_t grab_button;
	uint32_t grab_serial;

	struct wl_listener surface_destroy;
};

struct nx_seat_pointer_request_set_cursor_event {
	struct nx_seat_client *seat_client;
	struct nx_surface *surface;
	uint32_t serial;
	int32_t hotspot_x, hotspot_y;
};

// Direct sends, bypassing the grab.
void nx_seat_pointer_enter(struct nx_seat *seat,
		struct nx_surface *surface, double sx, double sy);
void nx_seat_pointer_clear_focus(struct nx_seat *seat);
void nx_seat_pointer_warp(struct nx_seat *seat, double sx, double sy);
void nx_seat_pointer_send_motion(struct nx_seat *seat, uint32_t time_msec,
		double sx, double sy);
uint32_t nx_seat_pointer_send_button(struct nx_seat *seat,
		uint32_t time_msec, uint32_t button, enum wl_pointer_button_state state);
void nx_seat_pointer_send_axis(struct nx_seat *seat, uint32_t time_msec,
		enum wl_pointer_axis orientation, double value,
		int32_t value_discrete, enum wl_pointer_axis_source source,
		enum wl_pointer_axis_relative_direction relative_direction);
void nx_seat_pointer_send_frame(struct nx_seat *seat);

// Grab-aware notifications.
void nx_seat_pointer_notify_enter(struct nx_seat *seat,
		struct nx_surface *surface, double sx, double sy);
void nx_seat_pointer_notify_clear_focus(struct nx_seat *seat);
void nx_seat_pointer_notify_motion(struct nx_seat *seat,
		uint32_t time_msec, double sx, double sy);
uint32_t nx_seat_pointer_notify_button(struct nx_seat *seat,
		uint32_t time_msec, uint32_t button, enum wl_pointer_button_state state);
void nx_seat_pointer_notify_axis(struct nx_seat *seat, uint32_t time_msec,
		enum wl_pointer_axis orientation, double value,
		int32_t value_discrete, enum wl_pointer_axis_source source,
		enum wl_pointer_axis_relative_direction relative_direction);
void nx_seat_pointer_notify_frame(struct nx_seat *seat);

void nx_seat_pointer_start_grab(struct nx_seat *seat,
		struct nx_seat_pointer_grab *grab);
void nx_seat_pointer_end_grab(struct nx_seat *seat);

bool nx_seat_validate_pointer_grab_serial(struct nx_seat *seat,
		struct nx_surface *origin, uint32_t serial);

// NULL for inert resources.
struct nx_seat_client *nx_seat_client_from_pointer_resource(
		struct wl_resource *resource);

#endif

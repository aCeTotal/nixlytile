#ifndef PLATFORM_INPUT_POINTER_CONSTRAINTS_H
#define PLATFORM_INPUT_POINTER_CONSTRAINTS_H

#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-protocols/pointer-constraints-unstable-v1-enum.h>
#include <pixman.h>
#include "platform/surface/compositor.h"
#include "platform/input/seat/seat.h"

enum nx_pointer_constraint_v1_type {
	NX_POINTER_CONSTRAINT_V1_LOCKED,
	NX_POINTER_CONSTRAINT_V1_CONFINED,
};

enum nx_pointer_constraint_v1_state_field {
	NX_POINTER_CONSTRAINT_V1_STATE_REGION = 1 << 0,
	NX_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT = 1 << 1,
};

struct nx_pointer_constraint_v1_state {
	uint32_t committed; // enum nx_pointer_constraint_v1_state_field
	pixman_region32_t region;

	// Locked pointers only.
	struct {
		bool enabled;
		double x, y;
	} cursor_hint;
};

struct nx_pointer_constraint_v1 {
	struct wl_resource *resource;
	struct nx_surface *surface;
	struct nx_seat *seat;
	enum zwp_pointer_constraints_v1_lifetime lifetime;
	enum nx_pointer_constraint_v1_type type;
	pixman_region32_t region; // input region clipped by current

	struct nx_pointer_constraint_v1_state current, pending;

	struct wl_list link; // nx_pointer_constraints_v1.constraints

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wl_listener surface_destroy;
		struct wl_listener seat_destroy;

		struct nx_surface_synced synced;

		bool destroying;
	};
};

struct nx_pointer_constraints_v1 {
	struct wl_global *global;
	struct wl_list constraints; // nx_pointer_constraint_v1.link

	struct {
		struct wl_signal new_constraint; // struct nx_pointer_constraint_v1
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_pointer_constraints_v1 *nx_pointer_constraints_v1_create(
	struct wl_display *display);

void nx_pointer_constraint_v1_send_activated(
	struct nx_pointer_constraint_v1 *constraint);
// Destroys oneshot constraints.
void nx_pointer_constraint_v1_send_deactivated(
	struct nx_pointer_constraint_v1 *constraint);

#endif

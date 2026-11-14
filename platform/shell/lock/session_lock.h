#ifndef PLATFORM_SHELL_LOCK_SESSION_LOCK_H
#define PLATFORM_SHELL_LOCK_SESSION_LOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include "platform/surface/compositor.h"

struct nx_session_lock_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal new_lock; // struct nx_session_lock_v1
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_session_lock_v1 {
	struct wl_resource *resource;

	struct wl_list surfaces; // nx_session_lock_surface_v1.link

	struct {
		struct wl_signal new_surface; // struct nx_session_lock_surface_v1
		struct wl_signal unlock;
		struct wl_signal destroy;
	} events;

	void *data;

	struct {
		bool locked_sent;
	};
};

struct nx_session_lock_surface_v1_state {
	uint32_t width, height;
	uint32_t configure_serial;
};

struct nx_session_lock_surface_v1 {
	struct wl_resource *resource;
	struct wl_list link; // nx_session_lock_v1.surfaces

	struct nx_output *output;
	struct nx_surface *surface;

	bool configured;
	struct wl_list configure_list; // lock_configure.link

	// The last acked configure.
	struct nx_session_lock_surface_v1_state acked;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct wl_listener output_destroy;
	};
};

struct nx_session_lock_manager_v1 *nx_session_lock_manager_v1_create(
	struct wl_display *display);

void nx_session_lock_v1_send_locked(struct nx_session_lock_v1 *lock);
void nx_session_lock_v1_destroy(struct nx_session_lock_v1 *lock);

uint32_t nx_session_lock_surface_v1_configure(
	struct nx_session_lock_surface_v1 *lock_surface,
	uint32_t width, uint32_t height);

#endif

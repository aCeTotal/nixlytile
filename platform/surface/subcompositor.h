#ifndef PLATFORM_SURFACE_SUBCOMPOSITOR_H
#define PLATFORM_SURFACE_SUBCOMPOSITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include "platform/surface/compositor.h"

// Applied on parent commit
struct nx_subsurface_parent_state {
	int32_t x, y;
	struct wl_list link;

	struct {
		struct nx_surface_synced *synced;
	};
};

struct nx_subsurface {
	struct wl_resource *resource;
	struct nx_surface *surface;
	struct nx_surface *parent;

	struct nx_subsurface_parent_state current, pending;

	uint32_t cached_seq;
	bool has_cache;

	bool synchronized;
	bool added;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct nx_surface_synced parent_synced;

		struct wl_listener surface_client_commit;
		struct wl_listener parent_destroy;
	};
};

struct nx_subcompositor;

struct nx_subcompositor *nx_subcompositor_create(struct wl_display *display);

#endif

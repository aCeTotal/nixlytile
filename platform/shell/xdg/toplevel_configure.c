#include <assert.h>
#include <stdlib.h>
#include "platform/geometry/edges.h"
#include "platform/log/log.h"
#include "platform/shell/xdg/role.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
// Flags, tiled edges, suspended.
#define MAX_STATES 9

static const struct {
	enum nx_xdg_toplevel_wm_capabilities cap;
	enum xdg_toplevel_wm_capabilities wire;
} wm_capabilities[] = {
	{ NX_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU, XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU },
	{ NX_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE, XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE },
	{ NX_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN, XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN },
	{ NX_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE, XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE },
};

static const struct {
	enum nx_edges edge;
	enum xdg_toplevel_state state;
} tiled_states[] = {
	{ NX_EDGE_LEFT, XDG_TOPLEVEL_STATE_TILED_LEFT },
	{ NX_EDGE_RIGHT, XDG_TOPLEVEL_STATE_TILED_RIGHT },
	{ NX_EDGE_TOP, XDG_TOPLEVEL_STATE_TILED_TOP },
	{ NX_EDGE_BOTTOM, XDG_TOPLEVEL_STATE_TILED_BOTTOM },
};

static void send_wm_capabilities(struct wl_resource *resource, uint32_t caps) {
	uint32_t wire[ARRAY_LEN(wm_capabilities)];
	size_t len = 0;
	for (size_t i = 0; i < ARRAY_LEN(wm_capabilities); i++) {
		if (caps & wm_capabilities[i].cap) {
			wire[len++] = wm_capabilities[i].wire;
		}
	}

	struct wl_array array = {
		.size = len * sizeof(wire[0]),
		.data = wire,
	};
	xdg_toplevel_send_wm_capabilities(resource, &array);
}

static void send_states(struct wl_resource *resource,
		const struct nx_xdg_toplevel_configure *configure) {
	uint32_t version = wl_resource_get_version(resource);
	uint32_t states[MAX_STATES];
	size_t len = 0;
	if (configure->maximized) {
		states[len++] = XDG_TOPLEVEL_STATE_MAXIMIZED;
	}
	if (configure->fullscreen) {
		states[len++] = XDG_TOPLEVEL_STATE_FULLSCREEN;
	}
	if (configure->resizing) {
		states[len++] = XDG_TOPLEVEL_STATE_RESIZING;
	}
	if (configure->activated) {
		states[len++] = XDG_TOPLEVEL_STATE_ACTIVATED;
	}
	uint32_t tiled = version >= XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION ?
		configure->tiled : 0;
	for (size_t i = 0; i < ARRAY_LEN(tiled_states); i++) {
		if (tiled & tiled_states[i].edge) {
			states[len++] = tiled_states[i].state;
		}
	}
	if (configure->suspended && version >= XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION) {
		states[len++] = XDG_TOPLEVEL_STATE_SUSPENDED;
	}

	struct wl_array array = {
		.size = len * sizeof(states[0]),
		.data = states,
	};
	xdg_toplevel_send_configure(resource, configure->width, configure->height, &array);
}

struct nx_xdg_toplevel_configure *shell_toplevel_send_configure(
		struct nx_xdg_toplevel *toplevel) {
	struct nx_xdg_toplevel_configure *configure = calloc(1, sizeof(*configure));
	if (configure == NULL) {
		nx_log(NX_ERROR, "Allocation failed");
		wl_resource_post_no_memory(toplevel->resource);
		return NULL;
	}
	*configure = toplevel->scheduled;

	uint32_t version = wl_resource_get_version(toplevel->resource);
	if ((configure->fields & NX_XDG_TOPLEVEL_CONFIGURE_BOUNDS) &&
			version >= XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION) {
		xdg_toplevel_send_configure_bounds(toplevel->resource,
			configure->bounds.width, configure->bounds.height);
	}
	if ((configure->fields & NX_XDG_TOPLEVEL_CONFIGURE_WM_CAPABILITIES) &&
			version >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION) {
		send_wm_capabilities(toplevel->resource, configure->wm_capabilities);
	}
	send_states(toplevel->resource, configure);

	toplevel->scheduled.fields = 0;
	return configure;
}

void shell_toplevel_ack_configure(struct nx_xdg_toplevel *toplevel,
		const struct nx_xdg_toplevel_configure *configure) {
	toplevel->pending.width = configure->width;
	toplevel->pending.height = configure->height;
}

uint32_t nx_xdg_toplevel_set_size(struct nx_xdg_toplevel *toplevel,
		int32_t width, int32_t height) {
	assert(width >= 0 && height >= 0);
	toplevel->scheduled.width = width;
	toplevel->scheduled.height = height;
	return nx_xdg_surface_schedule_configure(toplevel->base);
}

uint32_t nx_xdg_toplevel_set_activated(struct nx_xdg_toplevel *toplevel,
		bool activated) {
	toplevel->scheduled.activated = activated;
	return nx_xdg_surface_schedule_configure(toplevel->base);
}

uint32_t nx_xdg_toplevel_set_maximized(struct nx_xdg_toplevel *toplevel,
		bool maximized) {
	toplevel->scheduled.maximized = maximized;
	return nx_xdg_surface_schedule_configure(toplevel->base);
}

uint32_t nx_xdg_toplevel_set_fullscreen(struct nx_xdg_toplevel *toplevel,
		bool fullscreen) {
	toplevel->scheduled.fullscreen = fullscreen;
	return nx_xdg_surface_schedule_configure(toplevel->base);
}

uint32_t nx_xdg_toplevel_set_resizing(struct nx_xdg_toplevel *toplevel,
		bool resizing) {
	toplevel->scheduled.resizing = resizing;
	return nx_xdg_surface_schedule_configure(toplevel->base);
}

uint32_t nx_xdg_toplevel_set_tiled(struct nx_xdg_toplevel *toplevel,
		uint32_t tiled_edges) {
	toplevel->scheduled.tiled = tiled_edges;
	return nx_xdg_surface_schedule_configure(toplevel->base);
}

uint32_t nx_xdg_toplevel_set_bounds(struct nx_xdg_toplevel *toplevel,
		int32_t width, int32_t height) {
	assert(width >= 0 && height >= 0);
	toplevel->scheduled.fields |= NX_XDG_TOPLEVEL_CONFIGURE_BOUNDS;
	toplevel->scheduled.bounds.width = width;
	toplevel->scheduled.bounds.height = height;
	return nx_xdg_surface_schedule_configure(toplevel->base);
}

uint32_t nx_xdg_toplevel_set_wm_capabilities(struct nx_xdg_toplevel *toplevel,
		uint32_t caps) {
	toplevel->scheduled.fields |= NX_XDG_TOPLEVEL_CONFIGURE_WM_CAPABILITIES;
	toplevel->scheduled.wm_capabilities = caps;
	return nx_xdg_surface_schedule_configure(toplevel->base);
}

uint32_t nx_xdg_toplevel_set_suspended(struct nx_xdg_toplevel *toplevel,
		bool suspended) {
	toplevel->scheduled.suspended = suspended;
	return nx_xdg_surface_schedule_configure(toplevel->base);
}

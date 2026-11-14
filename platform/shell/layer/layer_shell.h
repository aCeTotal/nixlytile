#ifndef PLATFORM_SHELL_LAYER_LAYER_SHELL_H
#define PLATFORM_SHELL_LAYER_LAYER_SHELL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include "platform/surface/compositor.h"
#include "platform/geometry/edges.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

struct nx_layer_shell_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal new_surface; // output may be NULL
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

enum nx_layer_surface_v1_state_field {
	NX_LAYER_SURFACE_V1_STATE_DESIRED_SIZE = 1 << 0,
	NX_LAYER_SURFACE_V1_STATE_ANCHOR = 1 << 1,
	NX_LAYER_SURFACE_V1_STATE_EXCLUSIVE_ZONE = 1 << 2,
	NX_LAYER_SURFACE_V1_STATE_MARGIN = 1 << 3,
	NX_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY = 1 << 4,
	NX_LAYER_SURFACE_V1_STATE_LAYER = 1 << 5,
};

struct nx_layer_surface_v1_state {
	uint32_t committed; // enum nx_layer_surface_v1_state_field

	uint32_t anchor;
	int32_t exclusive_zone;
	struct {
		int32_t top, right, bottom, left;
	} margin;
	enum zwlr_layer_surface_v1_keyboard_interactivity keyboard_interactive;
	uint32_t desired_width, desired_height;
	enum zwlr_layer_shell_v1_layer layer;

	uint32_t configure_serial;
};

struct nx_layer_surface_v1 {
	struct nx_surface *surface;
	struct nx_output *output;
	struct wl_resource *resource;
	struct wl_list popups; // nx_xdg_popup.link

	bool configured;
	struct wl_list configure_list; // layer_configure.link

	struct nx_layer_surface_v1_state current, pending;

	// Ready to receive configure events.
	bool initialized;
	bool initial_commit;

	struct {
		// Emitted after unmap.
		struct wl_signal destroy;
	} events;

	void *data;

	struct {
		struct nx_surface_synced synced;
	};
};

struct nx_layer_shell_v1 *nx_layer_shell_v1_create(struct wl_display *display,
	uint32_t version);

// Returns the configure serial.
uint32_t nx_layer_surface_v1_configure(struct nx_layer_surface_v1 *surface,
	uint32_t width, uint32_t height);

// Sends closed, then destroys.
void nx_layer_surface_v1_destroy(struct nx_layer_surface_v1 *surface);

struct nx_layer_surface_v1 *nx_layer_surface_v1_try_from_nx_surface(
	struct nx_surface *surface);

// NONE for non-positive zones.
enum nx_edges nx_layer_surface_v1_get_exclusive_edge(struct nx_layer_surface_v1 *surface);

#endif

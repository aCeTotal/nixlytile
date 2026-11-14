#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include "platform/shell/layer/layer_surface.h"
#include "platform/shell/xdg/xdg_shell.h"

// No protocol error fits.
#define ERROR_POPUP_HAS_PARENT ((uint32_t)-1)

struct layer_configure {
	struct wl_list link; // nx_layer_surface_v1.configure_list
	uint32_t serial;
};

static void configure_destroy(struct layer_configure *configure) {
	wl_list_remove(&configure->link);
	free(configure);
}

void shell_layer_surface_reset(struct nx_layer_surface_v1 *surface) {
	surface->configured = false;
	surface->initialized = false;

	struct nx_xdg_popup *popup, *popup_tmp;
	wl_list_for_each_safe(popup, popup_tmp, &surface->popups, link) {
		nx_xdg_popup_destroy(popup);
	}

	struct layer_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &surface->configure_list, link) {
		configure_destroy(configure);
	}
}

void shell_layer_surface_destroy(struct nx_layer_surface_v1 *surface) {
	nx_surface_unmap(surface->surface);
	shell_layer_surface_reset(surface);

	wl_signal_emit_mutable(&surface->events.destroy, surface);

	assert(wl_list_empty(&surface->events.destroy.listener_list));

	nx_surface_synced_finish(&surface->synced);
	wl_resource_set_user_data(surface->resource, NULL);
	free(surface);
}

struct nx_layer_surface_v1 *shell_layer_surface_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwlr_layer_surface_v1_interface,
		&shell_layer_surface_impl));
	return wl_resource_get_user_data(resource);
}

static struct layer_configure *find_configure(
		struct nx_layer_surface_v1 *surface, uint32_t serial) {
	struct layer_configure *configure;
	wl_list_for_each(configure, &surface->configure_list, link) {
		if (configure->serial == serial) {
			return configure;
		}
	}
	return NULL;
}

static void layer_surface_handle_ack_configure(struct wl_client *client,
		struct wl_resource *resource, uint32_t serial) {
	struct nx_layer_surface_v1 *surface = shell_layer_surface_from_resource(resource);
	if (!surface) {
		return;
	}

	struct layer_configure *acked = find_configure(surface, serial);
	if (acked == NULL) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
			"wrong configure serial: %" PRIu32, serial);
		return;
	}

	// Older configures are implicitly acked.
	struct layer_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &surface->configure_list, link) {
		if (configure == acked) {
			break;
		}
		configure_destroy(configure);
	}

	surface->pending.configure_serial = acked->serial;
	surface->configured = true;
	configure_destroy(acked);
}

static void layer_surface_handle_set_size(struct wl_client *client,
		struct wl_resource *resource, uint32_t width, uint32_t height) {
	struct nx_layer_surface_v1 *surface = shell_layer_surface_from_resource(resource);
	if (!surface) {
		return;
	}

	if (width > INT32_MAX || height > INT32_MAX) {
		wl_client_post_implementation_error(client,
			"zwlr_layer_surface_v1.set_size: width and height can't be greater than INT32_MAX");
		return;
	}

	if (surface->pending.desired_width == width
			&& surface->pending.desired_height == height) {
		return;
	}

	surface->pending.committed |= NX_LAYER_SURFACE_V1_STATE_DESIRED_SIZE;
	surface->pending.desired_width = width;
	surface->pending.desired_height = height;
}

static void layer_surface_handle_set_anchor(struct wl_client *client,
		struct wl_resource *resource, uint32_t anchor) {
	if (!zwlr_layer_surface_v1_anchor_is_valid(anchor, wl_resource_get_version(resource))) {
		wl_resource_post_error(resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_ANCHOR,
			"invalid anchor %" PRIu32, anchor);
	}
	struct nx_layer_surface_v1 *surface = shell_layer_surface_from_resource(resource);
	if (!surface || surface->pending.anchor == anchor) {
		return;
	}

	surface->pending.committed |= NX_LAYER_SURFACE_V1_STATE_ANCHOR;
	surface->pending.anchor = anchor;
}

static void layer_surface_handle_set_exclusive_zone(struct wl_client *client,
		struct wl_resource *resource, int32_t zone) {
	struct nx_layer_surface_v1 *surface = shell_layer_surface_from_resource(resource);
	if (!surface || surface->pending.exclusive_zone == zone) {
		return;
	}

	surface->pending.committed |= NX_LAYER_SURFACE_V1_STATE_EXCLUSIVE_ZONE;
	surface->pending.exclusive_zone = zone;
}

static void layer_surface_handle_set_margin(
		struct wl_client *client, struct wl_resource *resource,
		int32_t top, int32_t right, int32_t bottom, int32_t left) {
	struct nx_layer_surface_v1 *surface = shell_layer_surface_from_resource(resource);
	if (!surface) {
		return;
	}

	if (surface->pending.margin.top == top
			&& surface->pending.margin.right == right
			&& surface->pending.margin.bottom == bottom
			&& surface->pending.margin.left == left) {
		return;
	}

	surface->pending.committed |= NX_LAYER_SURFACE_V1_STATE_MARGIN;
	surface->pending.margin.top = top;
	surface->pending.margin.right = right;
	surface->pending.margin.bottom = bottom;
	surface->pending.margin.left = left;
}

static void layer_surface_handle_set_keyboard_interactivity(
		struct wl_client *client, struct wl_resource *resource,
		uint32_t interactive) {
	struct nx_layer_surface_v1 *surface = shell_layer_surface_from_resource(resource);
	if (!surface) {
		return;
	}

	// A boolean before version 4.
	surface->pending.committed |= NX_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY;
	surface->pending.keyboard_interactive = !!interactive;
}

static void layer_surface_handle_get_popup(struct wl_client *client,
		struct wl_resource *layer_resource,
		struct wl_resource *popup_resource) {
	struct nx_layer_surface_v1 *parent =
		shell_layer_surface_from_resource(layer_resource);
	struct nx_xdg_popup *popup = nx_xdg_popup_from_resource(popup_resource);
	if (!parent) {
		return;
	}
	if (popup->parent != NULL) {
		wl_resource_post_error(layer_resource, ERROR_POPUP_HAS_PARENT,
			"xdg_popup already has a parent");
		return;
	}

	popup->parent = parent->surface;
	wl_list_insert(&parent->popups, &popup->link);
}

static void layer_surface_handle_set_layer(struct wl_client *client,
		struct wl_resource *surface_resource, uint32_t layer) {
	struct nx_layer_surface_v1 *surface =
		shell_layer_surface_from_resource(surface_resource);
	if (!surface) {
		return;
	}
	if (!zwlr_layer_shell_v1_layer_is_valid(layer,
			wl_resource_get_version(surface->resource))) {
		// Borrows the shell's error code.
		wl_resource_post_error(surface->resource,
			ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
			"Invalid layer %" PRIu32, layer);
		return;
	}

	if (surface->pending.layer == layer) {
		return;
	}

	surface->pending.committed |= NX_LAYER_SURFACE_V1_STATE_LAYER;
	surface->pending.layer = layer;
}

static void layer_surface_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

// set_exclusive_edge needs version 5.
const struct zwlr_layer_surface_v1_interface shell_layer_surface_impl = {
	.destroy = layer_surface_handle_destroy,
	.ack_configure = layer_surface_handle_ack_configure,
	.set_size = layer_surface_handle_set_size,
	.set_anchor = layer_surface_handle_set_anchor,
	.set_exclusive_zone = layer_surface_handle_set_exclusive_zone,
	.set_margin = layer_surface_handle_set_margin,
	.set_keyboard_interactivity = layer_surface_handle_set_keyboard_interactivity,
	.get_popup = layer_surface_handle_get_popup,
	.set_layer = layer_surface_handle_set_layer,
};

uint32_t nx_layer_surface_v1_configure(struct nx_layer_surface_v1 *surface,
		uint32_t width, uint32_t height) {
	assert(surface->initialized);

	struct wl_client *client = wl_resource_get_client(surface->resource);
	struct layer_configure *configure = calloc(1, sizeof(*configure));
	if (configure == NULL) {
		wl_client_post_no_memory(client);
		return surface->pending.configure_serial;
	}

	wl_list_insert(surface->configure_list.prev, &configure->link);
	configure->serial = wl_display_next_serial(wl_client_get_display(client));
	zwlr_layer_surface_v1_send_configure(surface->resource,
		configure->serial, width, height);
	return configure->serial;
}

void nx_layer_surface_v1_destroy(struct nx_layer_surface_v1 *surface) {
	if (surface == NULL) {
		return;
	}
	zwlr_layer_surface_v1_send_closed(surface->resource);
	shell_layer_surface_destroy(surface);
}

enum nx_edges nx_layer_surface_v1_get_exclusive_edge(struct nx_layer_surface_v1 *surface) {
	if (surface->current.exclusive_zone <= 0) {
		return NX_EDGE_NONE;
	}
	switch (surface->current.anchor) {
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP:
		return NX_EDGE_TOP;
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM:
		return NX_EDGE_BOTTOM;
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT:
		return NX_EDGE_LEFT;
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
	case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT:
		return NX_EDGE_RIGHT;
	default:
		return NX_EDGE_NONE;
	}
}

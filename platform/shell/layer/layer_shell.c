#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include "platform/log/log.h"
#include "platform/output/output.h"
#include "platform/shell/layer/layer_surface.h"

// Version gates assume this maximum.
#define LAYER_SHELL_VERSION 3

static const struct zwlr_layer_shell_v1_interface layer_shell_implementation;
static const struct nx_surface_role layer_surface_role;

static struct nx_layer_shell_v1 *layer_shell_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwlr_layer_shell_v1_interface,
		&layer_shell_implementation));
	return wl_resource_get_user_data(resource);
}

struct nx_layer_surface_v1 *nx_layer_surface_v1_try_from_nx_surface(
		struct nx_surface *surface) {
	if (surface->role != &layer_surface_role || surface->role_resource == NULL) {
		return NULL;
	}
	return shell_layer_surface_from_resource(surface->role_resource);
}

static void layer_surface_role_client_commit(struct nx_surface *nx_surface) {
	struct nx_layer_surface_v1 *surface =
		nx_layer_surface_v1_try_from_nx_surface(nx_surface);
	if (surface == NULL) {
		return;
	}

	if (nx_surface_state_has_buffer(&nx_surface->pending) && !surface->configured) {
		nx_surface_reject_pending(nx_surface, surface->resource,
			ZWLR_LAYER_SHELL_V1_ERROR_ALREADY_CONSTRUCTED,
			"layer_surface has never been configured");
		return;
	}

	uint32_t anchor = surface->pending.anchor;

	const uint32_t horiz = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	if (surface->pending.desired_width == 0 && (anchor & horiz) != horiz) {
		nx_surface_reject_pending(nx_surface, surface->resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
			"width 0 requested without setting left and right anchors");
		return;
	}

	const uint32_t vert = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	if (surface->pending.desired_height == 0 && (anchor & vert) != vert) {
		nx_surface_reject_pending(nx_surface, surface->resource,
			ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SIZE,
			"height 0 requested without setting top and bottom anchors");
	}
}

static void layer_surface_role_commit(struct nx_surface *nx_surface) {
	struct nx_layer_surface_v1 *surface =
		nx_layer_surface_v1_try_from_nx_surface(nx_surface);
	if (surface == NULL) {
		return;
	}

	if (nx_surface->unmap_commit) {
		shell_layer_surface_reset(surface);

		assert(!surface->initialized);
		surface->initial_commit = false;
	} else {
		surface->initial_commit = !surface->initialized;
		surface->initialized = true;
	}

	if (nx_surface_has_buffer(nx_surface)) {
		nx_surface_map(nx_surface);
	}
}

static void layer_surface_role_destroy(struct nx_surface *nx_surface) {
	struct nx_layer_surface_v1 *surface =
		nx_layer_surface_v1_try_from_nx_surface(nx_surface);
	if (surface == NULL) {
		return;
	}

	shell_layer_surface_destroy(surface);
}

static const struct nx_surface_role layer_surface_role = {
	.name = "zwlr_layer_surface_v1",
	.client_commit = layer_surface_role_client_commit,
	.commit = layer_surface_role_commit,
	.destroy = layer_surface_role_destroy,
};

static void surface_synced_move_state(void *_dst, void *_src) {
	struct nx_layer_surface_v1_state *dst = _dst, *src = _src;
	*dst = *src;
	src->committed = 0;
}

static const struct nx_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct nx_layer_surface_v1_state),
	.move_state = surface_synced_move_state,
};

static void set_initial_layer(struct nx_layer_surface_v1 *surface,
		uint32_t layer) {
	surface->current.layer = surface->pending.layer = layer;

	struct nx_surface_state *cached;
	wl_list_for_each(cached, &surface->surface->cached, cached_state_link) {
		struct nx_layer_surface_v1_state *state =
			nx_surface_synced_get_state(&surface->synced, cached);
		state->layer = layer;
	}
}

static void layer_shell_handle_get_layer_surface(struct wl_client *wl_client,
		struct wl_resource *client_resource, uint32_t id,
		struct wl_resource *surface_resource,
		struct wl_resource *output_resource,
		uint32_t layer, const char *namespace) {
	struct nx_layer_shell_v1 *shell = layer_shell_from_resource(client_resource);
	struct nx_surface *nx_surface = nx_surface_from_resource(surface_resource);

	if (!zwlr_layer_shell_v1_layer_is_valid(layer,
			wl_resource_get_version(client_resource))) {
		wl_resource_post_error(client_resource,
			ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
			"Invalid layer %" PRIu32, layer);
		return;
	}

	struct nx_layer_surface_v1 *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	if (!nx_surface_set_role(nx_surface, &layer_surface_role,
			client_resource, ZWLR_LAYER_SHELL_V1_ERROR_ROLE)) {
		free(surface);
		return;
	}

	surface->surface = nx_surface;
	if (output_resource) {
		surface->output = nx_output_from_resource(output_resource);
	}

	if (!nx_surface_synced_init(&surface->synced, nx_surface,
			&surface_synced_impl, &surface->pending, &surface->current)) {
		goto error_surface;
	}
	set_initial_layer(surface, layer);

	surface->resource = wl_resource_create(wl_client, &zwlr_layer_surface_v1_interface,
		wl_resource_get_version(client_resource), id);
	if (surface->resource == NULL) {
		goto error_synced;
	}

	wl_list_init(&surface->configure_list);
	wl_list_init(&surface->popups);
	wl_signal_init(&surface->events.destroy);

	nx_log(NX_DEBUG, "new layer_surface %p (res %p)",
		(void *)surface, (void *)surface->resource);
	wl_resource_set_implementation(surface->resource,
		&shell_layer_surface_impl, surface, NULL);

	nx_surface_set_role_object(nx_surface, surface->resource);

	wl_signal_emit_mutable(&shell->events.new_surface, surface);
	return;

error_synced:
	nx_surface_synced_finish(&surface->synced);
error_surface:
	free(surface);
	wl_client_post_no_memory(wl_client);
}

static void layer_shell_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwlr_layer_shell_v1_interface layer_shell_implementation = {
	.get_layer_surface = layer_shell_handle_get_layer_surface,
	.destroy = layer_shell_handle_destroy,
};

static void layer_shell_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		wl_client, &zwlr_layer_shell_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource,
		&layer_shell_implementation, data, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_layer_shell_v1 *layer_shell =
		wl_container_of(listener, layer_shell, display_destroy);

	assert(wl_list_empty(&layer_shell->events.new_surface.listener_list));

	wl_list_remove(&layer_shell->display_destroy.link);
	wl_global_destroy(layer_shell->global);
	free(layer_shell);
}

struct nx_layer_shell_v1 *nx_layer_shell_v1_create(struct wl_display *display,
		uint32_t version) {
	assert(version <= LAYER_SHELL_VERSION);

	struct nx_layer_shell_v1 *layer_shell = calloc(1, sizeof(*layer_shell));
	if (!layer_shell) {
		return NULL;
	}

	layer_shell->global = wl_global_create(display,
		&zwlr_layer_shell_v1_interface, version, layer_shell, layer_shell_bind);
	if (!layer_shell->global) {
		free(layer_shell);
		return NULL;
	}

	wl_signal_init(&layer_shell->events.new_surface);

	layer_shell->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &layer_shell->display_destroy);

	return layer_shell;
}

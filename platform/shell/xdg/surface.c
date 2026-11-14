#include <assert.h>
#include <stdlib.h>
#include "platform/log/log.h"
#include "platform/shell/popup/positioner.h"
#include "platform/shell/xdg/role.h"
#include "platform/shell/xdg/surface.h"

static const struct xdg_surface_interface xdg_surface_implementation;

struct nx_xdg_surface *shell_xdg_surface_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_surface_interface,
		&xdg_surface_implementation));
	return wl_resource_get_user_data(resource);
}

static void xdg_surface_handle_get_popup(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *parent_resource,
		struct wl_resource *positioner_resource) {
	struct nx_xdg_surface *xdg_surface = shell_xdg_surface_from_resource(resource);
	assert(xdg_surface != NULL);
	struct nx_xdg_surface *parent = NULL;
	if (parent_resource != NULL) {
		parent = shell_xdg_surface_from_resource(parent_resource);
	}
	struct nx_xdg_positioner *positioner =
		shell_positioner_from_resource(positioner_resource);
	shell_popup_create(xdg_surface, parent, positioner, id);
}

static void xdg_surface_handle_get_toplevel(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	struct nx_xdg_surface *xdg_surface = shell_xdg_surface_from_resource(resource);
	assert(xdg_surface != NULL);
	shell_toplevel_create(xdg_surface, id);
}

static void xdg_surface_handle_set_window_geometry(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y, int32_t width,
		int32_t height) {
	struct nx_xdg_surface *surface = shell_xdg_surface_from_resource(resource);
	assert(surface != NULL);

	if (surface->role == NX_XDG_SURFACE_ROLE_NONE) {
		wl_resource_post_error(surface->resource,
			XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
			"xdg_surface must have a role");
		return;
	}

	if (width <= 0 || height <= 0) {
		wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE,
			"Tried to set invalid xdg-surface geometry");
		return;
	}

	surface->pending.committed |= NX_XDG_SURFACE_STATE_WINDOW_GEOMETRY;
	surface->pending.geometry = (struct nx_box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
}

static void xdg_surface_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_xdg_surface *surface = shell_xdg_surface_from_resource(resource);
	if (surface == NULL) {
		return;
	}

	if (surface->role_resource != NULL) {
		wl_resource_post_error(resource,
			XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
			"surface was destroyed before its role object");
		return;
	}

	wl_resource_destroy(resource);
}

static const struct xdg_surface_interface xdg_surface_implementation = {
	.destroy = xdg_surface_handle_destroy,
	.get_toplevel = xdg_surface_handle_get_toplevel,
	.get_popup = xdg_surface_handle_get_popup,
	.ack_configure = shell_xdg_surface_handle_ack_configure,
	.set_window_geometry = xdg_surface_handle_set_window_geometry,
};

static void surface_synced_move_state(void *_dst, void *_src) {
	struct nx_xdg_surface_state *dst = _dst, *src = _src;
	*dst = *src;
	src->committed = 0;
}

static const struct nx_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct nx_xdg_surface_state),
	.move_state = surface_synced_move_state,
};

void shell_xdg_surface_create(struct nx_xdg_client *client,
		struct nx_surface *nx_surface, uint32_t id) {
	if (!nx_surface_set_role(nx_surface, &shell_xdg_surface_role,
			client->resource, XDG_WM_BASE_ERROR_ROLE)) {
		return;
	}

	if (nx_surface_has_buffer(nx_surface)) {
		wl_resource_post_error(client->resource,
			XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
			"xdg_surface must not have a buffer at creation");
		return;
	}

	struct nx_xdg_surface *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		wl_client_post_no_memory(client->client);
		return;
	}

	if (!nx_surface_synced_init(&surface->synced, nx_surface,
			&surface_synced_impl, &surface->pending, &surface->current)) {
		goto error_surface;
	}

	surface->client = client;
	surface->role = NX_XDG_SURFACE_ROLE_NONE;
	surface->surface = nx_surface;
	surface->resource = wl_resource_create(client->client,
		&xdg_surface_interface, wl_resource_get_version(client->resource),
		id);
	if (surface->resource == NULL) {
		goto error_synced;
	}

	wl_list_init(&surface->configure_list);
	wl_list_init(&surface->popups);

	wl_signal_init(&surface->events.destroy);
	wl_signal_init(&surface->events.ping_timeout);
	wl_signal_init(&surface->events.configure);
	wl_signal_init(&surface->events.ack_configure);

	nx_log(NX_DEBUG, "new xdg_surface %p (res %p)", (void *)surface,
		(void *)surface->resource);
	wl_resource_set_implementation(surface->resource,
		&xdg_surface_implementation, surface, NULL);
	wl_list_insert(&client->surfaces, &surface->link);

	nx_surface_set_role_object(nx_surface, surface->resource);
	return;

error_synced:
	nx_surface_synced_finish(&surface->synced);
error_surface:
	free(surface);
	wl_client_post_no_memory(client->client);
}

void shell_xdg_surface_destroy(struct nx_xdg_surface *surface) {
	shell_xdg_surface_destroy_role_object(surface);
	shell_xdg_surface_reset(surface);

	wl_signal_emit_mutable(&surface->events.destroy, NULL);

	assert(wl_list_empty(&surface->events.destroy.listener_list));
	assert(wl_list_empty(&surface->events.ping_timeout.listener_list));
	assert(wl_list_empty(&surface->events.configure.listener_list));
	assert(wl_list_empty(&surface->events.ack_configure.listener_list));

	wl_list_remove(&surface->link);
	nx_surface_synced_finish(&surface->synced);
	wl_resource_set_user_data(surface->resource, NULL);
	free(surface);
}

void nx_xdg_surface_ping(struct nx_xdg_surface *surface) {
	struct nx_xdg_client *client = surface->client;
	if (client->ping_serial != 0) {
		return;
	}

	client->ping_serial =
		wl_display_next_serial(wl_client_get_display(client->client));
	wl_event_source_timer_update(client->ping_timer,
		client->shell->ping_timeout);
	xdg_wm_base_send_ping(client->resource, client->ping_serial);
}

#include <assert.h>
#include "platform/log/log.h"
#include "platform/surface/compositor.h"
#include "platform/xwayland/server/server.h"
#include "platform/xwayland/shell.h"
#include "platform/xwayland/wm/xwm.h"

static const struct nx_addon_interface surface_addon_impl;

struct nx_xwayland_surface *nx_xwayland_surface_try_from_nx_surface(
		struct nx_surface *surface) {
	struct nx_addon *addon =
		nx_addon_find(&surface->addons, NULL, &surface_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct nx_xwayland_surface *xsurface =
		wl_container_of(addon, xsurface, surface_addon);
	return xsurface;
}

static void handle_surface_commit(struct wl_listener *listener, void *data) {
	struct nx_xwayland_surface *xsurface =
		wl_container_of(listener, xsurface, surface_commit);
	if (nx_surface_has_buffer(xsurface->surface)) {
		nx_surface_map(xsurface->surface);
	}
}

static void handle_surface_map(struct wl_listener *listener, void *data) {
	struct nx_xwayland_surface *xsurface =
		wl_container_of(listener, xsurface, surface_map);
	xwm_update_client_list(xsurface->xwm);
}

static void handle_surface_unmap(struct wl_listener *listener, void *data) {
	struct nx_xwayland_surface *xsurface =
		wl_container_of(listener, xsurface, surface_unmap);
	xwm_update_client_list(xsurface->xwm);
}

static void handle_addon_destroy(struct nx_addon *addon) {
	struct nx_xwayland_surface *xsurface =
		wl_container_of(addon, xsurface, surface_addon);
	xwm_surface_dissociate(xsurface);
}

static const struct nx_addon_interface surface_addon_impl = {
	.name = "nx_xwayland_surface",
	.destroy = handle_addon_destroy,
};

static void associate(struct nx_xwayland_surface *xsurface,
		struct nx_surface *surface) {
	assert(xsurface->surface == NULL);

	wl_list_remove(&xsurface->unpaired_link);
	wl_list_init(&xsurface->unpaired_link);
	xsurface->surface_id = 0;

	xsurface->surface = surface;
	nx_addon_init(&xsurface->surface_addon, &surface->addons, NULL,
		&surface_addon_impl);

	xsurface->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&surface->events.commit, &xsurface->surface_commit);
	xsurface->surface_map.notify = handle_surface_map;
	wl_signal_add(&surface->events.map, &xsurface->surface_map);
	xsurface->surface_unmap.notify = handle_surface_unmap;
	wl_signal_add(&surface->events.unmap, &xsurface->surface_unmap);

	xwm_read_initial_properties(xsurface);
	wl_signal_emit_mutable(&xsurface->events.associate, NULL);
}

static void release_surface(struct nx_xwayland_surface *xsurface) {
	nx_surface_unmap(xsurface->surface);
	wl_signal_emit_mutable(&xsurface->events.dissociate, NULL);

	wl_list_remove(&xsurface->surface_commit.link);
	wl_list_remove(&xsurface->surface_map.link);
	wl_list_remove(&xsurface->surface_unmap.link);
	nx_addon_finish(&xsurface->surface_addon);
	xsurface->surface = NULL;
}

void xwm_surface_dissociate(struct nx_xwayland_surface *xsurface) {
	if (xsurface->surface != NULL) {
		release_surface(xsurface);
	}

	// Stale ids must not pair.
	wl_list_remove(&xsurface->unpaired_link);
	wl_list_init(&xsurface->unpaired_link);
	xsurface->surface_id = 0;
	xsurface->serial = 0;

	wl_list_remove(&xsurface->stack_link);
	wl_list_init(&xsurface->stack_link);
	xwm_update_client_list_stacking(xsurface->xwm);
}

static void wait_for_surface(struct nx_xwayland_surface *xsurface) {
	wl_list_remove(&xsurface->unpaired_link);
	wl_list_insert(&xsurface->xwm->unpaired_surfaces, &xsurface->unpaired_link);
}

void xwm_handle_surface_id_message(struct nx_xwm *xwm,
		xcb_client_message_event_t *ev) {
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->window);
	if (xsurface == NULL) {
		nx_log(NX_DEBUG,
			"Received client message WL_SURFACE_ID but no X11 window %u",
			ev->window);
		return;
	}
	if (xsurface->surface != NULL) {
		nx_log(NX_DEBUG, "Received multiple client messages WL_SURFACE_ID "
			"for an already-associated X11 window %u", ev->window);
		return;
	}

	// The wl_surface may not exist yet.
	uint32_t id = ev->data.data32[0];
	struct wl_resource *resource =
		wl_client_get_object(xwm->xwayland->server->client, id);
	if (resource == NULL) {
		xsurface->surface_id = id;
		wait_for_surface(xsurface);
		return;
	}
	if (wl_resource_get_interface(resource) != &wl_surface_interface) {
		nx_log(NX_DEBUG, "Received client message WL_SURFACE_ID "
			"for X11 window %u but Wayland object is not a wl_surface",
			ev->window);
		return;
	}
	associate(xsurface, nx_surface_from_resource(resource));
}

void xwm_handle_surface_serial_message(struct nx_xwm *xwm,
		xcb_client_message_event_t *ev) {
	struct nx_xwayland_surface *xsurface = xwm_lookup_surface(xwm, ev->window);
	if (xsurface == NULL) {
		nx_log(NX_DEBUG,
			"Received client message WL_SURFACE_SERIAL but no X11 window %u",
			ev->window);
		return;
	}
	if (xsurface->serial != 0) {
		nx_log(NX_DEBUG, "Received multiple client messages WL_SURFACE_SERIAL "
			"for the same X11 window %u", ev->window);
		return;
	}

	uint32_t serial_lo = ev->data.data32[0];
	uint32_t serial_hi = ev->data.data32[1];
	xsurface->serial = ((uint64_t)serial_hi << 32) | serial_lo;

	struct nx_surface *surface = nx_xwayland_shell_v1_surface_from_serial(
		xwm->xwayland->shell_v1, xsurface->serial);
	if (surface == NULL) {
		wait_for_surface(xsurface);
		return;
	}
	associate(xsurface, surface);
}

static void handle_compositor_new_surface(struct wl_listener *listener,
		void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, compositor_new_surface);
	struct nx_surface *surface = data;
	if (wl_resource_get_client(surface->resource) != xwm->xwayland->server->client) {
		return;
	}

	nx_log(NX_DEBUG, "New xwayland surface: %p", (void *)surface);

	uint32_t surface_id = wl_resource_get_id(surface->resource);
	struct nx_xwayland_surface *xsurface;
	wl_list_for_each(xsurface, &xwm->unpaired_surfaces, unpaired_link) {
		if (xsurface->surface_id == surface_id) {
			associate(xsurface, surface);
			xwm_schedule_flush(xwm);
			return;
		}
	}
}

static void handle_compositor_destroy(struct wl_listener *listener, void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, compositor_destroy);
	wl_list_remove(&xwm->compositor_new_surface.link);
	wl_list_remove(&xwm->compositor_destroy.link);
	wl_list_init(&xwm->compositor_new_surface.link);
	wl_list_init(&xwm->compositor_destroy.link);
}

static void handle_shell_v1_new_surface(struct wl_listener *listener,
		void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, shell_v1_new_surface);
	struct nx_xwayland_surface_v1 *shell_surface = data;

	struct nx_xwayland_surface *xsurface;
	wl_list_for_each(xsurface, &xwm->unpaired_surfaces, unpaired_link) {
		if (xsurface->serial == shell_surface->serial) {
			associate(xsurface, shell_surface->surface);
			return;
		}
	}
}

static void handle_shell_v1_destroy(struct wl_listener *listener, void *data) {
	struct nx_xwm *xwm = wl_container_of(listener, xwm, shell_v1_destroy);
	wl_list_remove(&xwm->shell_v1_new_surface.link);
	wl_list_remove(&xwm->shell_v1_destroy.link);
	wl_list_init(&xwm->shell_v1_new_surface.link);
	wl_list_init(&xwm->shell_v1_destroy.link);
}

void xwm_watch_new_surfaces(struct nx_xwm *xwm) {
	struct nx_xwayland *xwayland = xwm->xwayland;

	xwm->compositor_new_surface.notify = handle_compositor_new_surface;
	wl_signal_add(&xwayland->compositor->events.new_surface,
		&xwm->compositor_new_surface);
	xwm->compositor_destroy.notify = handle_compositor_destroy;
	wl_signal_add(&xwayland->compositor->events.destroy,
		&xwm->compositor_destroy);

	xwm->shell_v1_new_surface.notify = handle_shell_v1_new_surface;
	wl_signal_add(&xwayland->shell_v1->events.new_surface,
		&xwm->shell_v1_new_surface);
	xwm->shell_v1_destroy.notify = handle_shell_v1_destroy;
	wl_signal_add(&xwayland->shell_v1->events.destroy, &xwm->shell_v1_destroy);
}

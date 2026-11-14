#include <assert.h>
#include "platform/geometry/box.h"
#include "platform/log/log.h"
#include "platform/shell/xdg/role.h"
#include "platform/shell/xdg/surface.h"

static void update_geometry(struct nx_xdg_surface *surface) {
	struct nx_box *geom = &surface->geometry;
	if (nx_box_empty(&surface->current.geometry)) {
		nx_surface_get_extents(surface->surface, geom);
		return;
	}
	if ((surface->current.committed & NX_XDG_SURFACE_STATE_WINDOW_GEOMETRY) == 0) {
		return;
	}

	nx_surface_get_extents(surface->surface, geom);
	nx_box_intersection(geom, geom, &surface->current.geometry);
	if (!nx_box_empty(geom)) {
		return;
	}

	nx_log(NX_INFO,
		"A client has committed an invalid effective window geometry (%d,%d %dx%d); "
		"this will result in client disconnection in the future",
		geom->x, geom->y, geom->width, geom->height);
	// Empty extents break later commits.
	*geom = surface->current.geometry;
}

static bool has_role_object(const struct nx_xdg_surface *surface) {
	switch (surface->role) {
	case NX_XDG_SURFACE_ROLE_NONE:
		assert(0 && "not reached");
		return false;
	case NX_XDG_SURFACE_ROLE_TOPLEVEL:
		return surface->toplevel != NULL;
	case NX_XDG_SURFACE_ROLE_POPUP:
		return surface->popup != NULL;
	}
	return false;
}

static void xdg_surface_role_client_commit(struct nx_surface *nx_surface) {
	struct nx_xdg_surface *surface = nx_xdg_surface_try_from_nx_surface(nx_surface);
	assert(surface != NULL);

	if (nx_surface_state_has_buffer(&nx_surface->pending) && !surface->configured) {
		nx_surface_reject_pending(nx_surface, surface->resource,
			XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER, "xdg_surface has never been configured");
		return;
	}

	if (surface->role_resource == NULL) {
		nx_surface_reject_pending(nx_surface, surface->resource,
			XDG_SURFACE_ERROR_NOT_CONSTRUCTED, "xdg_surface must have a role object");
		return;
	}

	switch (surface->role) {
	case NX_XDG_SURFACE_ROLE_NONE:
		assert(0 && "not reached");
		return;
	case NX_XDG_SURFACE_ROLE_TOPLEVEL:
		if (surface->toplevel != NULL) {
			shell_toplevel_client_commit(surface->toplevel);
		}
		break;
	case NX_XDG_SURFACE_ROLE_POPUP:
		if (surface->popup != NULL) {
			shell_popup_client_commit(surface->popup);
		}
		break;
	}
}

static void xdg_surface_role_commit(struct nx_surface *nx_surface) {
	struct nx_xdg_surface *surface = nx_xdg_surface_try_from_nx_surface(nx_surface);
	assert(surface != NULL);

	if (nx_surface->unmap_commit) {
		shell_xdg_surface_reset_role_object(surface);
		shell_xdg_surface_reset(surface);

		assert(!surface->initial_commit);
		surface->initial_commit = false;
	} else {
		surface->initial_commit = !surface->initialized;
		surface->initialized = true;
	}

	if (!has_role_object(surface)) {
		return;
	}
	// The map hook updates geometry.
	if (!nx_surface->mapped && nx_surface_has_buffer(nx_surface)) {
		nx_surface_map(nx_surface);
		return;
	}
	update_geometry(surface);
}

static void xdg_surface_role_map(struct nx_surface *nx_surface) {
	struct nx_xdg_surface *surface = nx_xdg_surface_try_from_nx_surface(nx_surface);
	assert(surface != NULL);

	update_geometry(surface);
}

static void xdg_surface_role_destroy(struct nx_surface *nx_surface) {
	struct nx_xdg_surface *surface = nx_xdg_surface_try_from_nx_surface(nx_surface);
	// Only inert during destruction.
	if (surface == NULL) {
		return;
	}

	shell_xdg_surface_destroy(surface);
}

const struct nx_surface_role shell_xdg_surface_role = {
	.name = "xdg_surface",
	.client_commit = xdg_surface_role_client_commit,
	.commit = xdg_surface_role_commit,
	.map = xdg_surface_role_map,
	.destroy = xdg_surface_role_destroy,
};

struct nx_xdg_surface *nx_xdg_surface_try_from_nx_surface(
		struct nx_surface *surface) {
	if (surface->role != &shell_xdg_surface_role || surface->role_resource == NULL) {
		return NULL;
	}
	return shell_xdg_surface_from_resource(surface->role_resource);
}

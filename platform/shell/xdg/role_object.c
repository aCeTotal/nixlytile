#include <assert.h>
#include <inttypes.h>
#include "platform/shell/xdg/role.h"
#include "platform/shell/xdg/surface.h"

static const char *const role_names[] = {
	[NX_XDG_SURFACE_ROLE_TOPLEVEL] = "xdg_toplevel",
	[NX_XDG_SURFACE_ROLE_POPUP] = "xdg_popup",
};

bool shell_xdg_surface_set_role(struct nx_xdg_surface *surface,
		enum nx_xdg_surface_role role) {
	assert(role != NX_XDG_SURFACE_ROLE_NONE);

	if (surface->role != NX_XDG_SURFACE_ROLE_NONE && surface->role != role) {
		wl_resource_post_error(surface->client->resource, XDG_WM_BASE_ERROR_ROLE,
			"Cannot assign role %s to xdg_surface@%" PRIu32 ", already has role %s",
			role_names[role], wl_resource_get_id(surface->resource),
			role_names[surface->role]);
		return false;
	}
	if (surface->role_resource != NULL) {
		wl_resource_post_error(surface->client->resource, XDG_WM_BASE_ERROR_ROLE,
			"Cannot reassign role %s to xdg_surface@%" PRIu32 ", role object still exists",
			role_names[role], wl_resource_get_id(surface->resource));
		return false;
	}

	surface->role = role;
	return true;
}

void shell_xdg_surface_reset_role_object(struct nx_xdg_surface *surface) {
	switch (surface->role) {
	case NX_XDG_SURFACE_ROLE_TOPLEVEL:
		if (surface->toplevel != NULL) {
			shell_toplevel_reset(surface->toplevel);
		}
		break;
	case NX_XDG_SURFACE_ROLE_POPUP:
		if (surface->popup != NULL) {
			shell_popup_reset(surface->popup);
		}
		break;
	case NX_XDG_SURFACE_ROLE_NONE:
		break;
	}
}

void shell_xdg_surface_destroy_role_object(struct nx_xdg_surface *surface) {
	if (surface->role_resource == NULL) {
		return;
	}

	switch (surface->role) {
	case NX_XDG_SURFACE_ROLE_NONE:
		assert(0 && "not reached");
		break;
	case NX_XDG_SURFACE_ROLE_TOPLEVEL:
		if (surface->toplevel != NULL) {
			shell_toplevel_destroy(surface->toplevel);
		}
		break;
	case NX_XDG_SURFACE_ROLE_POPUP:
		if (surface->popup != NULL) {
			shell_popup_destroy(surface->popup);
		}
		break;
	}

	surface->role_resource = NULL;
	wl_list_remove(&surface->role_resource_destroy.link);
	wl_list_init(&surface->role_resource_destroy.link);
}

static void handle_role_resource_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_xdg_surface *surface =
		wl_container_of(listener, surface, role_resource_destroy);
	shell_xdg_surface_destroy_role_object(surface);
	shell_xdg_surface_reset(surface);
}

void shell_xdg_surface_set_role_object(struct nx_xdg_surface *surface,
		struct wl_resource *role_resource) {
	assert(surface->role != NX_XDG_SURFACE_ROLE_NONE);
	assert(surface->role_resource == NULL);
	assert(role_resource != NULL);
	surface->role_resource = role_resource;
	surface->role_resource_destroy.notify = handle_role_resource_destroy;
	wl_resource_add_destroy_listener(role_resource, &surface->role_resource_destroy);
}

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include "platform/surface/compositor/surface.h"
#include "platform/surface/subsurface/subsurface.h"

void nx_surface_map(struct nx_surface *surface) {
	if (surface->mapped) {
		return;
	}
	assert(nx_surface_has_buffer(surface));
	surface->mapped = true;

	struct nx_subsurface *subsurface;
	wl_list_for_each(subsurface, &surface->current.subsurfaces_below, current.link) {
		subsurface_consider_map(subsurface);
	}
	wl_list_for_each(subsurface, &surface->current.subsurfaces_above, current.link) {
		subsurface_consider_map(subsurface);
	}

	if (surface_role_active(surface) && surface->role->map != NULL) {
		surface->role->map(surface);
	}

	wl_signal_emit_mutable(&surface->events.map, NULL);
}

void nx_surface_unmap(struct nx_surface *surface) {
	if (!surface->mapped) {
		return;
	}
	surface->mapped = false;
	wl_signal_emit_mutable(&surface->events.unmap, NULL);
	if (surface_role_active(surface) && surface->role->unmap != NULL) {
		surface->role->unmap(surface);
	}

	struct nx_subsurface *subsurface;
	wl_list_for_each(subsurface, &surface->current.subsurfaces_below, current.link) {
		nx_surface_unmap(subsurface->surface);
	}
	wl_list_for_each(subsurface, &surface->current.subsurfaces_above, current.link) {
		nx_surface_unmap(subsurface->surface);
	}
}

void nx_surface_reject_pending(struct nx_surface *surface, struct wl_resource *resource,
		uint32_t code, const char *msg, ...) {
	assert(surface->handling_commit);
	if (surface->pending_rejected) {
		return;
	}

	va_list args;
	va_start(args, msg);

	wl_resource_post_error_vargs(resource, code, msg, args);
	surface->pending_rejected = true;

	va_end(args);
}

bool nx_surface_set_role(struct nx_surface *surface, const struct nx_surface_role *role,
		struct wl_resource *error_resource, uint32_t error_code) {
	assert(role != NULL);

	bool conflict = surface->role != NULL && surface->role != role;
	if (conflict && error_resource != NULL) {
		wl_resource_post_error(error_resource, error_code,
			"Cannot assign role %s to wl_surface@%" PRIu32 ", already has role %s",
			role->name, wl_resource_get_id(surface->resource),
			surface->role->name);
	}
	if (conflict) {
		return false;
	}
	if (surface->role_resource != NULL) {
		wl_resource_post_error(error_resource, error_code,
			"Cannot reassign role %s to wl_surface@%" PRIu32 ", role object still exists",
			role->name, wl_resource_get_id(surface->resource));
		return false;
	}

	surface->role = role;
	return true;
}

static void surface_handle_role_resource_destroy(struct wl_listener *listener, void *data) {
	struct nx_surface *surface = wl_container_of(listener, surface, role_resource_destroy);
	surface_destroy_role_object(surface);
}

void nx_surface_set_role_object(struct nx_surface *surface, struct wl_resource *role_resource) {
	assert(surface->role != NULL);
	assert(!surface->role->no_object);
	assert(surface->role_resource == NULL);
	assert(role_resource != NULL);
	surface->role_resource = role_resource;
	surface->role_resource_destroy.notify = surface_handle_role_resource_destroy;
	wl_resource_add_destroy_listener(role_resource, &surface->role_resource_destroy);
}

void surface_destroy_role_object(struct nx_surface *surface) {
	if (surface->role_resource == NULL) {
		return;
	}
	nx_surface_unmap(surface);
	if (surface->role->destroy != NULL) {
		surface->role->destroy(surface);
	}
	surface->role_resource = NULL;
	wl_list_remove(&surface->role_resource_destroy.link);
	wl_list_init(&surface->role_resource_destroy.link);
}

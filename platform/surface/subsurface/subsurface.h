#ifndef PLATFORM_SURFACE_SUBSURFACE_SUBSURFACE_H
#define PLATFORM_SURFACE_SUBSURFACE_SUBSURFACE_H

#include "platform/surface/subcompositor.h"

extern const struct wl_subsurface_interface subsurface_implementation;

// NULL once inert
struct nx_subsurface *subsurface_from_resource(struct wl_resource *resource);
// NULL without a live role
struct nx_subsurface *subsurface_try_from_surface(struct nx_surface *surface);
bool subsurface_is_synchronized(struct nx_subsurface *subsurface);

void subsurface_consider_map(struct nx_subsurface *subsurface);
void subsurface_handle_parent_commit(struct nx_subsurface *subsurface);
void subcompositor_get_subsurface(struct wl_client *client,
	struct wl_resource *resource, uint32_t id,
	struct wl_resource *surface_resource, struct wl_resource *parent_resource);

#endif

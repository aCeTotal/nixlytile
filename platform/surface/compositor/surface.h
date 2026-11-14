#ifndef PLATFORM_SURFACE_COMPOSITOR_SURFACE_H
#define PLATFORM_SURFACE_COMPOSITOR_SURFACE_H

#include "platform/surface/compositor.h"

extern const struct wl_surface_interface surface_implementation;

struct nx_surface *surface_create(struct wl_client *client,
	uint32_t version, uint32_t id, struct nx_compositor *compositor);
void surface_set_pending_buffer_resource(struct nx_surface *surface,
	struct wl_resource *resource);
void surface_handle_commit(struct wl_client *client, struct wl_resource *resource);

bool surface_state_init(struct nx_surface_state *state, struct nx_surface *surface);
void surface_state_finish(struct nx_surface_state *state);
void surface_state_move(struct nx_surface_state *state,
	struct nx_surface_state *next, struct nx_surface *surface);
void surface_state_destroy_cached(struct nx_surface_state *state,
	struct nx_surface *surface);
void surface_state_transformed_buffer_size(const struct nx_surface_state *state,
	int *width, int *height);
void surface_state_viewport_src_size(const struct nx_surface_state *state,
	int *width, int *height);
void surface_finalize_pending(struct nx_surface *surface);

void *surface_synced_create_state(struct nx_surface_synced *synced);
void surface_synced_destroy_state(struct nx_surface_synced *synced, void *state);
void surface_synced_move_state(struct nx_surface_synced *synced,
	void *dst, void *src);

void surface_destroy_role_object(struct nx_surface *surface);
void surface_output_destroy(struct nx_surface_output *surface_output);

// Hooks need the role object
static inline bool surface_role_active(const struct nx_surface *surface) {
	return surface->role != NULL &&
		(surface->role_resource != NULL || surface->role->no_object);
}

#endif

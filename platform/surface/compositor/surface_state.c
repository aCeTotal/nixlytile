#include <stdlib.h>
#include <string.h>
#include <wayland-server-protocol.h>
#include "platform/buffer/buffer.h"
#include "platform/geometry/transform.h"
#include "platform/log/log.h"
#include "platform/surface/compositor/surface.h"
#include "platform/surface/subcompositor.h"

bool surface_state_init(struct nx_surface_state *state,
		struct nx_surface *surface) {
	*state = (struct nx_surface_state){
		.scale = 1,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
	};

	wl_list_init(&state->subsurfaces_above);
	wl_list_init(&state->subsurfaces_below);

	wl_list_init(&state->frame_callback_list);

	pixman_region32_init(&state->surface_damage);
	pixman_region32_init(&state->buffer_damage);
	pixman_region32_init(&state->opaque);
	pixman_region32_init_rect(&state->input,
		INT32_MIN, INT32_MIN, UINT32_MAX, UINT32_MAX);

	wl_array_init(&state->synced);
	void *ptr = wl_array_add(&state->synced, surface->synced_len * sizeof(void *));
	return ptr != NULL;
}

void surface_state_finish(struct nx_surface_state *state) {
	nx_buffer_unlock(state->buffer);

	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &state->frame_callback_list) {
		wl_resource_destroy(resource);
	}

	pixman_region32_fini(&state->surface_damage);
	pixman_region32_fini(&state->buffer_damage);
	pixman_region32_fini(&state->opaque);
	pixman_region32_fini(&state->input);

	wl_array_release(&state->synced);
}

void surface_state_destroy_cached(struct nx_surface_state *state,
		struct nx_surface *surface) {
	void **synced_states = state->synced.data;
	struct nx_surface_synced *synced;
	wl_list_for_each(synced, &surface->synced, link) {
		surface_synced_destroy_state(synced, synced_states[synced->index]);
	}

	surface_state_finish(state);
	wl_list_remove(&state->cached_state_link);
	free(state);
}

void surface_state_transformed_buffer_size(const struct nx_surface_state *state,
		int *width, int *height) {
	*width = state->buffer_width;
	*height = state->buffer_height;
	nx_output_transform_coords(state->transform, width, height);
}

// Size before destination scaling
void surface_state_viewport_src_size(const struct nx_surface_state *state,
		int *width, int *height) {
	if (state->buffer_width == 0 && state->buffer_height == 0) {
		*width = *height = 0;
		return;
	}
	if (state->viewport.has_src) {
		*width = (int)state->viewport.src.width;
		*height = (int)state->viewport.src.height;
		return;
	}
	surface_state_transformed_buffer_size(state, width, height);
	*width /= state->scale;
	*height /= state->scale;
}

static void take_pending_buffer(struct nx_surface *surface) {
	struct wl_resource *buffer_resource = surface->pending_buffer_resource;
	if (buffer_resource == NULL) {
		return;
	}
	surface_set_pending_buffer_resource(surface, NULL);
	surface->pending.buffer = nx_buffer_try_from_resource(buffer_resource);
	if (surface->pending.buffer == NULL) {
		nx_surface_reject_pending(surface, buffer_resource, -1, "unknown buffer type");
	}
}

// Cursor clients get a pass
static bool tolerates_unscaled_buffer(const struct nx_surface_role *role) {
	return role == NULL || strcmp(role->name, "wl_pointer-cursor") == 0 ||
		strcmp(role->name, "wp_tablet_tool-cursor") == 0;
}

static void check_buffer_scale(struct nx_surface *surface) {
	const struct nx_surface_state *pending = &surface->pending;
	if (pending->viewport.has_src ||
			(pending->buffer_width % pending->scale == 0 &&
			pending->buffer_height % pending->scale == 0)) {
		return;
	}
	if (tolerates_unscaled_buffer(surface->role)) {
		nx_log(NX_DEBUG, "Client bug: submitted a buffer whose size (%dx%d) "
			"is not divisible by scale (%d)", pending->buffer_width,
			pending->buffer_height, pending->scale);
		return;
	}
	nx_surface_reject_pending(surface, surface->resource,
		WL_SURFACE_ERROR_INVALID_SIZE,
		"Buffer size (%dx%d) is not divisible by scale (%d)",
		pending->buffer_width, pending->buffer_height, pending->scale);
}

static void finalize_pending_size(struct nx_surface_state *pending) {
	if (!pending->viewport.has_dst) {
		surface_state_viewport_src_size(pending, &pending->width, &pending->height);
		return;
	}
	if (pending->buffer_width == 0 && pending->buffer_height == 0) {
		pending->width = pending->height = 0;
		return;
	}
	pending->width = pending->viewport.dst_width;
	pending->height = pending->viewport.dst_height;
}

void surface_finalize_pending(struct nx_surface *surface) {
	struct nx_surface_state *pending = &surface->pending;

	if (pending->committed & NX_SURFACE_STATE_BUFFER) {
		take_pending_buffer(surface);
		pending->buffer_width = pending->buffer != NULL ? pending->buffer->width : 0;
		pending->buffer_height = pending->buffer != NULL ? pending->buffer->height : 0;
	}

	check_buffer_scale(surface);
	finalize_pending_size(pending);

	pixman_region32_intersect_rect(&pending->surface_damage,
		&pending->surface_damage, 0, 0, pending->width, pending->height);
	pixman_region32_intersect_rect(&pending->buffer_damage,
		&pending->buffer_damage, 0, 0, pending->buffer_width,
		pending->buffer_height);
}

static void move_synced_states(struct nx_surface_state *state,
		struct nx_surface_state *next, struct nx_surface *surface) {
	void **state_synced = state->synced.data;
	void **next_synced = next->synced.data;
	struct nx_surface_synced *synced;
	wl_list_for_each(synced, &surface->synced, link) {
		surface_synced_move_state(synced,
			state_synced[synced->index], next_synced[synced->index]);
	}
}

static void move_subsurface_order(struct wl_list *order,
		struct wl_list *next_order, struct nx_surface_state *state) {
	struct nx_subsurface_parent_state *sub_state_next, *sub_state;
	wl_list_for_each(sub_state_next, next_order, link) {
		sub_state = nx_surface_synced_get_state(sub_state_next->synced, state);
		wl_list_remove(&sub_state->link);
		wl_list_insert(order->prev, &sub_state->link);
	}
}

static void move_regions(struct nx_surface_state *state, struct nx_surface_state *next) {
	if (next->committed & NX_SURFACE_STATE_SURFACE_DAMAGE) {
		pixman_region32_copy(&state->surface_damage, &next->surface_damage);
		pixman_region32_clear(&next->surface_damage);
	} else {
		pixman_region32_clear(&state->surface_damage);
	}
	if (next->committed & NX_SURFACE_STATE_BUFFER_DAMAGE) {
		pixman_region32_copy(&state->buffer_damage, &next->buffer_damage);
		pixman_region32_clear(&next->buffer_damage);
	} else {
		pixman_region32_clear(&state->buffer_damage);
	}
	if (next->committed & NX_SURFACE_STATE_OPAQUE_REGION) {
		pixman_region32_copy(&state->opaque, &next->opaque);
	}
	if (next->committed & NX_SURFACE_STATE_INPUT_REGION) {
		pixman_region32_copy(&state->input, &next->input);
	}
}

// Moves next into state
void surface_state_move(struct nx_surface_state *state,
		struct nx_surface_state *next, struct nx_surface *surface) {
	state->width = next->width;
	state->height = next->height;
	state->buffer_width = next->buffer_width;
	state->buffer_height = next->buffer_height;

	if (next->committed & NX_SURFACE_STATE_SCALE) {
		state->scale = next->scale;
	}
	if (next->committed & NX_SURFACE_STATE_TRANSFORM) {
		state->transform = next->transform;
	}
	if (next->committed & NX_SURFACE_STATE_OFFSET) {
		state->dx = next->dx;
		state->dy = next->dy;
		next->dx = next->dy = 0;
	} else {
		state->dx = state->dy = 0;
	}
	if (next->committed & NX_SURFACE_STATE_BUFFER) {
		nx_buffer_unlock(state->buffer);
		state->buffer = next->buffer;
		next->buffer = NULL;
	}
	move_regions(state, next);
	if (next->committed & NX_SURFACE_STATE_VIEWPORT) {
		state->viewport = next->viewport;
	}
	if (next->committed & NX_SURFACE_STATE_FRAME_CALLBACK_LIST) {
		wl_list_insert_list(&state->frame_callback_list,
			&next->frame_callback_list);
		wl_list_init(&next->frame_callback_list);
	}

	move_synced_states(state, next, surface);
	move_subsurface_order(&state->subsurfaces_below, &next->subsurfaces_below, state);
	move_subsurface_order(&state->subsurfaces_above, &next->subsurfaces_above, state);

	state->committed = next->committed;
	next->committed = 0;

	state->seq = next->seq;

	state->cached_state_locks = next->cached_state_locks;
	next->cached_state_locks = 0;
}

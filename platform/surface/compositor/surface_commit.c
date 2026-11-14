#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "platform/buffer/buffer.h"
#include "platform/geometry/region.h"
#include "platform/geometry/transform.h"
#include "platform/log/log.h"
#include "platform/surface/compositor/surface.h"
#include "platform/surface/subsurface/subsurface.h"

static void surface_update_damage(pixman_region32_t *buffer_damage,
		struct nx_surface_state *state) {
	pixman_region32_clear(buffer_damage);

	pixman_region32_t surface_damage;
	pixman_region32_init(&surface_damage);
	pixman_region32_copy(&surface_damage, &state->surface_damage);

	if (state->viewport.has_dst) {
		int src_width, src_height;
		surface_state_viewport_src_size(state, &src_width, &src_height);
		float scale_x = (float)state->viewport.dst_width / src_width;
		float scale_y = (float)state->viewport.dst_height / src_height;
		nx_region_scale_xy(&surface_damage, &surface_damage,
			(float)(1.0 / scale_x), (float)(1.0 / scale_y));
	}
	if (state->viewport.has_src) {
		// Lossy, best effort
		pixman_region32_translate(&surface_damage,
			(int)floor(state->viewport.src.x),
			(int)floor(state->viewport.src.y));
	}

	nx_region_scale(&surface_damage, &surface_damage, state->scale);

	int width, height;
	surface_state_transformed_buffer_size(state, &width, &height);
	nx_region_transform(&surface_damage, &surface_damage,
		nx_output_transform_invert(state->transform),
		width, height);

	pixman_region32_union(buffer_damage,
		&state->buffer_damage, &surface_damage);

	pixman_region32_fini(&surface_damage);
}

static void release_client_buffer(struct nx_surface *surface) {
	if (surface->buffer != NULL) {
		nx_buffer_unlock(&surface->buffer->base);
	}
	surface->buffer = NULL;
}

static void surface_apply_damage(struct nx_surface *surface) {
	struct nx_buffer *committed = surface->current.buffer;
	if (committed == NULL) {
		release_client_buffer(surface);
		surface->opaque = false;
		return;
	}

	surface->opaque = nx_buffer_is_opaque(committed);

	if (surface->buffer != NULL && nx_client_buffer_apply_damage(surface->buffer,
			committed, &surface->buffer_damage)) {
		nx_buffer_unlock(committed);
		surface->current.buffer = NULL;
		return;
	}

	if (surface->compositor->renderer == NULL) {
		return;
	}

	struct nx_client_buffer *buffer = nx_client_buffer_create(
		committed, surface->compositor->renderer);
	if (buffer == NULL) {
		// Show nothing, not stale content
		nx_log(NX_ERROR, "Failed to upload buffer");
	}
	release_client_buffer(surface);
	surface->buffer = buffer;
}

static void surface_update_opaque_region(struct nx_surface *surface) {
	if (!nx_surface_has_buffer(surface)) {
		pixman_region32_clear(&surface->opaque_region);
		return;
	}

	if (surface->opaque) {
		pixman_region32_fini(&surface->opaque_region);
		pixman_region32_init_rect(&surface->opaque_region,
			0, 0, surface->current.width, surface->current.height);
		return;
	}

	pixman_region32_intersect_rect(&surface->opaque_region,
		&surface->current.opaque,
		0, 0, surface->current.width, surface->current.height);
}

static void surface_update_input_region(struct nx_surface *surface) {
	pixman_region32_intersect_rect(&surface->input_region,
		&surface->current.input,
		0, 0, surface->current.width, surface->current.height);
}

static bool create_cached_synced_states(struct nx_surface *surface,
		struct nx_surface_state *cached) {
	void **cached_synced = cached->synced.data;
	struct nx_surface_synced *synced;
	wl_list_for_each(synced, &surface->synced, link) {
		void *synced_state = surface_synced_create_state(synced);
		if (synced_state == NULL) {
			return false;
		}
		cached_synced[synced->index] = synced_state;
	}
	return true;
}

static void surface_cache_pending(struct nx_surface *surface) {
	struct nx_surface_state *cached = calloc(1, sizeof(*cached));
	if (!cached) {
		goto error;
	}

	if (!surface_state_init(cached, surface)) {
		goto error_cached;
	}

	if (!create_cached_synced_states(surface, cached)) {
		goto error_state;
	}

	surface_state_move(cached, &surface->pending, surface);

	wl_list_insert(surface->cached.prev, &cached->cached_state_link);

	surface->pending.seq++;

	return;

error_state:
	surface_state_finish(cached);
error_cached:
	free(cached);
error:
	wl_resource_post_no_memory(surface->resource);
}

static void notify_subsurfaces(struct nx_surface *surface) {
	struct nx_subsurface *subsurface;
	wl_list_for_each(subsurface, &surface->current.subsurfaces_below, current.link) {
		subsurface_handle_parent_commit(subsurface);
	}
	wl_list_for_each(subsurface, &surface->current.subsurfaces_above, current.link) {
		subsurface_handle_parent_commit(subsurface);
	}
}

static void surface_commit_state(struct nx_surface *surface,
		struct nx_surface_state *next) {
	assert(next->cached_state_locks == 0);

	bool buffer_committed = next->committed & NX_SURFACE_STATE_BUFFER;

	if (buffer_committed && next->buffer == NULL) {
		surface->unmap_commit = surface->mapped;
		nx_surface_unmap(surface);
	} else {
		surface->unmap_commit = false;
	}

	surface_state_move(&surface->current, next, surface);
	surface_update_damage(&surface->buffer_damage, &surface->current);

	if (buffer_committed) {
		surface_apply_damage(surface);
	}
	surface_update_opaque_region(surface);
	surface_update_input_region(surface);

	notify_subsurfaces(surface);

	// Let listeners lock new pending
	if (next == &surface->pending) {
		surface->pending.seq++;
	}

	struct nx_surface_synced *synced;
	wl_list_for_each(synced, &surface->synced, link) {
		if (synced->impl->commit) {
			synced->impl->commit(synced);
		}
	}

	if (surface_role_active(surface) && surface->role->commit != NULL) {
		surface->role->commit(surface);
	}

	wl_signal_emit_mutable(&surface->events.commit, surface);

	// Lets shm buffers release early
	nx_buffer_unlock(surface->current.buffer);
	surface->current.buffer = NULL;
}

void surface_handle_commit(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_surface *surface = nx_surface_from_resource(resource);
	surface->handling_commit = true;

	surface_finalize_pending(surface);

	if (surface_role_active(surface) && surface->role->client_commit != NULL) {
		surface->role->client_commit(surface);
	}

	wl_signal_emit_mutable(&surface->events.client_commit, NULL);

	surface->handling_commit = false;
	if (surface->pending_rejected) {
		return;
	}

	if (surface->pending.cached_state_locks > 0 || !wl_list_empty(&surface->cached)) {
		surface_cache_pending(surface);
		return;
	}
	surface_commit_state(surface, &surface->pending);
}

uint32_t nx_surface_lock_pending(struct nx_surface *surface) {
	surface->pending.cached_state_locks++;
	return surface->pending.seq;
}

static struct nx_surface_state *find_cached_state(struct nx_surface *surface,
		uint32_t seq) {
	struct nx_surface_state *cached;
	wl_list_for_each(cached, &surface->cached, cached_state_link) {
		if (cached->seq == seq) {
			return cached;
		}
	}
	return NULL;
}

void nx_surface_unlock_cached(struct nx_surface *surface, uint32_t seq) {
	if (surface->pending.seq == seq) {
		assert(surface->pending.cached_state_locks > 0);
		surface->pending.cached_state_locks--;
		return;
	}

	struct nx_surface_state *cached = find_cached_state(surface, seq);
	assert(cached != NULL);

	assert(cached->cached_state_locks > 0);
	cached->cached_state_locks--;

	if (cached->cached_state_locks != 0) {
		return;
	}

	// Blocked on an older state
	if (cached->cached_state_link.prev != &surface->cached) {
		return;
	}

	struct nx_surface_state *next, *tmp;
	wl_list_for_each_safe(next, tmp, &surface->cached, cached_state_link) {
		if (next->cached_state_locks > 0) {
			break;
		}

		surface_commit_state(surface, next);
		surface_state_destroy_cached(next, surface);
	}
}

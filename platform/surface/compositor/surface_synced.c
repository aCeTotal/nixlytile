#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "platform/container/array.h"
#include "platform/surface/compositor/surface.h"

void *surface_synced_create_state(struct nx_surface_synced *synced) {
	void *state = calloc(1, synced->impl->state_size);
	if (state == NULL) {
		return NULL;
	}
	if (synced->impl->init_state) {
		synced->impl->init_state(state);
	}
	return state;
}

void surface_synced_destroy_state(struct nx_surface_synced *synced,
		void *state) {
	if (state == NULL) {
		return;
	}
	if (synced->impl->finish_state) {
		synced->impl->finish_state(state);
	}
	free(state);
}

void surface_synced_move_state(struct nx_surface_synced *synced,
		void *dst, void *src) {
	if (synced->impl->move_state) {
		synced->impl->move_state(dst, src);
		return;
	}
	memcpy(dst, src, synced->impl->state_size);
}

static bool surface_state_add_synced(struct nx_surface_state *state, void *value) {
	void **ptr = wl_array_add(&state->synced, sizeof(void *));
	if (ptr == NULL) {
		return false;
	}
	*ptr = value;
	return true;
}

static void *surface_state_remove_synced(struct nx_surface_state *state,
		struct nx_surface_synced *synced) {
	void **synced_states = state->synced.data;
	void *synced_state = synced_states[synced->index];
	array_remove_at(&state->synced, synced->index * sizeof(void *), sizeof(void *));
	return synced_state;
}

static void surface_state_remove_and_destroy_synced(struct nx_surface_state *state,
		struct nx_surface_synced *synced) {
	void *synced_state = surface_state_remove_synced(state, synced);
	surface_synced_destroy_state(synced, synced_state);
}

static void remove_cached_states_before(struct nx_surface_synced *synced,
		struct nx_surface_state *failed_at) {
	struct nx_surface_state *cached;
	wl_list_for_each(cached, &synced->surface->cached, cached_state_link) {
		if (cached == failed_at) {
			return;
		}
		surface_state_remove_and_destroy_synced(cached, synced);
	}
}

static bool add_cached_states(struct nx_surface_synced *synced) {
	struct nx_surface_state *cached;
	wl_list_for_each(cached, &synced->surface->cached, cached_state_link) {
		void *synced_state = surface_synced_create_state(synced);
		if (synced_state != NULL && surface_state_add_synced(cached, synced_state)) {
			continue;
		}
		surface_synced_destroy_state(synced, synced_state);
		remove_cached_states_before(synced, cached);
		return false;
	}
	return true;
}

bool nx_surface_synced_init(struct nx_surface_synced *synced,
		struct nx_surface *surface, const struct nx_surface_synced_impl *impl,
		void *pending, void *current) {
	assert(impl->state_size > 0);

	struct nx_surface_synced *other;
	wl_list_for_each(other, &surface->synced, link) {
		assert(synced != other);
	}

	*synced = (struct nx_surface_synced){
		.surface = surface,
		.impl = impl,
		.index = surface->synced_len,
	};

	memset(pending, 0, impl->state_size);
	memset(current, 0, impl->state_size);
	if (impl->init_state) {
		impl->init_state(pending);
		impl->init_state(current);
	}
	if (!surface_state_add_synced(&surface->pending, pending)) {
		goto error_init;
	}
	if (!surface_state_add_synced(&surface->current, current)) {
		goto error_pending;
	}
	if (!add_cached_states(synced)) {
		goto error_current;
	}

	wl_list_insert(&surface->synced, &synced->link);
	surface->synced_len++;

	return true;

error_current:
	surface_state_remove_synced(&surface->current, synced);
error_pending:
	surface_state_remove_synced(&surface->pending, synced);
error_init:
	if (impl->finish_state) {
		impl->finish_state(pending);
		impl->finish_state(current);
	}
	return false;
}

void nx_surface_synced_finish(struct nx_surface_synced *synced) {
	struct nx_surface *surface = synced->surface;

	bool found = false;
	struct nx_surface_synced *other;
	wl_list_for_each(other, &surface->synced, link) {
		if (other == synced) {
			found = true;
		} else if (other->index > synced->index) {
			other->index--;
		}
	}
	assert(found);

	struct nx_surface_state *cached;
	wl_list_for_each(cached, &surface->cached, cached_state_link) {
		surface_state_remove_and_destroy_synced(cached, synced);
	}

	void *pending = surface_state_remove_synced(&surface->pending, synced);
	void *current = surface_state_remove_synced(&surface->current, synced);
	if (synced->impl->finish_state) {
		synced->impl->finish_state(pending);
		synced->impl->finish_state(current);
	}

	wl_list_remove(&synced->link);
	surface->synced_len--;
}

void *nx_surface_synced_get_state(struct nx_surface_synced *synced,
		const struct nx_surface_state *state) {
	void **synced_states = state->synced.data;
	return synced_states[synced->index];
}

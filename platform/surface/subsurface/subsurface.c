#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <wayland-server-protocol.h>
#include "platform/surface/subsurface/subsurface.h"

bool subsurface_is_synchronized(struct nx_subsurface *subsurface) {
	while (subsurface != NULL) {
		if (subsurface->synchronized) {
			return true;
		}

		subsurface = subsurface_try_from_surface(subsurface->parent);
	}

	return false;
}

static void subsurface_destroy(struct nx_subsurface *subsurface) {
	if (subsurface->has_cache) {
		nx_surface_unlock_cached(subsurface->surface, subsurface->cached_seq);
	}

	nx_surface_unmap(subsurface->surface);

	wl_signal_emit_mutable(&subsurface->events.destroy, subsurface);

	assert(wl_list_empty(&subsurface->events.destroy.listener_list));

	nx_surface_synced_finish(&subsurface->parent_synced);

	wl_list_remove(&subsurface->surface_client_commit.link);
	wl_list_remove(&subsurface->parent_destroy.link);

	wl_resource_set_user_data(subsurface->resource, NULL);
	free(subsurface);
}

void subsurface_consider_map(struct nx_subsurface *subsurface) {
	if (subsurface->added && subsurface->parent->mapped &&
			nx_surface_has_buffer(subsurface->surface)) {
		nx_surface_map(subsurface->surface);
	}
}

static void subsurface_role_commit(struct nx_surface *surface) {
	struct nx_subsurface *subsurface = subsurface_try_from_surface(surface);
	if (subsurface == NULL) {
		return;
	}

	subsurface_consider_map(subsurface);
}

static void subsurface_role_destroy(struct nx_surface *surface) {
	struct nx_subsurface *subsurface = subsurface_try_from_surface(surface);
	if (subsurface == NULL) {
		return;
	}

	subsurface_destroy(subsurface);
}

static const struct nx_surface_role subsurface_role = {
	.name = "wl_subsurface",
	.commit = subsurface_role_commit,
	.destroy = subsurface_role_destroy,
};

struct nx_subsurface *subsurface_try_from_surface(struct nx_surface *surface) {
	if (surface->role != &subsurface_role || surface->role_resource == NULL) {
		return NULL;
	}
	return subsurface_from_resource(surface->role_resource);
}

static void parent_state_init(void *_state) {
	struct nx_subsurface_parent_state *state = _state;
	wl_list_init(&state->link);
}

static void parent_state_finish(void *_state) {
	struct nx_subsurface_parent_state *state = _state;
	wl_list_remove(&state->link);
}

// Parent commit moves list position
static void parent_state_move(void *_dst, void *_src) {
	struct nx_subsurface_parent_state *dst = _dst, *src = _src;
	dst->x = src->x;
	dst->y = src->y;
	dst->synced = src->synced;
}

static const struct nx_surface_synced_impl parent_synced_impl = {
	.state_size = sizeof(struct nx_subsurface_parent_state),
	.init_state = parent_state_init,
	.finish_state = parent_state_finish,
	.move_state = parent_state_move,
};

// Inert without a parent
static void subsurface_handle_parent_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_subsurface *subsurface =
		wl_container_of(listener, subsurface, parent_destroy);
	subsurface_destroy(subsurface);
}

static void subsurface_handle_surface_client_commit(
		struct wl_listener *listener, void *data) {
	struct nx_subsurface *subsurface =
		wl_container_of(listener, subsurface, surface_client_commit);
	struct nx_surface *surface = subsurface->surface;

	bool synchronized = subsurface_is_synchronized(subsurface);
	if (!synchronized && subsurface->has_cache) {
		nx_surface_unlock_cached(surface, subsurface->cached_seq);
		subsurface->has_cache = false;
	}
	// One held commit blocks others
	if (!synchronized || subsurface->has_cache) {
		return;
	}
	subsurface->has_cache = true;
	subsurface->cached_seq = nx_surface_lock_pending(surface);
}

void subsurface_handle_parent_commit(struct nx_subsurface *subsurface) {
	if (subsurface->synchronized && subsurface->has_cache) {
		nx_surface_unlock_cached(subsurface->surface, subsurface->cached_seq);
		subsurface->has_cache = false;
	}

	if (subsurface->added) {
		return;
	}
	subsurface->added = true;
	wl_signal_emit_mutable(&subsurface->parent->events.new_subsurface,
		subsurface);
	subsurface_consider_map(subsurface);
}

// Lets state moves find subsurfaces
static void link_parent_states(struct nx_subsurface *subsurface,
		struct nx_surface *parent) {
	subsurface->pending.synced = &subsurface->parent_synced;
	subsurface->current.synced = &subsurface->parent_synced;

	struct nx_surface_state *cached;
	wl_list_for_each(cached, &parent->cached, cached_state_link) {
		struct nx_subsurface_parent_state *sub_state =
			nx_surface_synced_get_state(&subsurface->parent_synced, cached);
		sub_state->synced = &subsurface->parent_synced;
	}
}

static void subsurface_attach(struct nx_subsurface *subsurface,
		struct nx_surface *surface, struct nx_surface *parent) {
	nx_surface_set_role_object(surface, subsurface->resource);

	wl_signal_init(&subsurface->events.destroy);

	wl_signal_add(&surface->events.client_commit,
		&subsurface->surface_client_commit);
	subsurface->surface_client_commit.notify =
		subsurface_handle_surface_client_commit;

	subsurface->parent = parent;
	wl_signal_add(&parent->events.destroy, &subsurface->parent_destroy);
	subsurface->parent_destroy.notify = subsurface_handle_parent_destroy;

	wl_list_remove(&subsurface->pending.link);
	wl_list_insert(parent->pending.subsurfaces_above.prev,
		&subsurface->pending.link);
}

void subcompositor_get_subsurface(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource,
		struct wl_resource *parent_resource) {
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);
	struct nx_surface *parent = nx_surface_from_resource(parent_resource);

	struct nx_subsurface *subsurface = calloc(1, sizeof(*subsurface));
	if (!subsurface) {
		wl_client_post_no_memory(client);
		return;
	}

	if (!nx_surface_set_role(surface, &subsurface_role,
			resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE)) {
		free(subsurface);
		return;
	}

	if (nx_surface_get_root_surface(parent) == surface) {
		wl_resource_post_error(resource,
			WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
			"wl_subsurface@%" PRIu32
			" cannot be a parent of itself or its ancestor", id);
		free(subsurface);
		return;
	}

	if (!nx_surface_synced_init(&subsurface->parent_synced, parent, &parent_synced_impl,
			&subsurface->pending, &subsurface->current)) {
		free(subsurface);
		wl_client_post_no_memory(client);
		return;
	}

	subsurface->synchronized = true;
	subsurface->surface = surface;
	subsurface->resource = wl_resource_create(client, &wl_subsurface_interface,
		wl_resource_get_version(resource), id);
	if (subsurface->resource == NULL) {
		nx_surface_synced_finish(&subsurface->parent_synced);
		free(subsurface);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(subsurface->resource,
		&subsurface_implementation, subsurface, NULL);

	link_parent_states(subsurface, parent);
	subsurface_attach(subsurface, surface, parent);
}

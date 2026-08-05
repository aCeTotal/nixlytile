#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_fifo_v1.h>
#include <wlr/util/addon.h>
#include "fifo-v1-protocol.h"

#define FIFO_V1_VERSION 1

// A barrier is normally cleared when the surface content is latched for
// display. If that never happens (surface off-screen or fully occluded), the
// barrier is cleared anyway after this many milliseconds so that clients keep
// making forward progress, as allowed by the protocol.
#define FIFO_BARRIER_TIMEOUT_MS 100

struct wlr_fifo_v1_state {
	bool set_barrier;
	bool wait_barrier;
};

struct wlr_fifo_v1 {
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wlr_addon addon;
	struct wlr_surface_synced synced;
	struct wlr_fifo_v1_state pending, current;

	struct wl_event_loop *event_loop;
	struct wl_event_source *timeout;
	struct wl_event_source *idle;

	// A fifo_barrier condition is present on the surface
	bool barrier;
	// Commits held back by the barrier, in commit order
	struct wl_list commits; // wlr_fifo_v1_commit.link

	struct wl_listener client_commit;
};

struct wlr_fifo_v1_commit {
	struct wlr_surface *surface;
	uint32_t cached_seq;
	struct wl_list link; // wlr_fifo_v1.commits
};

static const struct wp_fifo_v1_interface fifo_impl;

// Returns NULL if the wl_surface was destroyed
static struct wlr_fifo_v1 *fifo_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_fifo_v1_interface, &fifo_impl));
	return wl_resource_get_user_data(resource);
}

static void fifo_disarm_timeout(struct wlr_fifo_v1 *fifo) {
	if (fifo->timeout != NULL) {
		wl_event_source_timer_update(fifo->timeout, 0);
	}
}

static void fifo_arm_timeout(struct wlr_fifo_v1 *fifo) {
	if (fifo->timeout != NULL) {
		wl_event_source_timer_update(fifo->timeout, FIFO_BARRIER_TIMEOUT_MS);
	}
}

// Clear the barrier and release the commit waiting on it, if any. Applying
// that commit may set the barrier again (via the synced commit hook), in which
// case the commits behind it stay queued.
static void fifo_clear_barrier(struct wlr_fifo_v1 *fifo) {
	fifo->barrier = false;

	if (wl_list_empty(&fifo->commits)) {
		fifo_disarm_timeout(fifo);
		return;
	}

	struct wlr_fifo_v1_commit *commit =
		wl_container_of(fifo->commits.next, commit, link);
	wl_list_remove(&commit->link);
	wlr_surface_unlock_cached(commit->surface, commit->cached_seq);
	free(commit);

	if (wl_list_empty(&fifo->commits)) {
		fifo_disarm_timeout(fifo);
	} else {
		fifo_arm_timeout(fifo);
	}
}

static void fifo_handle_idle(void *data) {
	struct wlr_fifo_v1 *fifo = data;
	fifo->idle = NULL;
	fifo_clear_barrier(fifo);
}

static int fifo_handle_timeout(void *data) {
	struct wlr_fifo_v1 *fifo = data;
	fifo_clear_barrier(fifo);
	return 0;
}

static void fifo_destroy(struct wlr_fifo_v1 *fifo) {
	if (fifo == NULL) {
		return;
	}

	struct wlr_fifo_v1_commit *commit, *commit_tmp;
	wl_list_for_each_safe(commit, commit_tmp, &fifo->commits, link) {
		wl_list_remove(&commit->link);
		wlr_surface_unlock_cached(commit->surface, commit->cached_seq);
		free(commit);
	}

	if (fifo->timeout != NULL) {
		wl_event_source_remove(fifo->timeout);
	}
	if (fifo->idle != NULL) {
		wl_event_source_remove(fifo->idle);
	}

	wl_list_remove(&fifo->client_commit.link);
	wlr_surface_synced_finish(&fifo->synced);
	wlr_addon_finish(&fifo->addon);
	wl_resource_set_user_data(fifo->resource, NULL);
	free(fifo);
}

static void fifo_handle_resource_destroy(struct wl_resource *resource) {
	fifo_destroy(fifo_from_resource(resource));
}

static void fifo_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void fifo_handle_set_barrier(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo = fifo_from_resource(resource);
	if (fifo == NULL) {
		wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
			"The wl_surface object has been destroyed");
		return;
	}
	fifo->pending.set_barrier = true;
}

static void fifo_handle_wait_barrier(struct wl_client *client,
		struct wl_resource *resource) {
	struct wlr_fifo_v1 *fifo = fifo_from_resource(resource);
	if (fifo == NULL) {
		wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
			"The wl_surface object has been destroyed");
		return;
	}
	fifo->pending.wait_barrier = true;
}

static const struct wp_fifo_v1_interface fifo_impl = {
	.destroy = fifo_handle_destroy,
	.set_barrier = fifo_handle_set_barrier,
	.wait_barrier = fifo_handle_wait_barrier,
};

static void fifo_handle_client_commit(struct wl_listener *listener, void *data) {
	struct wlr_fifo_v1 *fifo = wl_container_of(listener, fifo, client_commit);

	// Hold the commit back while a barrier is present. Commits queued behind
	// an already held commit must wait too, so that they stay in order.
	if (!fifo->pending.wait_barrier ||
			(!fifo->barrier && wl_list_empty(&fifo->commits))) {
		return;
	}

	struct wlr_fifo_v1_commit *commit = calloc(1, sizeof(*commit));
	if (commit == NULL) {
		wl_resource_post_no_memory(fifo->resource);
		return;
	}

	commit->surface = fifo->surface;
	commit->cached_seq = wlr_surface_lock_pending(fifo->surface);
	wl_list_insert(fifo->commits.prev, &commit->link);

	fifo_arm_timeout(fifo);
}

static void fifo_synced_move_state(void *_dst, void *_src) {
	struct wlr_fifo_v1_state *dst = _dst, *src = _src;
	*dst = *src;
	// set_barrier/wait_barrier only apply to the commit they were made on
	*src = (struct wlr_fifo_v1_state){0};
}

static void fifo_synced_commit(struct wlr_surface_synced *synced) {
	struct wlr_fifo_v1 *fifo = wl_container_of(synced, fifo, synced);
	if (fifo->current.set_barrier) {
		fifo->barrier = true;
		fifo_arm_timeout(fifo);
	}
}

static const struct wlr_surface_synced_impl fifo_synced_impl = {
	.state_size = sizeof(struct wlr_fifo_v1_state),
	.move_state = fifo_synced_move_state,
	.commit = fifo_synced_commit,
};

static void fifo_addon_destroy(struct wlr_addon *addon) {
	struct wlr_fifo_v1 *fifo = wl_container_of(addon, fifo, addon);
	fifo_destroy(fifo);
}

static const struct wlr_addon_interface fifo_addon_impl = {
	.name = "wp_fifo_v1",
	.destroy = fifo_addon_destroy,
};

static struct wlr_fifo_v1 *fifo_from_wlr_surface(struct wlr_surface *wlr_surface) {
	struct wlr_addon *addon =
		wlr_addon_find(&wlr_surface->addons, NULL, &fifo_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct wlr_fifo_v1 *fifo = wl_container_of(addon, fifo, addon);
	return fifo;
}

void wlr_fifo_v1_surface_latched(struct wlr_surface *surface) {
	struct wlr_fifo_v1 *fifo = fifo_from_wlr_surface(surface);
	if (fifo == NULL || (!fifo->barrier && wl_list_empty(&fifo->commits))) {
		return;
	}
	if (fifo->idle != NULL) {
		return;
	}

	// Clearing the barrier applies a cached commit, which mutates the scene
	// graph. This can be called from within output rendering, so defer it.
	fifo->idle = wl_event_loop_add_idle(fifo->event_loop, fifo_handle_idle, fifo);
	if (fifo->idle == NULL) {
		fifo_clear_barrier(fifo);
	}
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void manager_handle_get_fifo(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct wlr_surface *wlr_surface = wlr_surface_from_resource(surface_resource);

	if (fifo_from_wlr_surface(wlr_surface) != NULL) {
		wl_resource_post_error(manager_resource,
			WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
			"The wl_surface object already has a wp_fifo_v1 object");
		return;
	}

	struct wlr_fifo_v1 *fifo = calloc(1, sizeof(*fifo));
	if (fifo == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	if (!wlr_surface_synced_init(&fifo->synced, wlr_surface, &fifo_synced_impl,
			&fifo->pending, &fifo->current)) {
		free(fifo);
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_resource);
	fifo->resource = wl_resource_create(client, &wp_fifo_v1_interface, version, id);
	if (fifo->resource == NULL) {
		wlr_surface_synced_finish(&fifo->synced);
		free(fifo);
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(fifo->resource, &fifo_impl, fifo,
		fifo_handle_resource_destroy);

	fifo->surface = wlr_surface;
	fifo->event_loop = wl_display_get_event_loop(wl_client_get_display(client));
	wl_list_init(&fifo->commits);

	fifo->timeout = wl_event_loop_add_timer(fifo->event_loop,
		fifo_handle_timeout, fifo);

	fifo->client_commit.notify = fifo_handle_client_commit;
	wl_signal_add(&wlr_surface->events.client_commit, &fifo->client_commit);

	wlr_addon_init(&fifo->addon, &wlr_surface->addons, NULL, &fifo_addon_impl);
}

static const struct wp_fifo_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_fifo = manager_handle_get_fifo,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&wp_fifo_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_fifo_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_fifo_manager_v1 *wlr_fifo_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= FIFO_V1_VERSION);

	struct wlr_fifo_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display, &wp_fifo_manager_v1_interface,
		version, NULL, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}

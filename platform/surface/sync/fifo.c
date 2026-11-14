#include <assert.h>
#include <stdlib.h>
#include "fifo-v1-protocol.h"
#include "platform/container/addon.h"
#include "platform/surface/compositor.h"
#include "platform/surface/fifo.h"
#include "platform/surface/sync/fifo_manager.h"

// Fallback when content never latches
#define FIFO_BARRIER_TIMEOUT_MS 100

struct nx_fifo_v1_state {
	bool set_barrier;
	bool wait_barrier;
};

struct nx_fifo_v1 {
	struct wl_resource *resource;
	struct nx_surface *surface;
	struct nx_addon addon;
	struct nx_surface_synced synced;
	struct nx_fifo_v1_state pending, current;

	struct wl_event_loop *event_loop;
	struct wl_event_source *timeout;
	struct wl_event_source *idle;

	bool barrier;
	struct wl_list commits; // nx_fifo_v1_commit.link, in commit order

	struct wl_listener client_commit;
};

struct nx_fifo_v1_commit {
	uint32_t cached_seq;
	struct wl_list link; // nx_fifo_v1.commits
};

static const struct wp_fifo_v1_interface fifo_impl;

// NULL once wl_surface is gone
static struct nx_fifo_v1 *fifo_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_fifo_v1_interface, &fifo_impl));
	return wl_resource_get_user_data(resource);
}

static void fifo_disarm_timeout(struct nx_fifo_v1 *fifo) {
	if (fifo->timeout != NULL) {
		wl_event_source_timer_update(fifo->timeout, 0);
	}
}

static void fifo_arm_timeout(struct nx_fifo_v1 *fifo) {
	if (fifo->timeout != NULL) {
		wl_event_source_timer_update(fifo->timeout, FIFO_BARRIER_TIMEOUT_MS);
	}
}

static void fifo_release_commit(struct nx_fifo_v1 *fifo, struct nx_fifo_v1_commit *commit) {
	wl_list_remove(&commit->link);
	nx_surface_unlock_cached(fifo->surface, commit->cached_seq);
	free(commit);
}

// Drain until a barrier reappears
static void fifo_clear_barrier(struct nx_fifo_v1 *fifo) {
	fifo->barrier = false;

	while (!fifo->barrier && !wl_list_empty(&fifo->commits)) {
		struct nx_fifo_v1_commit *commit =
			wl_container_of(fifo->commits.next, commit, link);
		fifo_release_commit(fifo, commit);
	}

	if (wl_list_empty(&fifo->commits)) {
		fifo_disarm_timeout(fifo);
		return;
	}
	fifo_arm_timeout(fifo);
}

static void fifo_handle_idle(void *data) {
	struct nx_fifo_v1 *fifo = data;
	fifo->idle = NULL;
	fifo_clear_barrier(fifo);
}

static int fifo_handle_timeout(void *data) {
	struct nx_fifo_v1 *fifo = data;
	fifo_clear_barrier(fifo);
	return 0;
}

static void fifo_destroy(struct nx_fifo_v1 *fifo) {
	if (fifo == NULL) {
		return;
	}

	struct nx_fifo_v1_commit *commit, *commit_tmp;
	wl_list_for_each_safe(commit, commit_tmp, &fifo->commits, link) {
		fifo_release_commit(fifo, commit);
	}

	if (fifo->timeout != NULL) {
		wl_event_source_remove(fifo->timeout);
	}
	if (fifo->idle != NULL) {
		wl_event_source_remove(fifo->idle);
	}

	wl_list_remove(&fifo->client_commit.link);
	nx_surface_synced_finish(&fifo->synced);
	nx_addon_finish(&fifo->addon);
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
	struct nx_fifo_v1 *fifo = fifo_from_resource(resource);
	if (fifo == NULL) {
		wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
			"The wl_surface object has been destroyed");
		return;
	}
	fifo->pending.set_barrier = true;
}

static void fifo_handle_wait_barrier(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_fifo_v1 *fifo = fifo_from_resource(resource);
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
	struct nx_fifo_v1 *fifo = wl_container_of(listener, fifo, client_commit);

	// Queued commits keep their order
	if (!fifo->pending.wait_barrier ||
			(!fifo->barrier && wl_list_empty(&fifo->commits))) {
		return;
	}

	struct nx_fifo_v1_commit *commit = calloc(1, sizeof(*commit));
	if (commit == NULL) {
		wl_resource_post_no_memory(fifo->resource);
		return;
	}

	commit->cached_seq = nx_surface_lock_pending(fifo->surface);
	wl_list_insert(fifo->commits.prev, &commit->link);

	fifo_arm_timeout(fifo);
}

// Barriers cover one commit
static void fifo_synced_move_state(void *_dst, void *_src) {
	struct nx_fifo_v1_state *dst = _dst, *src = _src;
	*dst = *src;
	*src = (struct nx_fifo_v1_state){0};
}

static void fifo_synced_commit(struct nx_surface_synced *synced) {
	struct nx_fifo_v1 *fifo = wl_container_of(synced, fifo, synced);
	if (fifo->current.set_barrier) {
		fifo->barrier = true;
		fifo_arm_timeout(fifo);
	}
}

static const struct nx_surface_synced_impl fifo_synced_impl = {
	.state_size = sizeof(struct nx_fifo_v1_state),
	.move_state = fifo_synced_move_state,
	.commit = fifo_synced_commit,
};

static void fifo_addon_destroy(struct nx_addon *addon) {
	struct nx_fifo_v1 *fifo = wl_container_of(addon, fifo, addon);
	fifo_destroy(fifo);
}

static const struct nx_addon_interface fifo_addon_impl = {
	.name = "wp_fifo_v1",
	.destroy = fifo_addon_destroy,
};

static struct nx_fifo_v1 *fifo_from_nx_surface(struct nx_surface *nx_surface) {
	struct nx_addon *addon =
		nx_addon_find(&nx_surface->addons, NULL, &fifo_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct nx_fifo_v1 *fifo = wl_container_of(addon, fifo, addon);
	return fifo;
}

void nx_fifo_v1_surface_latched(struct nx_surface *surface) {
	struct nx_fifo_v1 *fifo = fifo_from_nx_surface(surface);
	if (fifo == NULL || (!fifo->barrier && wl_list_empty(&fifo->commits))) {
		return;
	}
	if (fifo->idle != NULL) {
		return;
	}

	// May run during output rendering
	fifo->idle = wl_event_loop_add_idle(fifo->event_loop, fifo_handle_idle, fifo);
	if (fifo->idle == NULL) {
		fifo_clear_barrier(fifo);
	}
}

static void fifo_attach(struct nx_fifo_v1 *fifo, struct nx_surface *surface,
		struct wl_client *client) {
	fifo->surface = surface;
	fifo->event_loop = wl_display_get_event_loop(wl_client_get_display(client));
	wl_list_init(&fifo->commits);

	fifo->timeout = wl_event_loop_add_timer(fifo->event_loop,
		fifo_handle_timeout, fifo);

	fifo->client_commit.notify = fifo_handle_client_commit;
	wl_signal_add(&surface->events.client_commit, &fifo->client_commit);

	nx_addon_init(&fifo->addon, &surface->addons, NULL, &fifo_addon_impl);
}

void fifo_manager_get_fifo(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct nx_surface *nx_surface = nx_surface_from_resource(surface_resource);

	if (fifo_from_nx_surface(nx_surface) != NULL) {
		wl_resource_post_error(manager_resource,
			WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
			"The wl_surface object already has a wp_fifo_v1 object");
		return;
	}

	struct nx_fifo_v1 *fifo = calloc(1, sizeof(*fifo));
	if (fifo == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	if (!nx_surface_synced_init(&fifo->synced, nx_surface, &fifo_synced_impl,
			&fifo->pending, &fifo->current)) {
		free(fifo);
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_resource);
	fifo->resource = wl_resource_create(client, &wp_fifo_v1_interface, version, id);
	if (fifo->resource == NULL) {
		nx_surface_synced_finish(&fifo->synced);
		free(fifo);
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(fifo->resource, &fifo_impl, fifo,
		fifo_handle_resource_destroy);

	fifo_attach(fifo, nx_surface, client);
}

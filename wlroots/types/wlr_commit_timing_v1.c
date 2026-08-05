#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_commit_timing_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>
#include "commit-timing-v1-protocol.h"

#define COMMIT_TIMING_V1_VERSION 1

#define NSEC_PER_SEC 1000000000LL
#define NSEC_PER_MSEC 1000000LL

struct wlr_commit_timer_v1 {
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wlr_addon addon;
	struct wl_event_loop *event_loop;

	bool has_timestamp;
	struct timespec timestamp;

	struct wl_list commits; // wlr_commit_timing_commit_v1.link
	struct wl_listener client_commit;
};

struct wlr_commit_timing_commit_v1 {
	struct wlr_surface *surface;
	struct wl_event_source *timer;
	uint32_t cached_seq;
	struct wl_list link; // wlr_commit_timer_v1.commits
	struct wl_listener surface_destroy;
};

static const struct wp_commit_timer_v1_interface timer_impl;

// Returns NULL if the wl_surface was destroyed
static struct wlr_commit_timer_v1 *timer_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_commit_timer_v1_interface,
		&timer_impl));
	return wl_resource_get_user_data(resource);
}

static void commit_destroy(struct wlr_commit_timing_commit_v1 *commit) {
	wlr_surface_unlock_cached(commit->surface, commit->cached_seq);
	wl_event_source_remove(commit->timer);
	wl_list_remove(&commit->surface_destroy.link);
	wl_list_remove(&commit->link);
	free(commit);
}

static int commit_handle_timer(void *data) {
	struct wlr_commit_timing_commit_v1 *commit = data;
	commit_destroy(commit);
	return 0;
}

static void commit_handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct wlr_commit_timing_commit_v1 *commit =
		wl_container_of(listener, commit, surface_destroy);
	commit_destroy(commit);
}

static void timer_destroy(struct wlr_commit_timer_v1 *timer) {
	if (timer == NULL) {
		return;
	}

	struct wlr_commit_timing_commit_v1 *commit, *commit_tmp;
	wl_list_for_each_safe(commit, commit_tmp, &timer->commits, link) {
		commit_destroy(commit);
	}

	wl_list_remove(&timer->client_commit.link);
	wlr_addon_finish(&timer->addon);
	wl_resource_set_user_data(timer->resource, NULL);
	free(timer);
}

static void timer_handle_resource_destroy(struct wl_resource *resource) {
	timer_destroy(timer_from_resource(resource));
}

static void timer_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void timer_handle_set_timestamp(struct wl_client *client,
		struct wl_resource *resource, uint32_t tv_sec_hi, uint32_t tv_sec_lo,
		uint32_t tv_nsec) {
	struct wlr_commit_timer_v1 *timer = timer_from_resource(resource);
	if (timer == NULL) {
		wl_resource_post_error(resource,
			WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED,
			"The wl_surface object has been destroyed");
		return;
	}

	if (tv_nsec >= NSEC_PER_SEC) {
		wl_resource_post_error(resource,
			WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP,
			"Invalid nanoseconds value");
		return;
	}

	if (timer->has_timestamp) {
		wl_resource_post_error(resource,
			WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS,
			"A timestamp has already been set for this commit");
		return;
	}

	timer->has_timestamp = true;
	timer->timestamp = (struct timespec){
		.tv_sec = (time_t)(((uint64_t)tv_sec_hi << 32) | tv_sec_lo),
		.tv_nsec = tv_nsec,
	};
}

static const struct wp_commit_timer_v1_interface timer_impl = {
	.destroy = timer_handle_destroy,
	.set_timestamp = timer_handle_set_timestamp,
};

// Milliseconds until the target time, rounded up so that a commit is never
// applied before the client asked for. Zero if the deadline has passed.
static int64_t delay_until(const struct timespec *target) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	int64_t delta_ns = ((int64_t)target->tv_sec - (int64_t)now.tv_sec) * NSEC_PER_SEC +
		((int64_t)target->tv_nsec - (int64_t)now.tv_nsec);
	if (delta_ns <= 0) {
		return 0;
	}
	return (delta_ns + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC;
}

static void timer_handle_client_commit(struct wl_listener *listener, void *data) {
	struct wlr_commit_timer_v1 *timer =
		wl_container_of(listener, timer, client_commit);

	if (!timer->has_timestamp) {
		return;
	}
	timer->has_timestamp = false;

	int64_t delay_ms = delay_until(&timer->timestamp);
	if (delay_ms == 0) {
		// Deadline already passed, apply as usual
		return;
	}

	struct wlr_commit_timing_commit_v1 *commit = calloc(1, sizeof(*commit));
	if (commit == NULL) {
		wl_resource_post_no_memory(timer->resource);
		return;
	}

	commit->timer = wl_event_loop_add_timer(timer->event_loop,
		commit_handle_timer, commit);
	if (commit->timer == NULL) {
		free(commit);
		wl_resource_post_no_memory(timer->resource);
		return;
	}

	commit->surface = timer->surface;
	commit->cached_seq = wlr_surface_lock_pending(timer->surface);

	commit->surface_destroy.notify = commit_handle_surface_destroy;
	wl_signal_add(&timer->surface->events.destroy, &commit->surface_destroy);

	wl_list_insert(timer->commits.prev, &commit->link);

	if (delay_ms > INT32_MAX) {
		delay_ms = INT32_MAX;
	}
	wl_event_source_timer_update(commit->timer, (int)delay_ms);
}

static void timer_addon_destroy(struct wlr_addon *addon) {
	struct wlr_commit_timer_v1 *timer = wl_container_of(addon, timer, addon);
	timer_destroy(timer);
}

static const struct wlr_addon_interface timer_addon_impl = {
	.name = "wp_commit_timer_v1",
	.destroy = timer_addon_destroy,
};

static struct wlr_commit_timer_v1 *timer_from_wlr_surface(
		struct wlr_surface *wlr_surface) {
	struct wlr_addon *addon =
		wlr_addon_find(&wlr_surface->addons, NULL, &timer_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct wlr_commit_timer_v1 *timer = wl_container_of(addon, timer, addon);
	return timer;
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void manager_handle_get_timer(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct wlr_surface *wlr_surface = wlr_surface_from_resource(surface_resource);

	if (timer_from_wlr_surface(wlr_surface) != NULL) {
		wl_resource_post_error(manager_resource,
			WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS,
			"The wl_surface object already has a wp_commit_timer_v1 object");
		return;
	}

	struct wlr_commit_timer_v1 *timer = calloc(1, sizeof(*timer));
	if (timer == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_resource);
	timer->resource = wl_resource_create(client, &wp_commit_timer_v1_interface,
		version, id);
	if (timer->resource == NULL) {
		free(timer);
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(timer->resource, &timer_impl, timer,
		timer_handle_resource_destroy);

	timer->surface = wlr_surface;
	timer->event_loop = wl_display_get_event_loop(wl_client_get_display(client));
	wl_list_init(&timer->commits);

	timer->client_commit.notify = timer_handle_client_commit;
	wl_signal_add(&wlr_surface->events.client_commit, &timer->client_commit);

	wlr_addon_init(&timer->addon, &wlr_surface->addons, NULL, &timer_addon_impl);
}

static const struct wp_commit_timing_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_timer = manager_handle_get_timer,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&wp_commit_timing_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_commit_timing_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct wlr_commit_timing_manager_v1 *wlr_commit_timing_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= COMMIT_TIMING_V1_VERSION);

	struct wlr_commit_timing_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&wp_commit_timing_manager_v1_interface, version, NULL, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}

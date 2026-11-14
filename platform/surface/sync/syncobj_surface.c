#include <assert.h>
#include <stdlib.h>
#include "linux-drm-syncobj-v1-protocol.h"
#include "platform/render/drm_syncobj.h"
#include "platform/render/drm_syncobj_merger.h"
#include "platform/surface/compositor.h"
#include "platform/surface/sync/linux_drm_syncobj.h"
#include "platform/surface/syncobj.h"

struct nx_linux_drm_syncobj_surface_v1 {
	struct wl_resource *resource;
	struct nx_surface *surface;
	struct nx_linux_drm_syncobj_surface_v1_state pending, current;

	struct nx_addon addon;
	struct nx_surface_synced synced;

	struct wl_listener client_commit;
};

static const struct wp_linux_drm_syncobj_surface_v1_interface surface_impl;

// NULL once inert
static struct nx_linux_drm_syncobj_surface_v1 *surface_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_linux_drm_syncobj_surface_v1_interface, &surface_impl));
	return wl_resource_get_user_data(resource);
}

static void surface_destroy(struct nx_linux_drm_syncobj_surface_v1 *surface) {
	if (surface == NULL) {
		return;
	}
	wl_list_remove(&surface->client_commit.link);
	nx_addon_finish(&surface->addon);
	nx_surface_synced_finish(&surface->synced);
	wl_resource_set_user_data(surface->resource, NULL);
	free(surface);
}

static void surface_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

// NULL after posting inert error
static struct nx_linux_drm_syncobj_surface_v1 *live_surface_from_resource(
		struct wl_resource *resource) {
	struct nx_linux_drm_syncobj_surface_v1 *surface =
		surface_from_resource(resource);
	if (surface == NULL) {
		wl_resource_post_error(resource,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
			"The surface has been destroyed");
	}
	return surface;
}

static void replace_timeline(struct nx_drm_syncobj_timeline **timeline,
		struct wl_resource *timeline_resource) {
	struct nx_drm_syncobj_timeline *next =
		syncobj_timeline_from_resource(timeline_resource);
	nx_drm_syncobj_timeline_unref(*timeline);
	*timeline = nx_drm_syncobj_timeline_ref(next);
}

static void surface_handle_set_acquire_point(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *timeline_resource,
		uint32_t point_hi, uint32_t point_lo) {
	struct nx_linux_drm_syncobj_surface_v1 *surface =
		live_surface_from_resource(resource);
	if (surface == NULL) {
		return;
	}
	replace_timeline(&surface->pending.acquire_timeline, timeline_resource);
	surface->pending.acquire_point = (uint64_t)point_hi << 32 | point_lo;
}

static void surface_handle_set_release_point(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *timeline_resource,
		uint32_t point_hi, uint32_t point_lo) {
	struct nx_linux_drm_syncobj_surface_v1 *surface =
		live_surface_from_resource(resource);
	if (surface == NULL) {
		return;
	}
	replace_timeline(&surface->pending.release_timeline, timeline_resource);
	surface->pending.release_point = (uint64_t)point_hi << 32 | point_lo;
}

static const struct wp_linux_drm_syncobj_surface_v1_interface surface_impl = {
	.destroy = surface_handle_destroy,
	.set_acquire_point = surface_handle_set_acquire_point,
	.set_release_point = surface_handle_set_release_point,
};

static void surface_handle_resource_destroy(struct wl_resource *resource) {
	surface_destroy(surface_from_resource(resource));
}

static void surface_addon_handle_surface_destroy(struct nx_addon *addon) {
	struct nx_linux_drm_syncobj_surface_v1 *surface =
		wl_container_of(addon, surface, addon);
	surface_destroy(surface);
}

static const struct nx_addon_interface surface_addon_impl = {
	.name = "wp_linux_drm_syncobj_surface_v1",
	.destroy = surface_addon_handle_surface_destroy,
};

static void surface_synced_finish_state(void *_state) {
	struct nx_linux_drm_syncobj_surface_v1_state *state = _state;
	nx_drm_syncobj_timeline_unref(state->acquire_timeline);
	nx_drm_syncobj_timeline_unref(state->release_timeline);
	nx_drm_syncobj_merger_unref(state->release_merger);
}

static void surface_synced_move_state(void *_dst, void *_src) {
	struct nx_linux_drm_syncobj_surface_v1_state *dst = _dst, *src = _src;
	// Only buffer commits carry points
	if (src->acquire_timeline == NULL) {
		return;
	}
	// Dropping the merger signals release
	surface_synced_finish_state(dst);
	*dst = *src;
	dst->committed = true;
	*src = (struct nx_linux_drm_syncobj_surface_v1_state){0};
}

static void surface_synced_commit(struct nx_surface_synced *synced) {
	struct nx_linux_drm_syncobj_surface_v1 *surface = wl_container_of(synced, surface, synced);

	if (!surface->current.committed) {
		return;
	}

	surface->current.release_merger = nx_drm_syncobj_merger_create(
		surface->current.release_timeline, surface->current.release_point);
	surface->current.committed = false;
}

static const struct nx_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct nx_linux_drm_syncobj_surface_v1_state),
	.finish_state = surface_synced_finish_state,
	.move_state = surface_synced_move_state,
	.commit = surface_synced_commit,
};

static struct nx_linux_drm_syncobj_surface_v1 *surface_from_nx_surface(
		struct nx_surface *nx_surface) {
	struct nx_addon *addon =
		nx_addon_find(&nx_surface->addons, NULL, &surface_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct nx_linux_drm_syncobj_surface_v1 *surface =
		wl_container_of(addon, surface, addon);
	return surface;
}

static void drop_pending_points(struct nx_linux_drm_syncobj_surface_v1_state *pending) {
	nx_drm_syncobj_timeline_unref(pending->acquire_timeline);
	nx_drm_syncobj_timeline_unref(pending->release_timeline);
	pending->acquire_timeline = NULL;
	pending->release_timeline = NULL;
	pending->acquire_point = 0;
	pending->release_point = 0;
}

static void surface_handle_client_commit(struct wl_listener *listener,
		void *data) {
	struct nx_linux_drm_syncobj_surface_v1 *surface =
		wl_container_of(listener, surface, client_commit);
	struct nx_linux_drm_syncobj_surface_v1_state *pending = &surface->pending;
	bool has_buffer = surface->surface->pending.buffer != NULL;

	if (pending->acquire_timeline != NULL && !has_buffer) {
		nx_surface_reject_pending(surface->surface, surface->resource,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER,
			"Acquire point set but no buffer attached");
		return;
	}
	if (pending->release_timeline != NULL && !has_buffer) {
		nx_surface_reject_pending(surface->surface, surface->resource,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_BUFFER,
			"Release point set but no buffer attached");
		return;
	}

	// SDL3 omits points: implicit sync
	if ((pending->acquire_timeline == NULL || pending->release_timeline == NULL) &&
			has_buffer) {
		drop_pending_points(pending);
		return;
	}

	if (pending->acquire_timeline != NULL && pending->release_timeline != NULL &&
			pending->acquire_timeline == pending->release_timeline &&
			pending->acquire_point >= pending->release_point) {
		nx_surface_reject_pending(surface->surface, surface->resource,
			WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_CONFLICTING_POINTS,
			"Acquire and release points conflict");
		return;
	}

	if (pending->acquire_timeline != NULL && !syncobj_hold_until_acquired(
			surface->surface, pending->acquire_timeline, pending->acquire_point)) {
		wl_resource_post_no_memory(surface->resource);
	}
}

void syncobj_manager_get_surface(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct nx_surface *nx_surface = nx_surface_from_resource(surface_resource);

	if (surface_from_nx_surface(nx_surface) != NULL) {
		wl_resource_post_error(resource,
			WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"wp_linux_drm_syncobj_surface_v1 already created for this surface");
		return;
	}

	struct nx_linux_drm_syncobj_surface_v1 *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	if (!nx_surface_synced_init(&surface->synced, nx_surface,
			&surface_synced_impl, &surface->pending, &surface->current)) {
		goto error_surface;
	}

	uint32_t version = wl_resource_get_version(resource);
	surface->resource = wl_resource_create(client,
		&wp_linux_drm_syncobj_surface_v1_interface, version, id);
	if (surface->resource == NULL) {
		goto error_surface_synced;
	}
	wl_resource_set_implementation(surface->resource,
		&surface_impl, surface, surface_handle_resource_destroy);

	surface->surface = nx_surface;

	surface->client_commit.notify = surface_handle_client_commit;
	wl_signal_add(&nx_surface->events.client_commit, &surface->client_commit);

	nx_addon_init(&surface->addon, &nx_surface->addons, NULL, &surface_addon_impl);

	return;

error_surface_synced:
	nx_surface_synced_finish(&surface->synced);
error_surface:
	free(surface);
	wl_resource_post_no_memory(resource);
}

struct nx_linux_drm_syncobj_surface_v1_state *
nx_linux_drm_syncobj_v1_get_surface_state(struct nx_surface *nx_surface) {
	struct nx_linux_drm_syncobj_surface_v1 *surface =
		surface_from_nx_surface(nx_surface);
	if (surface == NULL) {
		return NULL;
	}
	return &surface->current;
}

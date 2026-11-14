#include <stdlib.h>
#include <unistd.h>
#include "platform/nested/output/output.h"

#include "linux-drm-syncobj-v1-client-protocol.h"

// Host import of a local timeline.
struct nx_wl_drm_syncobj_timeline {
	struct nx_drm_syncobj_timeline *base;
	struct nx_addon addon;
	struct wl_list link; // nx_wl_backend.drm_syncobj_timelines
	struct wp_linux_drm_syncobj_timeline_v1 *wl;
};

struct sync_point {
	struct nx_wl_drm_syncobj_timeline *timeline;
	uint64_t point;
};

static void timeline_destroy(struct nx_wl_drm_syncobj_timeline *timeline) {
	wp_linux_drm_syncobj_timeline_v1_destroy(timeline->wl);
	nx_addon_finish(&timeline->addon);
	wl_list_remove(&timeline->link);
	free(timeline);
}

static void timeline_addon_destroy(struct nx_addon *addon) {
	struct nx_wl_drm_syncobj_timeline *timeline = wl_container_of(addon, timeline, addon);
	timeline_destroy(timeline);
}

static const struct nx_addon_interface timeline_addon_impl = {
	.name = "nx_wl_drm_syncobj_timeline",
	.destroy = timeline_addon_destroy,
};

static struct nx_wl_drm_syncobj_timeline *get_or_import_timeline(
		struct nx_wl_backend *wl, struct nx_drm_syncobj_timeline *nx_timeline) {
	struct nx_addon *addon =
		nx_addon_find(&nx_timeline->addons, wl, &timeline_addon_impl);
	if (addon != NULL) {
		struct nx_wl_drm_syncobj_timeline *timeline = wl_container_of(addon, timeline, addon);
		return timeline;
	}

	struct nx_wl_drm_syncobj_timeline *timeline = calloc(1, sizeof(*timeline));
	if (timeline == NULL) {
		return NULL;
	}
	timeline->base = nx_timeline;

	int fd = nx_drm_syncobj_timeline_export(nx_timeline);
	if (fd < 0) {
		free(timeline);
		return NULL;
	}
	timeline->wl = wp_linux_drm_syncobj_manager_v1_import_timeline(
		wl->drm_syncobj_manager_v1, fd);
	close(fd);
	if (timeline->wl == NULL) {
		free(timeline);
		return NULL;
	}

	nx_addon_init(&timeline->addon, &nx_timeline->addons, wl, &timeline_addon_impl);
	wl_list_insert(&wl->drm_syncobj_timelines, &timeline->link);
	return timeline;
}

void nested_timelines_destroy(struct nx_wl_backend *wl) {
	struct nx_wl_drm_syncobj_timeline *timeline, *tmp;
	wl_list_for_each_safe(timeline, tmp, &wl->drm_syncobj_timelines, link) {
		timeline_destroy(timeline);
	}
}

// Buffer-owned timeline when none is given.
static struct sync_point fallback_release_point(struct nx_wl_backend *wl,
		struct nx_wl_buffer *buffer) {
	if (buffer->fallback_signal_timeline == NULL) {
		buffer->fallback_signal_timeline = nx_drm_syncobj_timeline_create(wl->drm_fd);
	}
	if (buffer->fallback_signal_timeline == NULL) {
		return (struct sync_point){0};
	}

	struct nx_wl_drm_syncobj_timeline *timeline =
		get_or_import_timeline(wl, buffer->fallback_signal_timeline);
	return (struct sync_point){ timeline, ++buffer->fallback_signal_point };
}

static struct sync_point release_point(struct nx_wl_backend *wl,
		const struct nx_output_state *state, struct nx_wl_buffer *buffer) {
	if (!(state->committed & NX_OUTPUT_STATE_SIGNAL_TIMELINE)) {
		return fallback_release_point(wl, buffer);
	}
	return (struct sync_point){
		get_or_import_timeline(wl, state->signal_timeline),
		state->signal_point,
	};
}

static void drop_sync_surface(struct nx_wl_output *output) {
	if (output->drm_syncobj_surface_v1 == NULL) {
		return;
	}
	wp_linux_drm_syncobj_surface_v1_destroy(output->drm_syncobj_surface_v1);
	output->drm_syncobj_surface_v1 = NULL;
}

static bool ensure_sync_surface(struct nx_wl_output *output) {
	if (output->drm_syncobj_surface_v1 != NULL) {
		return true;
	}
	output->drm_syncobj_surface_v1 = wp_linux_drm_syncobj_manager_v1_get_surface(
		output->backend->drm_syncobj_manager_v1, output->surface);
	return output->drm_syncobj_surface_v1 != NULL;
}

bool nested_sync_apply(struct nx_wl_output *output,
		const struct nx_output_state *state, struct nx_wl_buffer *buffer) {
	if (!(state->committed & NX_OUTPUT_STATE_WAIT_TIMELINE)) {
		drop_sync_surface(output);
		return true;
	}

	struct nx_wl_backend *wl = output->backend;
	struct nx_wl_drm_syncobj_timeline *acquire =
		get_or_import_timeline(wl, state->wait_timeline);
	struct sync_point release = release_point(wl, state, buffer);
	if (acquire == NULL || release.timeline == NULL || !ensure_sync_surface(output)) {
		return false;
	}

	wp_linux_drm_syncobj_surface_v1_set_acquire_point(output->drm_syncobj_surface_v1,
		acquire->wl, (uint32_t)(state->wait_point >> 32), (uint32_t)state->wait_point);
	wp_linux_drm_syncobj_surface_v1_set_release_point(output->drm_syncobj_surface_v1,
		release.timeline->wl, (uint32_t)(release.point >> 32), (uint32_t)release.point);

	if (!nx_drm_syncobj_timeline_waiter_init(&buffer->drm_syncobj_waiter,
			release.timeline->base, release.point, 0, wl->event_loop,
			nested_buffer_sync_ready)) {
		return false;
	}
	buffer->has_drm_syncobj_waiter = true;
	return true;
}

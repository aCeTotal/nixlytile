#include <assert.h>
#include <stdlib.h>
#include "platform/buffer/buffer.h"
#include "platform/render/drm_syncobj_merger.h"
#include "platform/surface/syncobj.h"

struct release_signaller {
	struct nx_drm_syncobj_merger *merger;
	struct wl_listener buffer_release;
};

static void release_signaller_handle_buffer_release(struct wl_listener *listener, void *data) {
	struct release_signaller *signaller = wl_container_of(listener, signaller, buffer_release);

	nx_drm_syncobj_merger_unref(signaller->merger);
	wl_list_remove(&signaller->buffer_release.link);
	free(signaller);
}

bool nx_linux_drm_syncobj_v1_state_signal_release_with_buffer(
		struct nx_linux_drm_syncobj_surface_v1_state *state, struct nx_buffer *buffer) {
	assert(buffer->n_locks > 0);
	// No release point committed yet
	if (state->release_merger == NULL) {
		return true;
	}

	struct release_signaller *signaller = calloc(1, sizeof(*signaller));
	if (signaller == NULL) {
		return false;
	}

	signaller->merger = nx_drm_syncobj_merger_ref(state->release_merger);
	signaller->buffer_release.notify = release_signaller_handle_buffer_release;
	wl_signal_add(&buffer->events.release, &signaller->buffer_release);

	return true;
}

bool nx_linux_drm_syncobj_v1_state_add_release_point(
		struct nx_linux_drm_syncobj_surface_v1_state *state,
		struct nx_drm_syncobj_timeline *release_timeline, uint64_t release_point,
		struct wl_event_loop *event_loop) {
	// No release point committed yet
	if (state->release_merger == NULL) {
		return true;
	}
	return nx_drm_syncobj_merger_add(state->release_merger,
		release_timeline, release_point, event_loop);
}

#include <errno.h>
#include "platform/drm/atomic/atomic.h"
#include "platform/log/log.h"

static bool commit_request(struct nx_drm_backend *drm,
		const struct nx_drm_device_state *state,
		struct nx_drm_page_flip *page_flip, uint32_t flags) {
	struct drm_atomic atom;
	drm_atomic_begin(&atom);
	for (size_t i = 0; i < state->connectors_len; i++) {
		drm_atomic_connector_add(&atom, &state->connectors[i], state->modeset);
	}
	bool ok = drm_atomic_commit(&atom, drm, state, page_flip, flags);
	drm_atomic_finish(&atom);
	return ok;
}

static bool strip_in_fences(const struct nx_drm_device_state *state) {
	bool had_fence = false;
	for (size_t i = 0; i < state->connectors_len; i++) {
		struct nx_drm_connector_state *conn_state = &state->connectors[i];
		if (conn_state->primary_in_fence_fd < 0) {
			continue;
		}
		drm_atomic_wait_fence(conn_state->primary_in_fence_fd);
		conn_state->primary_in_fence_fd = -1;
		had_fence = true;
	}
	return had_fence;
}

/* NVIDIA 555+ rejects IN_FENCE_FD. */
static bool retry_without_in_fences(struct nx_drm_backend *drm,
		const struct nx_drm_device_state *state,
		struct nx_drm_page_flip *page_flip, uint32_t flags) {
	if (!strip_in_fences(state)) {
		return false;
	}

	nx_log(NX_ERROR, "Atomic commit rejected IN_FENCE_FD (EPERM), "
		"retrying without plane in-fences (CPU-side fence waits)");
	if (!commit_request(drm, state, page_flip, flags)) {
		return false;
	}
	drm->in_fence_fd_broken = true;
	return true;
}

/* Gen9 i915 infoframes need modesets. */
static bool commit_deferring_content_types(struct nx_drm_backend *drm,
		const struct nx_drm_device_state *state,
		struct nx_drm_page_flip *page_flip, uint32_t flags) {
	bool deferred = false;
	for (size_t i = 0; i < state->connectors_len; i++) {
		struct nx_drm_connector_state *conn_state = &state->connectors[i];
		struct nx_drm_connector *conn = conn_state->connector;
		if (conn->props.content_type == 0 ||
				conn_state->content_type == conn->content_type) {
			continue;
		}
		conn_state->content_type = conn->content_type;
		conn_state->content_type_deferred = true;
		deferred = true;
	}

	return deferred && commit_request(drm, state, page_flip, flags);
}

static bool commit_with_fallbacks(struct nx_drm_backend *drm,
		const struct nx_drm_device_state *state,
		struct nx_drm_page_flip *page_flip, uint32_t flags) {
	if (commit_request(drm, state, page_flip, flags)) {
		return true;
	}
	if (errno == EPERM && !drm->in_fence_fd_broken &&
			retry_without_in_fences(drm, state, page_flip, flags)) {
		return true;
	}
	if (errno == EINVAL && !state->modeset) {
		return commit_deferring_content_types(drm, state, page_flip, flags);
	}
	return false;
}

static bool prepare_connectors(const struct nx_drm_device_state *state) {
	for (size_t i = 0; i < state->connectors_len; i++) {
		if (!drm_atomic_connector_prepare(&state->connectors[i], state->modeset)) {
			return false;
		}
	}
	return true;
}

static bool atomic_device_commit(struct nx_drm_backend *drm,
		const struct nx_drm_device_state *state,
		struct nx_drm_page_flip *page_flip, uint32_t flags, bool test_only) {
	if (test_only) {
		flags |= DRM_MODE_ATOMIC_TEST_ONLY;
	}
	if (state->modeset) {
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	}
	if (state->nonblock) {
		flags |= DRM_MODE_ATOMIC_NONBLOCK;
	}

	bool ok = prepare_connectors(state) &&
		commit_with_fallbacks(drm, state, page_flip, flags);

	for (size_t i = 0; i < state->connectors_len; i++) {
		struct nx_drm_connector_state *conn_state = &state->connectors[i];
		if (ok && conn_state->content_type_deferred) {
			conn_state->connector->content_type_needs_modeset = true;
		}
		if (ok && !test_only) {
			drm_atomic_connector_apply_commit(conn_state);
		} else {
			drm_atomic_connector_rollback_commit(conn_state);
		}
	}
	return ok;
}

const struct nx_drm_interface drm_atomic_iface = {
	.commit = atomic_device_commit,
};

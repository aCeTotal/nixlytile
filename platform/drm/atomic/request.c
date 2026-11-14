#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "platform/drm/atomic/atomic.h"
#include "platform/log/log.h"

static char *atomic_commit_flags_str(uint32_t flags) {
	const char *const l[] = {
		(flags & DRM_MODE_PAGE_FLIP_EVENT) ? "PAGE_FLIP_EVENT" : NULL,
		(flags & DRM_MODE_PAGE_FLIP_ASYNC) ? "PAGE_FLIP_ASYNC" : NULL,
		(flags & DRM_MODE_ATOMIC_TEST_ONLY) ? "ATOMIC_TEST_ONLY" : NULL,
		(flags & DRM_MODE_ATOMIC_NONBLOCK) ? "ATOMIC_NONBLOCK" : NULL,
		(flags & DRM_MODE_ATOMIC_ALLOW_MODESET) ? "ATOMIC_ALLOW_MODESET" : NULL,
	};

	char *buf = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&buf, &size);
	if (f == NULL) {
		return NULL;
	}

	for (size_t i = 0; i < sizeof(l) / sizeof(l[0]); i++) {
		if (l[i] == NULL) {
			continue;
		}
		if (ftell(f) > 0) {
			fprintf(f, " | ");
		}
		fprintf(f, "%s", l[i]);
	}

	if (ftell(f) == 0) {
		fprintf(f, "none");
	}

	fclose(f);

	return buf;
}

static void log_commit_failure(const struct nx_drm_device_state *state,
		uint32_t flags) {
	enum nx_log_importance log_level =
		flags & DRM_MODE_ATOMIC_TEST_ONLY ? NX_DEBUG : NX_ERROR;

	if (state->connectors_len == 1) {
		drm_conn_log_errno(state->connectors[0].connector, log_level,
			"Atomic commit failed");
	} else {
		nx_log_errno(log_level, "Atomic commit failed");
	}

	char *flags_str = atomic_commit_flags_str(flags);
	nx_log(NX_DEBUG, "(Atomic commit flags: %s)",
		flags_str ? flags_str : "<error>");
	free(flags_str);
}

void drm_atomic_begin(struct drm_atomic *atom) {
	*atom = (struct drm_atomic){0};

	atom->req = drmModeAtomicAlloc();
	if (!atom->req) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		atom->failed = true;
	}
}

void drm_atomic_add(struct drm_atomic *atom, uint32_t id, uint32_t prop, uint64_t val) {
	if (!atom->failed && drmModeAtomicAddProperty(atom->req, id, prop, val) < 0) {
		nx_log_errno(NX_ERROR, "Failed to add atomic DRM property");
		atom->failed = true;
	}
}

bool drm_atomic_commit(struct drm_atomic *atom, struct nx_drm_backend *drm,
		const struct nx_drm_device_state *state,
		struct nx_drm_page_flip *page_flip, uint32_t flags) {
	if (atom->failed) {
		return false;
	}

	int ret = drmModeAtomicCommit(drm->fd, atom->req, flags, page_flip);
	if (ret == 0) {
		return true;
	}

	log_commit_failure(state, flags);
	// Logging may clobber errno
	errno = -ret;
	return false;
}

void drm_atomic_finish(struct drm_atomic *atom) {
	drmModeAtomicFree(atom->req);
}

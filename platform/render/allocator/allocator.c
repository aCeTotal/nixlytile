#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "platform/backend/backend.h"
#include "platform/buffer/buffer_interface.h"
#include "platform/log/log.h"
#include "platform/render/allocator/allocator.h"
#include "platform/render/allocator/drm_dumb.h"
#include "platform/render/allocator/gbm.h"
#include "platform/render/allocator/shm.h"
#include "platform/render/renderer.h"

void nx_allocator_init(struct nx_allocator *alloc,
		const struct nx_allocator_interface *impl, uint32_t buffer_caps) {
	assert(impl && impl->destroy && impl->create_buffer);
	*alloc = (struct nx_allocator){
		.impl = impl,
		.buffer_caps = buffer_caps,
	};

	wl_signal_init(&alloc->events.destroy);
}

static char *render_or_primary_node_name(int drm_fd) {
	char *name = drmGetRenderDeviceNameFromFd(drm_fd);
	if (name != NULL) {
		return name;
	}
	return drmGetDeviceNameFromFd2(drm_fd);
}

/* Dumb buffers need primary auth */
static bool authenticate(int master_fd, int fd) {
	drm_magic_t magic;
	if (drmGetMagic(fd, &magic) < 0) {
		nx_log_errno(NX_ERROR, "drmGetMagic failed");
		return false;
	}

	if (drmAuthMagic(master_fd, magic) < 0) {
		nx_log_errno(NX_ERROR, "drmAuthMagic failed");
		return false;
	}
	return true;
}

static int open_drm_node(int drm_fd, char *(*node_name)(int fd)) {
	char *name = node_name(drm_fd);
	if (name == NULL) {
		nx_log(NX_ERROR, "drmGetDeviceNameFromFd2 failed");
		return -1;
	}

	int new_fd = open(name, O_RDWR | O_CLOEXEC);
	if (new_fd < 0) {
		nx_log_errno(NX_ERROR, "Failed to open DRM node '%s'", name);
		free(name);
		return -1;
	}

	free(name);

	if (drmIsMaster(drm_fd) && drmGetNodeTypeFromFd(new_fd) == DRM_NODE_PRIMARY &&
			!authenticate(drm_fd, new_fd)) {
		close(new_fd);
		return -1;
	}

	return new_fd;
}

/* Own FD isolates GEM handles */
static int reopen_drm_node(int drm_fd, char *(*node_name)(int fd)) {
	if (!drmIsMaster(drm_fd)) {
		return open_drm_node(drm_fd, node_name);
	}

	uint32_t lessee_id;
	int lease_fd = drmModeCreateLease(drm_fd, NULL, 0, O_CLOEXEC, &lessee_id);
	if (lease_fd >= 0) {
		return lease_fd;
	}
	if (lease_fd != -EINVAL && lease_fd != -EOPNOTSUPP) {
		nx_log_errno(NX_ERROR, "drmModeCreateLease failed");
		return -1;
	}
	nx_log(NX_DEBUG, "drmModeCreateLease failed, "
		"falling back to plain open");
	return open_drm_node(drm_fd, node_name);
}

struct nx_allocator *nx_allocator_autocreate(struct nx_backend *backend,
		struct nx_renderer *renderer) {
	uint32_t backend_caps = backend->buffer_caps;
	uint32_t renderer_caps = renderer->render_buffer_caps;

	int drm_fd = nx_backend_get_drm_fd(backend);
	if (drm_fd < 0) {
		drm_fd = nx_renderer_get_drm_fd(renderer);
	}

	struct nx_allocator *alloc = NULL;

	uint32_t gbm_caps = NX_BUFFER_CAP_DMABUF;
	if ((backend_caps & gbm_caps) && (renderer_caps & gbm_caps)
			&& drm_fd >= 0) {
		nx_log(NX_DEBUG, "Trying to create gbm allocator");
		int gbm_fd = reopen_drm_node(drm_fd, render_or_primary_node_name);
		if (gbm_fd < 0) {
			return NULL;
		}
		if ((alloc = nx_gbm_allocator_create(gbm_fd)) != NULL) {
			return alloc;
		}
		close(gbm_fd);
		nx_log(NX_DEBUG, "Failed to create gbm allocator");
	}

	uint32_t shm_caps = NX_BUFFER_CAP_SHM | NX_BUFFER_CAP_DATA_PTR;
	if ((backend_caps & shm_caps) && (renderer_caps & shm_caps)) {
		nx_log(NX_DEBUG, "Trying to create shm allocator");
		if ((alloc = nx_shm_allocator_create()) != NULL) {
			return alloc;
		}
		nx_log(NX_DEBUG, "Failed to create shm allocator");
	}

	uint32_t drm_caps = NX_BUFFER_CAP_DMABUF | NX_BUFFER_CAP_DATA_PTR;
	if ((backend_caps & drm_caps) && (renderer_caps & drm_caps)
			&& drm_fd >= 0 && drmIsMaster(drm_fd)) {
		nx_log(NX_DEBUG, "Trying to create drm dumb allocator");
		int dumb_fd = reopen_drm_node(drm_fd, drmGetDeviceNameFromFd2);
		if (dumb_fd < 0) {
			return NULL;
		}
		if ((alloc = nx_drm_dumb_allocator_create(dumb_fd)) != NULL) {
			return alloc;
		}
		close(dumb_fd);
		nx_log(NX_DEBUG, "Failed to create drm dumb allocator");
	}

	nx_log(NX_ERROR, "Failed to create allocator");
	return NULL;
}

void nx_allocator_destroy(struct nx_allocator *alloc) {
	if (alloc == NULL) {
		return;
	}
	wl_signal_emit_mutable(&alloc->events.destroy, NULL);

	assert(wl_list_empty(&alloc->events.destroy.listener_list));

	alloc->impl->destroy(alloc);
}

struct nx_buffer *nx_allocator_create_buffer(struct nx_allocator *alloc,
		int width, int height, const struct nx_drm_format *format) {
	struct nx_buffer *buffer =
		alloc->impl->create_buffer(alloc, width, height, format);
	if (buffer == NULL) {
		return NULL;
	}
	if (alloc->buffer_caps & NX_BUFFER_CAP_DATA_PTR) {
		assert(buffer->impl->begin_data_ptr_access &&
			buffer->impl->end_data_ptr_access);
	}
	if (alloc->buffer_caps & NX_BUFFER_CAP_DMABUF) {
		assert(buffer->impl->get_dmabuf);
	}
	if (alloc->buffer_caps & NX_BUFFER_CAP_SHM) {
		assert(buffer->impl->get_shm);
	}
	return buffer;
}

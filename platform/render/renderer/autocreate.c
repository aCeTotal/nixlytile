#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include "platform/backend/backend.h"
#include "platform/log/log.h"
#include "platform/os/env.h"
#include "platform/render/pixman/pixman_renderer.h"
#include "platform/render/renderer.h"
#include "platform/vulkan/vulkan.h"

enum renderer_choice {
	RENDERER_AUTO,
	RENDERER_VULKAN,
	RENDERER_PIXMAN,
};

static int open_first_render_node(drmDevice **devices, int devices_len) {
	for (int i = 0; i < devices_len; i++) {
		if (!(devices[i]->available_nodes & (1 << DRM_NODE_RENDER))) {
			continue;
		}
		const char *name = devices[i]->nodes[DRM_NODE_RENDER];
		nx_log(NX_DEBUG, "Opening DRM render node '%s'", name);
		int fd = open(name, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			nx_log_errno(NX_ERROR, "Failed to open '%s'", name);
		}
		return fd;
	}
	nx_log(NX_ERROR, "Failed to find any DRM render node");
	return -1;
}

static int open_drm_render_node(void) {
	int devices_len = drmGetDevices2(0, NULL, 0);
	if (devices_len < 0) {
		nx_log(NX_ERROR, "drmGetDevices2 failed: %s", strerror(-devices_len));
		return -1;
	}
	drmDevice **devices = calloc(devices_len, sizeof(*devices));
	if (devices == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return -1;
	}
	devices_len = drmGetDevices2(0, devices, devices_len);
	if (devices_len < 0) {
		free(devices);
		nx_log(NX_ERROR, "drmGetDevices2 failed: %s", strerror(-devices_len));
		return -1;
	}

	int fd = open_first_render_node(devices, devices_len);
	for (int i = 0; i < devices_len; i++) {
		drmFreeDevice(&devices[i]);
	}
	free(devices);
	return fd;
}

static int open_render_device(const char *name) {
	nx_log(NX_INFO, "Opening DRM render node '%s' from NIXLY_RENDER_DRM_DEVICE", name);
	int fd = open(name, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		nx_log_errno(NX_ERROR, "Failed to open '%s'", name);
		return -1;
	}
	if (drmGetNodeTypeFromFd(fd) != DRM_NODE_RENDER) {
		nx_log(NX_ERROR, "'%s' is not a DRM render node", name);
		close(fd);
		return -1;
	}
	return fd;
}

static void log_creation_failure(bool is_auto, const char *msg) {
	nx_log(is_auto ? NX_DEBUG : NX_ERROR, "%s%s", msg, is_auto ? ". Skipping!" : "");
}

static bool has_render_node(struct nx_backend *backend) {
	if (!backend) {
		return false;
	}

	int backend_drm_fd = nx_backend_get_drm_fd(backend);
	if (backend_drm_fd < 0) {
		return false;
	}

	char *render_node = drmGetRenderDeviceNameFromFd(backend_drm_fd);
	bool has_render_node = render_node != NULL;
	free(render_node);

	return has_render_node;
}

static struct nx_renderer *create_vulkan(int drm_fd, bool is_auto) {
	struct nx_renderer *renderer = nx_vk_renderer_create_with_drm_fd(drm_fd);
	if (renderer == NULL) {
		log_creation_failure(is_auto, "Failed to create a Vulkan renderer");
	}
	return renderer;
}

/* Takes ownership of the FD */
static struct nx_renderer *create_vulkan_with_own_fd(int drm_fd, bool is_auto) {
	if (drm_fd < 0) {
		log_creation_failure(is_auto, "Cannot create Vulkan renderer: no DRM FD available");
		return NULL;
	}
	struct nx_renderer *renderer = create_vulkan(drm_fd, is_auto);
	close(drm_fd);
	return renderer;
}

static struct nx_renderer *try_vulkan(struct nx_backend *backend, int drm_fd,
		bool is_auto) {
	if (drm_fd >= 0 || env_parse_bool("NIXLY_RENDERER_FORCE_SOFTWARE")) {
		return create_vulkan(drm_fd, is_auto);
	}

	const char *render_name = getenv("NIXLY_RENDER_DRM_DEVICE");
	if (render_name != NULL) {
		return create_vulkan_with_own_fd(open_render_device(render_name), is_auto);
	}

	int backend_drm_fd = nx_backend_get_drm_fd(backend);
	if (backend_drm_fd >= 0) {
		return create_vulkan(backend_drm_fd, is_auto);
	}

	if (backend->buffer_caps & NX_BUFFER_CAP_DMABUF) {
		return create_vulkan_with_own_fd(open_drm_render_node(), is_auto);
	}

	log_creation_failure(is_auto, "Cannot create Vulkan renderer: no DRM FD available");
	return NULL;
}

static struct nx_renderer *try_pixman(bool is_auto) {
	struct nx_renderer *renderer = nx_pixman_renderer_create();
	if (renderer == NULL) {
		log_creation_failure(is_auto, "Failed to create a pixman renderer");
	}
	return renderer;
}

static struct nx_renderer *renderer_autocreate(struct nx_backend *backend, int drm_fd) {
	const char *choices[] = { "auto", "vulkan", "pixman", NULL };
	enum renderer_choice choice = env_parse_switch("NIXLY_RENDERER", choices);
	bool is_auto = choice == RENDERER_AUTO;

	struct nx_renderer *renderer = NULL;
	if (choice != RENDERER_PIXMAN) {
		renderer = try_vulkan(backend, drm_fd, is_auto);
	}
	if (renderer == NULL && is_auto && has_render_node(backend)) {
		nx_log(NX_ERROR, "Vulkan unavailable, falling back to pixman");
	}
	if (renderer == NULL && choice != RENDERER_VULKAN) {
		renderer = try_pixman(is_auto);
	}

	if (renderer == NULL) {
		nx_log(NX_ERROR, "Could not initialize renderer");
		return NULL;
	}
	if (env_parse_bool("NIXLY_RENDER_NO_EXPLICIT_SYNC")) {
		renderer->features.timeline = false;
	}
	return renderer;
}

struct nx_renderer *renderer_autocreate_with_drm_fd(int drm_fd) {
	assert(drm_fd >= 0);

	return renderer_autocreate(NULL, drm_fd);
}

struct nx_renderer *nx_renderer_autocreate(struct nx_backend *backend) {
	return renderer_autocreate(backend, -1);
}

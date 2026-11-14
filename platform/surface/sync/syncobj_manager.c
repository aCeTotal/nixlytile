#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>
#include "linux-drm-syncobj-v1-protocol.h"
#include "platform/log/log.h"
#include "platform/render/drm_syncobj.h"
#include "platform/surface/sync/linux_drm_syncobj.h"
#include "platform/surface/syncobj.h"

#define LINUX_DRM_SYNCOBJ_V1_VERSION 1

struct nx_linux_drm_syncobj_manager_v1 {
	struct wl_global *global;
	int drm_fd;
	struct wl_listener display_destroy;
};

static const struct wp_linux_drm_syncobj_manager_v1_interface manager_impl;
static const struct wp_linux_drm_syncobj_timeline_v1_interface timeline_impl;

static struct nx_linux_drm_syncobj_manager_v1 *manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_linux_drm_syncobj_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

struct nx_drm_syncobj_timeline *syncobj_timeline_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_linux_drm_syncobj_timeline_v1_interface, &timeline_impl));
	return wl_resource_get_user_data(resource);
}

static void timeline_handle_resource_destroy(struct wl_resource *resource) {
	nx_drm_syncobj_timeline_unref(syncobj_timeline_from_resource(resource));
}

static void resource_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_linux_drm_syncobj_timeline_v1_interface timeline_impl = {
	.destroy = resource_handle_destroy,
};

static void manager_handle_import_timeline(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, int drm_syncobj_fd) {
	struct nx_linux_drm_syncobj_manager_v1 *manager =
		manager_from_resource(resource);

	struct nx_drm_syncobj_timeline *timeline =
		nx_drm_syncobj_timeline_import(manager->drm_fd, drm_syncobj_fd);
	close(drm_syncobj_fd);
	if (timeline == NULL) {
		wl_resource_post_error(resource,
			WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE,
			"Failed to import drm_syncobj timeline");
		return;
	}

	uint32_t version = wl_resource_get_version(resource);
	struct wl_resource *timeline_resource = wl_resource_create(client,
		&wp_linux_drm_syncobj_timeline_v1_interface, version, id);
	if (timeline_resource == NULL) {
		nx_drm_syncobj_timeline_unref(timeline);
		wl_resource_post_no_memory(resource);
		return;
	}
	wl_resource_set_implementation(timeline_resource,
		&timeline_impl, timeline, timeline_handle_resource_destroy);
}

static const struct wp_linux_drm_syncobj_manager_v1_interface manager_impl = {
	.destroy = resource_handle_destroy,
	.get_surface = syncobj_manager_get_surface,
	.import_timeline = manager_handle_import_timeline,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_linux_drm_syncobj_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&wp_linux_drm_syncobj_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_linux_drm_syncobj_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	close(manager->drm_fd);
	free(manager);
}

static bool check_syncobj_eventfd(int drm_fd) {
	uint64_t value = 0;
	if (drmGetCap(drm_fd, DRM_CAP_SYNCOBJ_TIMELINE, &value) != 0 || value == 0) {
		return false;
	}

	// Invalid handle must fail ENOENT
	struct drm_syncobj_eventfd syncobj_eventfd = {
		.handle = 0,
		.fd = -1,
	};
	return drmIoctl(drm_fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &syncobj_eventfd) != 0 && errno == ENOENT;
}

struct nx_linux_drm_syncobj_manager_v1 *nx_linux_drm_syncobj_manager_v1_create(
		struct wl_display *display, uint32_t version, int drm_fd) {
	assert(version <= LINUX_DRM_SYNCOBJ_V1_VERSION);

	if (!check_syncobj_eventfd(drm_fd)) {
		nx_log(NX_INFO, "DRM syncobj eventfd unavailable, disabling linux-drm-syncobj-v1");
		return NULL;
	}

	struct nx_linux_drm_syncobj_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->drm_fd = fcntl(drm_fd, F_DUPFD_CLOEXEC, 0);
	if (manager->drm_fd < 0) {
		goto error_manager;
	}

	manager->global = wl_global_create(display,
		&wp_linux_drm_syncobj_manager_v1_interface,
		version, manager, manager_bind);
	if (manager->global == NULL) {
		goto error_drm_fd;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;

error_drm_fd:
	close(manager->drm_fd);
error_manager:
	free(manager);
	return NULL;
}

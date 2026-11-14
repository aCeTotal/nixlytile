#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <xf86drm.h>
#include "platform/backend/backend_interface.h"
#include "platform/drm/commit/commit.h"
#include "platform/drm/connector/connector.h"
#include "platform/drm/device/device.h"
#include "platform/drm/device/fb.h"
#include "platform/log/log.h"
#include "platform/session/session.h"

struct nx_drm_backend *drm_backend_from_backend(struct nx_backend *backend) {
	assert(nx_backend_is_drm(backend));
	struct nx_drm_backend *drm = wl_container_of(backend, drm, backend);
	return drm;
}

static bool backend_start(struct nx_backend *backend) {
	struct nx_drm_backend *drm = drm_backend_from_backend(backend);
	drm_scan_connectors(drm, NULL);
	return true;
}

static void backend_destroy(struct nx_backend *backend) {
	struct nx_drm_backend *drm = drm_backend_from_backend(backend);

	struct nx_drm_connector *conn, *next;
	wl_list_for_each_safe(conn, next, &drm->connectors, link) {
		conn->crtc = NULL; // leave CRTCs on at exit
		drm_connector_destroy(conn);
	}

	struct nx_drm_page_flip *page_flip, *page_flip_tmp;
	wl_list_for_each_safe(page_flip, page_flip_tmp, &drm->page_flips, link) {
		drm_page_flip_destroy(page_flip);
	}

	nx_backend_finish(backend);

	wl_list_remove(&drm->session_destroy.link);
	wl_list_remove(&drm->session_active.link);
	wl_list_remove(&drm->parent_destroy.link);
	wl_list_remove(&drm->dev_change.link);
	wl_list_remove(&drm->dev_remove.link);

	if (drm->mgpu_renderer.nx_rend) {
		nx_drm_format_set_finish(&drm->mgpu_formats);
		drm_renderer_finish(&drm->mgpu_renderer);
	}

	drm_finish_resources(drm);

	struct nx_drm_fb *fb, *fb_tmp;
	wl_list_for_each_safe(fb, fb_tmp, &drm->fbs, link) {
		drm_fb_destroy(fb);
	}

	free(drm->name);
	nx_session_close_file(drm->session, drm->dev);
	wl_event_source_remove(drm->drm_event);
	free(drm);
}

static int backend_get_drm_fd(struct nx_backend *backend) {
	struct nx_drm_backend *drm = drm_backend_from_backend(backend);
	return drm->fd;
}

static const struct nx_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_drm_fd = backend_get_drm_fd,
};

bool nx_backend_is_drm(struct nx_backend *b) {
	return b->impl == &backend_impl;
}

struct nx_backend *nx_drm_backend_get_parent(struct nx_backend *backend) {
	struct nx_drm_backend *drm = drm_backend_from_backend(backend);
	return drm->parent ? &drm->parent->backend : NULL;
}

static void handle_session_active(struct wl_listener *listener, void *data) {
	struct nx_drm_backend *drm =
		wl_container_of(listener, drm, session_active);
	struct nx_session *session = drm->session;

	nx_log(NX_INFO, "DRM FD %s", session->active ? "resumed" : "paused");

	if (session->active) {
		drm_scan_connectors(drm, NULL);
		return;
	}

	// Resume rescans and modesets
	struct nx_drm_connector *conn;
	wl_list_for_each(conn, &drm->connectors, link) {
		if (conn->status == DRM_MODE_CONNECTED) {
			nx_output_destroy(&conn->output);
		}
	}
}

static void handle_dev_change(struct wl_listener *listener, void *data) {
	struct nx_drm_backend *drm = wl_container_of(listener, drm, dev_change);
	struct nx_device_change_event *change = data;

	if (!drm->session->active) {
		return;
	}

	if (change->type != NX_DEVICE_HOTPLUG) {
		nx_log(NX_DEBUG, "Received unknown change event for %s", drm->name);
		return;
	}

	nx_log(NX_DEBUG, "Received hotplug event for %s", drm->name);
	drm_scan_connectors(drm, &change->hotplug);
}

static void handle_dev_remove(struct wl_listener *listener, void *data) {
	struct nx_drm_backend *drm = wl_container_of(listener, drm, dev_remove);

	nx_log(NX_INFO, "Destroying DRM backend for %s", drm->name);
	backend_destroy(&drm->backend);
}

static void handle_session_destroy(struct wl_listener *listener, void *data) {
	struct nx_drm_backend *drm =
		wl_container_of(listener, drm, session_destroy);
	backend_destroy(&drm->backend);
}

static void handle_parent_destroy(struct wl_listener *listener, void *data) {
	struct nx_drm_backend *drm =
		wl_container_of(listener, drm, parent_destroy);
	backend_destroy(&drm->backend);
}

static char *device_name(int fd) {
	char *name = drmGetDeviceNameFromFd2(fd);
	if (name == NULL) {
		nx_log_errno(NX_ERROR, "drmGetDeviceNameFromFd2() failed");
		return NULL;
	}

	drmVersion *version = drmGetVersion(fd);
	if (version == NULL) {
		nx_log_errno(NX_ERROR, "drmGetVersion() failed");
		free(name);
		return NULL;
	}
	nx_log(NX_INFO, "Initializing DRM backend for %s (%s)", name, version->name);
	drmFreeVersion(version);
	return name;
}

static void watch_parent_and_device(struct nx_drm_backend *drm,
		struct nx_backend *parent) {
	if (parent != NULL) {
		drm->parent = drm_backend_from_backend(parent);
		drm->parent_destroy.notify = handle_parent_destroy;
		wl_signal_add(&parent->events.destroy, &drm->parent_destroy);
	} else {
		wl_list_init(&drm->parent_destroy.link);
	}

	drm->dev_change.notify = handle_dev_change;
	wl_signal_add(&drm->dev->events.change, &drm->dev_change);

	drm->dev_remove.notify = handle_dev_remove;
	wl_signal_add(&drm->dev->events.remove, &drm->dev_remove);
}

static bool init_device(struct nx_drm_backend *drm) {
	if (!drm_check_features(drm) || !drm_init_resources(drm)) {
		return false;
	}
	if (!drm->parent || drm_mgpu_init(drm)) {
		return true;
	}

	drm_renderer_finish(&drm->mgpu_renderer);
	drm_finish_resources(drm);
	return false;
}

struct nx_backend *nx_drm_backend_create(struct nx_session *session,
		struct nx_device *dev, struct nx_backend *parent) {
	assert(session && dev);
	assert(!parent || nx_backend_is_drm(parent));

	char *name = device_name(dev->fd);
	if (name == NULL) {
		return NULL;
	}

	struct nx_drm_backend *drm = calloc(1, sizeof(*drm));
	if (!drm) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		free(name);
		return NULL;
	}
	nx_backend_init(&drm->backend, &backend_impl);

	drm->backend.buffer_caps = NX_BUFFER_CAP_DMABUF;

	drm->session = session;
	wl_list_init(&drm->fbs);
	wl_list_init(&drm->connectors);
	wl_list_init(&drm->page_flips);

	drm->dev = dev;
	drm->fd = dev->fd;
	drm->name = name;

	watch_parent_and_device(drm, parent);

	drm->drm_event = wl_event_loop_add_fd(session->event_loop, drm->fd,
		WL_EVENT_READABLE, drm_handle_event, drm);
	if (!drm->drm_event) {
		nx_log(NX_ERROR, "Failed to create DRM event source");
		goto error_fd;
	}

	drm->session_active.notify = handle_session_active;
	wl_signal_add(&session->events.active, &drm->session_active);

	if (!init_device(drm)) {
		goto error_event;
	}

	drm->session_destroy.notify = handle_session_destroy;
	wl_signal_add(&session->events.destroy, &drm->session_destroy);

	return &drm->backend;

error_event:
	wl_list_remove(&drm->session_active.link);
	wl_event_source_remove(drm->drm_event);
error_fd:
	wl_list_remove(&drm->dev_remove.link);
	wl_list_remove(&drm->dev_change.link);
	wl_list_remove(&drm->parent_destroy.link);
	nx_session_close_file(drm->session, dev);
	free(drm->name);
	free(drm);
	return NULL;
}

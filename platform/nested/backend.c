#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/log/log.h"
#include "platform/nested/backend.h"
#include "platform/nested/output/output.h"
#include "platform/nested/seat/seat.h"

#include "xdg-shell-client-protocol.h"

static const struct nx_backend_impl backend_impl;

static bool backend_is_nested(struct nx_backend *backend) {
	return backend->impl == &backend_impl;
}

struct nx_wl_backend *nested_backend_from_backend(struct nx_backend *nx_backend) {
	assert(backend_is_nested(nx_backend));
	struct nx_wl_backend *backend = wl_container_of(nx_backend, backend, backend);
	return backend;
}

static int dispatch_events(int fd, uint32_t mask, void *data) {
	struct nx_wl_backend *wl = data;

	int count = 0;
	if (mask & WL_EVENT_READABLE) {
		count = wl_display_dispatch(wl->remote_display);
	}
	if (mask & WL_EVENT_WRITABLE) {
		wl_display_flush(wl->remote_display);
	}
	if (mask == 0) {
		count = wl_display_dispatch_pending(wl->remote_display);
		wl_display_flush(wl->remote_display);
	}

	// Drain wl_display.error before hangup.
	if (!(mask & WL_EVENT_READABLE) && (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) {
		if (mask & WL_EVENT_ERROR) {
			nx_log(NX_ERROR, "Failed to read from remote Wayland display");
		} else {
			nx_log(NX_DEBUG, "Disconnected from remote Wayland display");
		}
		nx_backend_destroy(&wl->backend);
		return 0;
	}

	if (count < 0) {
		nx_log(NX_ERROR, "Failed to dispatch remote Wayland display");
		nx_backend_destroy(&wl->backend);
		return 0;
	}
	return count;
}

static bool backend_start(struct nx_backend *backend) {
	struct nx_wl_backend *wl = nested_backend_from_backend(backend);
	nx_log(NX_INFO, "Starting Wayland backend");

	wl->started = true;

	struct nx_wl_seat *seat;
	wl_list_for_each(seat, &wl->seats, link) {
		if (seat->wl_keyboard) {
			nested_keyboard_init(seat);
		}
	}

	for (size_t i = 0; i < wl->requested_outputs; ++i) {
		nx_wl_output_create(&wl->backend);
	}
	return true;
}

static void disconnect_remote(struct nx_wl_backend *wl) {
	wl_event_queue_destroy(wl->busy_loop_queue);
	wl_display_disconnect(wl->remote_display);
}

static void backend_destroy(struct nx_backend *backend) {
	struct nx_wl_backend *wl = nested_backend_from_backend(backend);

	struct nx_wl_output *output, *tmp_output;
	wl_list_for_each_safe(output, tmp_output, &wl->outputs, link) {
		nx_output_destroy(&output->nx_output);
	}
	nested_buffers_destroy(wl);
	nested_timelines_destroy(wl);

	nx_backend_finish(backend);

	wl_list_remove(&wl->event_loop_destroy.link);
	wl_event_source_remove(wl->remote_display_src);
	close(wl->drm_fd);

	nx_drm_format_set_finish(&wl->shm_formats);
	nx_drm_format_set_finish(&wl->linux_dmabuf_v1_formats);

	struct nx_wl_seat *seat, *tmp_seat;
	wl_list_for_each_safe(seat, tmp_seat, &wl->seats, link) {
		nested_seat_destroy(seat);
	}

	nested_globals_destroy(wl);
	free(wl->drm_render_name);
	free(wl->activation_token);
	wl_display_flush(wl->remote_display);
	disconnect_remote(wl);
	free(wl);
}

static int backend_get_drm_fd(struct nx_backend *backend) {
	struct nx_wl_backend *wl = nested_backend_from_backend(backend);
	return wl->drm_fd;
}

static const struct nx_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_drm_fd = backend_get_drm_fd,
};

static void handle_event_loop_destroy(struct wl_listener *listener, void *data) {
	struct nx_wl_backend *wl = wl_container_of(listener, wl, event_loop_destroy);
	backend_destroy(&wl->backend);
}

static bool bind_globals(struct nx_wl_backend *wl) {
	wl->registry = wl_display_get_registry(wl->remote_display);
	if (wl->registry == NULL) {
		nx_log_errno(NX_ERROR, "Could not obtain reference to remote registry");
		return false;
	}
	wl_registry_add_listener(wl->registry, &nested_registry_listener, wl);
	wl_display_roundtrip(wl->remote_display);
	return true;
}

static bool has_required_globals(struct nx_wl_backend *wl) {
	if (wl->compositor == NULL) {
		nx_log(NX_ERROR, "Remote Wayland compositor does not support wl_compositor");
		return false;
	}
	if (wl->xdg_wm_base == NULL) {
		nx_log(NX_ERROR, "Remote Wayland compositor does not support xdg-shell");
		return false;
	}
	return true;
}

static void unbind_required_globals(struct nx_wl_backend *wl) {
	free(wl->drm_render_name);
	if (wl->compositor) {
		wl_compositor_destroy(wl->compositor);
	}
	if (wl->xdg_wm_base) {
		xdg_wm_base_destroy(wl->xdg_wm_base);
	}
	wl_registry_destroy(wl->registry);
}

static bool open_render_node(struct nx_wl_backend *wl) {
	wl->drm_fd = -1;
	if (wl->drm_render_name == NULL) {
		return true;
	}
	nx_log(NX_DEBUG, "Opening DRM render node %s", wl->drm_render_name);
	wl->drm_fd = open(wl->drm_render_name, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (wl->drm_fd < 0) {
		nx_log_errno(NX_ERROR, "Failed to open DRM render node %s",
			wl->drm_render_name);
		return false;
	}
	return true;
}

// Configures everything past the registry.
static bool backend_setup(struct nx_wl_backend *wl) {
	if (!has_required_globals(wl)) {
		return false;
	}
	wl->backend.features.timeline = wl->drm_syncobj_manager_v1 != NULL;

	// Initial event bursts.
	wl_display_roundtrip(wl->remote_display);
	if (!nested_dmabuf_feedback_query(wl)) {
		return false;
	}

	if (wl->zwp_linux_dmabuf_v1) {
		wl->backend.buffer_caps |= NX_BUFFER_CAP_DMABUF;
	}
	if (wl->shm) {
		wl->backend.buffer_caps |= NX_BUFFER_CAP_SHM;
	}

	int fd = wl_display_get_fd(wl->remote_display);
	wl->remote_display_src = wl_event_loop_add_fd(wl->event_loop, fd,
		WL_EVENT_READABLE, dispatch_events, wl);
	if (wl->remote_display_src == NULL) {
		nx_log(NX_ERROR, "Failed to create event source");
		return false;
	}
	wl_event_source_check(wl->remote_display_src);

	if (!open_render_node(wl)) {
		wl_event_source_remove(wl->remote_display_src);
		return false;
	}
	return true;
}

static bool connect_remote(struct nx_wl_backend *wl) {
	wl->remote_display = wl_display_connect(NULL);
	if (wl->remote_display == NULL) {
		nx_log_errno(NX_ERROR, "Could not connect to remote display");
		return false;
	}
	wl->busy_loop_queue = wl_display_create_queue(wl->remote_display);
	if (wl->busy_loop_queue == NULL) {
		nx_log_errno(NX_ERROR, "Could not create a Wayland event queue");
		wl_display_disconnect(wl->remote_display);
		return false;
	}
	if (!bind_globals(wl)) {
		disconnect_remote(wl);
		return false;
	}
	return true;
}

// Undoes everything on failure.
static bool backend_connect(struct nx_wl_backend *wl) {
	if (!connect_remote(wl)) {
		return false;
	}
	if (backend_setup(wl)) {
		return true;
	}
	unbind_required_globals(wl);
	disconnect_remote(wl);
	return false;
}

static void take_activation_token(struct nx_wl_backend *wl) {
	const char *token = getenv("XDG_ACTIVATION_TOKEN");
	if (token == NULL) {
		return;
	}
	wl->activation_token = strdup(token);
	unsetenv("XDG_ACTIVATION_TOKEN");
}

struct nx_backend *nx_wl_backend_create(struct wl_event_loop *loop) {
	nx_log(NX_INFO, "Creating wayland backend");

	struct nx_wl_backend *wl = calloc(1, sizeof(*wl));
	if (!wl) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return NULL;
	}

	nx_backend_init(&wl->backend, &backend_impl);
	wl->event_loop = loop;
	wl_list_init(&wl->outputs);
	wl_list_init(&wl->seats);
	wl_list_init(&wl->buffers);
	wl_list_init(&wl->drm_syncobj_timelines);

	if (!backend_connect(wl)) {
		nx_backend_finish(&wl->backend);
		free(wl);
		return NULL;
	}

	wl->event_loop_destroy.notify = handle_event_loop_destroy;
	wl_event_loop_add_destroy_listener(loop, &wl->event_loop_destroy);
	take_activation_token(wl);
	return &wl->backend;
}

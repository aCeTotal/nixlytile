#include <errno.h>
#include <libinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/backend/backend_interface.h"
#include "platform/input/libinput/libinput_device.h"
#include "platform/log/log.h"
#include "platform/os/env.h"
#include "platform/session/session.h"

static int libinput_open_restricted(const char *path,
		int flags, void *_backend) {
	struct nx_libinput_backend *backend = _backend;
	struct nx_device *dev = nx_session_open_file(backend->session, path);
	if (dev == NULL) {
		return -1;
	}
	return dev->fd;
}

static void libinput_close_restricted(int fd, void *_backend) {
	struct nx_libinput_backend *backend = _backend;

	struct nx_device *dev;
	wl_list_for_each(dev, &backend->session->devices, link) {
		if (dev->fd == fd) {
			nx_session_close_file(backend->session, dev);
			return;
		}
	}
}

static const struct libinput_interface libinput_impl = {
	.open_restricted = libinput_open_restricted,
	.close_restricted = libinput_close_restricted
};

static int handle_libinput_readable(int fd, uint32_t mask, void *_backend) {
	struct nx_libinput_backend *backend = _backend;
	int ret = libinput_dispatch(backend->libinput_context);
	if (ret != 0) {
		nx_log(NX_ERROR, "Failed to dispatch libinput: %s", strerror(-ret));
		nx_backend_destroy(&backend->backend);
		return 0;
	}
	struct libinput_event *event;
	while ((event = libinput_get_event(backend->libinput_context))) {
		handle_libinput_event(backend, event);
		libinput_event_destroy(event);
	}
	return 0;
}

static enum nx_log_importance libinput_log_priority_to_nx(
		enum libinput_log_priority priority) {
	switch (priority) {
	case LIBINPUT_LOG_PRIORITY_ERROR:
		return NX_ERROR;
	case LIBINPUT_LOG_PRIORITY_INFO:
		return NX_INFO;
	default:
		return NX_DEBUG;
	}
}

static void log_libinput(struct libinput *libinput_context,
		enum libinput_log_priority priority, const char *fmt, va_list args) {
	enum nx_log_importance importance = libinput_log_priority_to_nx(priority);
	static char nx_fmt[1024];
	snprintf(nx_fmt, sizeof(nx_fmt), "[libinput] %s", fmt);
	_nx_vlog(importance, nx_fmt, args);
}

static bool backend_start(struct nx_backend *nx_backend) {
	struct nx_libinput_backend *backend =
		wl_container_of(nx_backend, backend, backend);
	nx_log(NX_DEBUG, "Starting libinput backend");

	backend->libinput_context = libinput_udev_create_context(&libinput_impl,
		backend, backend->session->udev);
	if (!backend->libinput_context) {
		nx_log(NX_ERROR, "Failed to create libinput context");
		return false;
	}

	if (libinput_udev_assign_seat(backend->libinput_context,
			backend->session->seat) != 0) {
		nx_log(NX_ERROR, "Failed to assign libinput seat");
		return false;
	}

	libinput_log_set_handler(backend->libinput_context, log_libinput);
	libinput_log_set_priority(backend->libinput_context, LIBINPUT_LOG_PRIORITY_ERROR);

	int libinput_fd = libinput_get_fd(backend->libinput_context);

	handle_libinput_readable(libinput_fd, WL_EVENT_READABLE, backend);
	if (!env_parse_bool("NIXLY_LIBINPUT_NO_DEVICES") && wl_list_empty(&backend->devices)) {
		nx_log(NX_ERROR, "libinput initialization failed, no input devices");
		nx_log(NX_ERROR, "Set NIXLY_LIBINPUT_NO_DEVICES=1 to suppress this check");
		return false;
	}

	if (backend->input_event) {
		wl_event_source_remove(backend->input_event);
	}
	backend->input_event = wl_event_loop_add_fd(backend->session->event_loop, libinput_fd,
			WL_EVENT_READABLE, handle_libinput_readable, backend);
	if (!backend->input_event) {
		nx_log(NX_ERROR, "Failed to create input event on event loop");
		return false;
	}
	nx_log(NX_DEBUG, "libinput successfully initialized");
	return true;
}

static void backend_destroy(struct nx_backend *nx_backend) {
	struct nx_libinput_backend *backend =
		wl_container_of(nx_backend, backend, backend);

	struct nx_libinput_input_device *dev, *next;
	wl_list_for_each_safe(dev, next, &backend->devices, link) {
		destroy_libinput_input_device(dev);
	}

	nx_backend_finish(nx_backend);

	wl_list_remove(&backend->session_destroy.link);
	wl_list_remove(&backend->session_signal.link);

	if (backend->input_event) {
		wl_event_source_remove(backend->input_event);
	}
	libinput_unref(backend->libinput_context);
	free(backend);
}

static const struct nx_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
};

static void session_signal(struct wl_listener *listener, void *data) {
	struct nx_libinput_backend *backend =
		wl_container_of(listener, backend, session_signal);
	if (!backend->libinput_context) {
		return;
	}

	if (backend->session->active) {
		libinput_resume(backend->libinput_context);
		return;
	}
	libinput_suspend(backend->libinput_context);
}

static void handle_session_destroy(struct wl_listener *listener, void *data) {
	struct nx_libinput_backend *backend =
		wl_container_of(listener, backend, session_destroy);
	backend_destroy(&backend->backend);
}

struct nx_backend *nx_libinput_backend_create(struct nx_session *session) {
	struct nx_libinput_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		nx_log(NX_ERROR, "Allocation failed: %s", strerror(errno));
		return NULL;
	}
	nx_backend_init(&backend->backend, &backend_impl);

	wl_list_init(&backend->devices);

	backend->session = session;

	backend->session_signal.notify = session_signal;
	wl_signal_add(&session->events.active, &backend->session_signal);

	backend->session_destroy.notify = handle_session_destroy;
	wl_signal_add(&session->events.destroy, &backend->session_destroy);

	return &backend->backend;
}

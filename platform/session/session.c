#include <assert.h>
#include <libseat.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "platform/log/log.h"
#include "platform/session/seat.h"
#include "platform/session/session.h"
#include "platform/session/udev.h"

static void handle_event_loop_destroy(struct wl_listener *listener, void *data) {
	struct nx_session *session =
		wl_container_of(listener, session, event_loop_destroy);
	nx_session_destroy(session);
}

struct nx_session *nx_session_create(struct wl_event_loop *event_loop) {
	struct nx_session *session = calloc(1, sizeof(*session));
	if (!session) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return NULL;
	}

	session->event_loop = event_loop;
	wl_signal_init(&session->events.active);
	wl_signal_init(&session->events.add_drm_card);
	wl_signal_init(&session->events.destroy);
	wl_list_init(&session->devices);

	if (!session_seat_init(session)) {
		nx_log(NX_ERROR, "Failed to load session backend");
		goto error_session;
	}

	if (!session_udev_init(session)) {
		goto error_seat;
	}

	session->event_loop_destroy.notify = handle_event_loop_destroy;
	wl_event_loop_add_destroy_listener(event_loop, &session->event_loop_destroy);

	return session;

error_seat:
	session_seat_finish(session);
error_session:
	free(session);
	return NULL;
}

void nx_session_destroy(struct nx_session *session) {
	if (!session) {
		return;
	}

	wl_signal_emit_mutable(&session->events.destroy, session);

	assert(wl_list_empty(&session->events.active.listener_list));
	assert(wl_list_empty(&session->events.add_drm_card.listener_list));
	assert(wl_list_empty(&session->events.destroy.listener_list));

	wl_list_remove(&session->event_loop_destroy.link);

	session_udev_finish(session);

	struct nx_device *dev, *tmp_dev;
	wl_list_for_each_safe(dev, tmp_dev, &session->devices, link) {
		nx_session_close_file(session, dev);
	}

	session_seat_finish(session);
	free(session);
}

struct nx_device *nx_session_open_file(struct nx_session *session,
		const char *path) {
	int fd;
	int device_id = libseat_open_device(session->seat_handle, path, &fd);
	if (device_id == -1) {
		nx_log_errno(NX_ERROR, "Failed to open device: '%s'", path);
		return NULL;
	}

	struct nx_device *dev = malloc(sizeof(*dev));
	if (!dev) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		goto error;
	}

	struct stat st;
	if (fstat(fd, &st) < 0) {
		nx_log_errno(NX_ERROR, "Stat failed");
		goto error;
	}

	dev->fd = fd;
	dev->dev = st.st_rdev;
	dev->device_id = device_id;
	wl_signal_init(&dev->events.change);
	wl_signal_init(&dev->events.remove);
	wl_list_insert(&session->devices, &dev->link);

	return dev;

error:
	libseat_close_device(session->seat_handle, device_id);
	free(dev);
	close(fd);
	return NULL;
}

void nx_session_close_file(struct nx_session *session,
		struct nx_device *dev) {
	if (libseat_close_device(session->seat_handle, dev->device_id) == -1) {
		nx_log_errno(NX_ERROR, "Failed to close device %d", dev->device_id);
	}

	// remove unchecked, may be emitting
	assert(wl_list_empty(&dev->events.change.listener_list));

	close(dev->fd);
	wl_list_remove(&dev->link);
	free(dev);
}

bool nx_session_change_vt(struct nx_session *session, unsigned vt) {
	if (!session) {
		return false;
	}
	return libseat_switch_session(session->seat_handle, vt) == 0;
}

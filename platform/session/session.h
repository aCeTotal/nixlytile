#ifndef PLATFORM_SESSION_SESSION_H
#define PLATFORM_SESSION_SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>

struct libseat;

/* Device opened through the seat. */
struct nx_device {
	int fd;
	int device_id;
	dev_t dev;
	struct wl_list link; // nx_session.devices

	struct {
		struct wl_signal change; // struct nx_device_change_event
		struct wl_signal remove;
	} events;
};

/* Seat, devices and DRM hotplug. */
struct nx_session {
	/* False while another VT runs */
	bool active;

	char seat[256];

	struct udev *udev;
	struct udev_monitor *mon;
	struct wl_event_source *udev_event;

	struct libseat *seat_handle;
	struct wl_event_source *libseat_event;

	struct wl_list devices; // nx_device.link

	struct wl_event_loop *event_loop;

	struct {
		struct wl_signal active;
		struct wl_signal add_drm_card; // struct nx_session_add_event
		struct wl_signal destroy;
	} events;

	struct wl_listener event_loop_destroy;
};

struct nx_session_add_event {
	const char *path;
};

enum nx_device_change_type {
	NX_DEVICE_HOTPLUG = 1,
};

struct nx_device_hotplug_event {
	uint32_t connector_id;
};

struct nx_device_change_event {
	enum nx_device_change_type type;
	union {
		struct nx_device_hotplug_event hotplug;
	};
};

/* Takes over the current VT. */
struct nx_session *nx_session_create(struct wl_event_loop *loop);
/* Close opened files first. */
void nx_session_destroy(struct nx_session *session);

/* DRM or evdev nodes only. */
struct nx_device *nx_session_open_file(struct nx_session *session,
	const char *path);
void nx_session_close_file(struct nx_session *session,
	struct nx_device *device);

bool nx_session_change_vt(struct nx_session *session, unsigned vt);

/* Boot GPU first, waiting briefly. */
ssize_t nx_session_find_gpus(struct nx_session *session,
	size_t ret_len, struct nx_device **ret);

#endif

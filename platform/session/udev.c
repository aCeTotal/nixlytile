#include <libudev.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <xf86drm.h>
#include "platform/log/log.h"
#include "platform/session/session.h"
#include "platform/session/udev.h"

bool session_on_seat(const struct nx_session *session, struct udev_device *dev) {
	const char *seat = udev_device_get_property_value(dev, "ID_SEAT");
	if (!seat) {
		seat = "seat0";
	}
	return session->seat[0] == '\0' || strcmp(session->seat, seat) == 0;
}

static bool is_drm_card(const char *sysname) {
	const char prefix[] = DRM_PRIMARY_MINOR_NAME;
	if (strncmp(sysname, prefix, strlen(prefix)) != 0) {
		return false;
	}
	for (size_t i = strlen(prefix); sysname[i] != '\0'; i++) {
		if (sysname[i] < '0' || sysname[i] > '9') {
			return false;
		}
	}
	return true;
}

static struct nx_device *find_device(struct nx_session *session, dev_t devnum) {
	struct nx_device *dev;
	wl_list_for_each(dev, &session->devices, link) {
		if (dev->dev == devnum) {
			return dev;
		}
	}
	return NULL;
}

static void read_change_event(struct nx_device_change_event *event,
		struct udev_device *udev_dev) {
	const char *hotplug = udev_device_get_property_value(udev_dev, "HOTPLUG");
	if (hotplug == NULL || strcmp(hotplug, "1") != 0) {
		return;
	}

	event->type = NX_DEVICE_HOTPLUG;
	const char *connector = udev_device_get_property_value(udev_dev, "CONNECTOR");
	if (connector != NULL) {
		event->hotplug.connector_id = strtoul(connector, NULL, 10);
	}
}

static void card_added(struct nx_session *session, struct udev_device *udev_dev) {
	const char *sysname = udev_device_get_sysname(udev_dev);
	if (find_device(session, udev_device_get_devnum(udev_dev)) != NULL) {
		nx_log(NX_DEBUG, "Skipping duplicate device %s", sysname);
		return;
	}

	nx_log(NX_DEBUG, "DRM device %s added", sysname);
	struct nx_session_add_event event = {
		.path = udev_device_get_devnode(udev_dev),
	};
	wl_signal_emit_mutable(&session->events.add_drm_card, &event);
}

static void card_changed(struct nx_session *session, struct udev_device *udev_dev) {
	struct nx_device *dev = find_device(session, udev_device_get_devnum(udev_dev));
	if (dev == NULL) {
		return;
	}

	nx_log(NX_DEBUG, "DRM device %s changed", udev_device_get_sysname(udev_dev));
	struct nx_device_change_event event = {0};
	read_change_event(&event, udev_dev);
	wl_signal_emit_mutable(&dev->events.change, &event);
}

static void card_removed(struct nx_session *session, struct udev_device *udev_dev) {
	struct nx_device *dev = find_device(session, udev_device_get_devnum(udev_dev));
	if (dev == NULL) {
		return;
	}

	nx_log(NX_DEBUG, "DRM device %s removed", udev_device_get_sysname(udev_dev));
	wl_signal_emit_mutable(&dev->events.remove, NULL);
}

static int handle_udev_event(int fd, uint32_t mask, void *data) {
	struct nx_session *session = data;

	struct udev_device *udev_dev = udev_monitor_receive_device(session->mon);
	if (!udev_dev) {
		return 1;
	}

	const char *sysname = udev_device_get_sysname(udev_dev);
	const char *devnode = udev_device_get_devnode(udev_dev);
	const char *action = udev_device_get_action(udev_dev);
	nx_log(NX_DEBUG, "udev event for %s (%s)", sysname, action);

	if (!is_drm_card(sysname) || !action || !devnode ||
			!session_on_seat(session, udev_dev)) {
		udev_device_unref(udev_dev);
		return 1;
	}

	if (strcmp(action, "add") == 0) {
		card_added(session, udev_dev);
	} else if (strcmp(action, "change") == 0) {
		card_changed(session, udev_dev);
	} else if (strcmp(action, "remove") == 0) {
		card_removed(session, udev_dev);
	}

	udev_device_unref(udev_dev);
	return 1;
}

bool session_udev_init(struct nx_session *session) {
	session->udev = udev_new();
	if (!session->udev) {
		nx_log_errno(NX_ERROR, "Failed to create udev context");
		return false;
	}

	session->mon = udev_monitor_new_from_netlink(session->udev, "udev");
	if (!session->mon) {
		nx_log_errno(NX_ERROR, "Failed to create udev monitor");
		goto error_udev;
	}

	udev_monitor_filter_add_match_subsystem_devtype(session->mon, "drm", NULL);
	udev_monitor_enable_receiving(session->mon);

	int fd = udev_monitor_get_fd(session->mon);
	session->udev_event = wl_event_loop_add_fd(session->event_loop, fd,
		WL_EVENT_READABLE, handle_udev_event, session);
	if (!session->udev_event) {
		nx_log_errno(NX_ERROR, "Failed to create udev event source");
		goto error_mon;
	}

	return true;

error_mon:
	udev_monitor_unref(session->mon);
error_udev:
	udev_unref(session->udev);
	return false;
}

void session_udev_finish(struct nx_session *session) {
	wl_event_source_remove(session->udev_event);
	udev_monitor_unref(session->mon);
	udev_unref(session->udev);
}

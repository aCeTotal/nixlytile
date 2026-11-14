#include <libudev.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "platform/log/log.h"
#include "platform/os/clock.h"
#include "platform/session/gpu.h"
#include "platform/session/udev.h"

#define WAIT_GPU_TIMEOUT_MS 10000

struct nx_device *session_open_if_kms(struct nx_session *restrict session,
		const char *restrict path) {
	if (!path) {
		return NULL;
	}

	struct nx_device *dev = nx_session_open_file(session, path);
	if (!dev) {
		return NULL;
	}

	if (!drmIsKMS(dev->fd)) {
		nx_log(NX_DEBUG, "Ignoring '%s': not a KMS device", path);
		nx_session_close_file(session, dev);
		return NULL;
	}

	return dev;
}

static ssize_t explicit_find_gpus(struct nx_session *session,
		size_t ret_len, struct nx_device *ret[static ret_len], const char *str) {
	char *gpus = strdup(str);
	if (!gpus) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return -1;
	}

	size_t i = 0;
	char *save;
	char *ptr = strtok_r(gpus, ":", &save);
	do {
		if (i >= ret_len) {
			break;
		}

		ret[i] = session_open_if_kms(session, ptr);
		if (!ret[i]) {
			nx_log(NX_ERROR, "Unable to open %s as KMS device", ptr);
		} else {
			++i;
		}
	} while ((ptr = strtok_r(NULL, ":", &save)));

	free(gpus);
	return i;
}

static struct udev_enumerate *enumerate_drm_cards(struct udev *udev) {
	struct udev_enumerate *en = udev_enumerate_new(udev);
	if (!en) {
		nx_log(NX_ERROR, "udev_enumerate_new failed");
		return NULL;
	}

	udev_enumerate_add_match_subsystem(en, "drm");
	udev_enumerate_add_match_sysname(en, DRM_PRIMARY_MINOR_NAME "[0-9]*");

	if (udev_enumerate_scan_devices(en) != 0) {
		nx_log(NX_ERROR, "udev_enumerate_scan_devices failed");
		udev_enumerate_unref(en);
		return NULL;
	}

	return en;
}

struct find_gpus_add_handler {
	bool added;
	struct wl_listener listener;
};

static void find_gpus_handle_add(struct wl_listener *listener, void *data) {
	struct find_gpus_add_handler *handler =
		wl_container_of(listener, handler, listener);
	handler->added = true;
}

static bool dispatch_until(struct wl_event_loop *loop, const bool *done) {
	int64_t deadline = get_current_time_msec() + WAIT_GPU_TIMEOUT_MS;
	int64_t timeout = WAIT_GPU_TIMEOUT_MS;
	while (!*done) {
		if (wl_event_loop_dispatch(loop, (int)timeout) < 0) {
			nx_log_errno(NX_ERROR, "Failed to wait for KMS device: "
				"wl_event_loop_dispatch failed");
			return false;
		}

		int64_t now = get_current_time_msec();
		if (now >= deadline) {
			return true;
		}
		timeout = deadline - now;
	}
	return true;
}

static bool wait_for_gpu(struct nx_session *session) {
	nx_log(NX_INFO, "Waiting for a KMS device");

	struct find_gpus_add_handler handler = {0};
	handler.listener.notify = find_gpus_handle_add;
	wl_signal_add(&session->events.add_drm_card, &handler.listener);

	bool ok = dispatch_until(session->event_loop, &handler.added);
	wl_list_remove(&handler.listener.link);
	return ok;
}

static struct udev_enumerate *enumerate_or_wait(struct nx_session *session) {
	struct udev_enumerate *en = enumerate_drm_cards(session->udev);
	if (!en || udev_enumerate_get_list_entry(en) != NULL) {
		return en;
	}

	udev_enumerate_unref(en);
	if (!wait_for_gpu(session)) {
		return NULL;
	}
	return enumerate_drm_cards(session->udev);
}

/* boot_display, else PCI boot_vga. */
static bool is_boot_gpu(struct udev_device *dev) {
	const char *boot_display = udev_device_get_sysattr_value(dev, "boot_display");
	if (boot_display && strcmp(boot_display, "1") == 0) {
		return true;
	}

	// Owned by dev
	struct udev_device *pci =
		udev_device_get_parent_with_subsystem_devtype(dev, "pci", NULL);
	if (!pci) {
		return false;
	}
	const char *id = udev_device_get_sysattr_value(pci, "boot_vga");
	return id && strcmp(id, "1") == 0;
}

ssize_t nx_session_find_gpus(struct nx_session *session,
		size_t ret_len, struct nx_device **ret) {
	const char *explicit = getenv("NIXLY_DRM_DEVICES");
	if (explicit) {
		nx_log(NX_INFO, "Opening fixed list of KMS devices from NIXLY_DRM_DEVICES: %s", explicit);
		return explicit_find_gpus(session, ret_len, ret, explicit);
	}

	struct udev_enumerate *en = enumerate_or_wait(session);
	if (!en) {
		return -1;
	}

	size_t i = 0;
	struct udev_list_entry *entry;
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
		if (i == ret_len) {
			break;
		}

		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev = udev_device_new_from_syspath(session->udev, path);
		if (!dev) {
			continue;
		}
		if (!session_on_seat(session, dev)) {
			udev_device_unref(dev);
			continue;
		}

		bool is_primary = is_boot_gpu(dev);
		struct nx_device *nx_dev =
			session_open_if_kms(session, udev_device_get_devnode(dev));
		udev_device_unref(dev);
		if (!nx_dev) {
			continue;
		}

		ret[i] = nx_dev;
		if (is_primary) {
			struct nx_device *tmp = ret[0];
			ret[0] = ret[i];
			ret[i] = tmp;
		}
		++i;
	}

	udev_enumerate_unref(en);
	return i;
}

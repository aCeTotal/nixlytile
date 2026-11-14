#include <drm_fourcc.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include "platform/log/log.h"
#include "platform/nested/backend.h"
#include "platform/nested/seat/seat.h"
#include "platform/render/pixel_format.h"

#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static void xdg_wm_base_handle_ping(void *data,
		struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_handle_ping,
};

static void presentation_handle_clock_id(void *data,
		struct wp_presentation *presentation, uint32_t clock) {
	struct nx_wl_backend *wl = data;
	if (clock == CLOCK_MONOTONIC) {
		return;
	}
	wp_presentation_destroy(wl->presentation);
	wl->presentation = NULL;
}

static const struct wp_presentation_listener presentation_listener = {
	.clock_id = presentation_handle_clock_id,
};

// Deprecated, still sent by older hosts.
static void linux_dmabuf_v1_handle_format(void *data,
		struct zwp_linux_dmabuf_v1 *linux_dmabuf_v1, uint32_t format) {
	struct nx_wl_backend *wl = data;
	nx_drm_format_set_add(&wl->linux_dmabuf_v1_formats, format,
		DRM_FORMAT_MOD_INVALID);
}

static void linux_dmabuf_v1_handle_modifier(void *data,
		struct zwp_linux_dmabuf_v1 *linux_dmabuf_v1, uint32_t format,
		uint32_t modifier_hi, uint32_t modifier_lo) {
	struct nx_wl_backend *wl = data;
	uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
	nx_drm_format_set_add(&wl->linux_dmabuf_v1_formats, format, modifier);
}

static const struct zwp_linux_dmabuf_v1_listener linux_dmabuf_v1_listener = {
	.format = linux_dmabuf_v1_handle_format,
	.modifier = linux_dmabuf_v1_handle_modifier,
};

static void shm_handle_format(void *data, struct wl_shm *shm,
		uint32_t shm_format) {
	struct nx_wl_backend *wl = data;
	uint32_t drm_format = convert_wl_shm_format_to_drm(shm_format);
	nx_drm_format_set_add(&wl->shm_formats, drm_format, DRM_FORMAT_MOD_INVALID);
}

static const struct wl_shm_listener shm_listener = {
	.format = shm_handle_format,
};

static uint32_t cap_version(uint32_t version, uint32_t max) {
	return version < max ? version : max;
}

static void bind_compositor(struct nx_wl_backend *wl, uint32_t name, uint32_t version) {
	wl->compositor = wl_registry_bind(wl->registry, name,
		&wl_compositor_interface, 4);
}

static void bind_seat(struct nx_wl_backend *wl, uint32_t name, uint32_t version) {
	uint32_t target = version < 5 ? 5 : cap_version(version, 9);
	struct wl_seat *wl_seat = wl_registry_bind(wl->registry, name,
		&wl_seat_interface, target);
	if (!nested_seat_create(wl_seat, wl, name)) {
		wl_seat_destroy(wl_seat);
	}
}

static void bind_xdg_wm_base(struct nx_wl_backend *wl, uint32_t name, uint32_t version) {
	wl->xdg_wm_base = wl_registry_bind(wl->registry, name,
		&xdg_wm_base_interface, 1);
	xdg_wm_base_add_listener(wl->xdg_wm_base, &xdg_wm_base_listener, NULL);
}

static void bind_decoration_manager(struct nx_wl_backend *wl, uint32_t name,
		uint32_t version) {
	wl->zxdg_decoration_manager_v1 = wl_registry_bind(wl->registry, name,
		&zxdg_decoration_manager_v1_interface, 1);
}

static void bind_pointer_gestures(struct nx_wl_backend *wl, uint32_t name,
		uint32_t version) {
	wl->zwp_pointer_gestures_v1 = wl_registry_bind(wl->registry, name,
		&zwp_pointer_gestures_v1_interface, cap_version(version, 3));
}

static void bind_presentation(struct nx_wl_backend *wl, uint32_t name, uint32_t version) {
	wl->presentation = wl_registry_bind(wl->registry, name,
		&wp_presentation_interface, 1);
	wp_presentation_add_listener(wl->presentation, &presentation_listener, wl);
}

static void bind_linux_dmabuf(struct nx_wl_backend *wl, uint32_t name, uint32_t version) {
	if (version < 3) {
		return;
	}
	wl->zwp_linux_dmabuf_v1 = wl_registry_bind(wl->registry, name,
		&zwp_linux_dmabuf_v1_interface, cap_version(version, 4));
	zwp_linux_dmabuf_v1_add_listener(wl->zwp_linux_dmabuf_v1,
		&linux_dmabuf_v1_listener, wl);
}

static void bind_relative_pointer(struct nx_wl_backend *wl, uint32_t name,
		uint32_t version) {
	wl->zwp_relative_pointer_manager_v1 = wl_registry_bind(wl->registry, name,
		&zwp_relative_pointer_manager_v1_interface, 1);
}

static void bind_shm(struct nx_wl_backend *wl, uint32_t name, uint32_t version) {
	wl->shm = wl_registry_bind(wl->registry, name, &wl_shm_interface,
		cap_version(version, 2));
	wl_shm_add_listener(wl->shm, &shm_listener, wl);
}

static void bind_activation(struct nx_wl_backend *wl, uint32_t name, uint32_t version) {
	wl->activation_v1 = wl_registry_bind(wl->registry, name,
		&xdg_activation_v1_interface, 1);
}

static void bind_drm_syncobj(struct nx_wl_backend *wl, uint32_t name, uint32_t version) {
	wl->drm_syncobj_manager_v1 = wl_registry_bind(wl->registry, name,
		&wp_linux_drm_syncobj_manager_v1_interface, 1);
}

static const struct {
	const struct wl_interface *interface;
	void (*bind)(struct nx_wl_backend *wl, uint32_t name, uint32_t version);
} binders[] = {
	{ &wl_compositor_interface, bind_compositor },
	{ &wl_seat_interface, bind_seat },
	{ &xdg_wm_base_interface, bind_xdg_wm_base },
	{ &zxdg_decoration_manager_v1_interface, bind_decoration_manager },
	{ &zwp_pointer_gestures_v1_interface, bind_pointer_gestures },
	{ &wp_presentation_interface, bind_presentation },
	{ &zwp_linux_dmabuf_v1_interface, bind_linux_dmabuf },
	{ &zwp_relative_pointer_manager_v1_interface, bind_relative_pointer },
	{ &wl_shm_interface, bind_shm },
	{ &xdg_activation_v1_interface, bind_activation },
	{ &wp_linux_drm_syncobj_manager_v1_interface, bind_drm_syncobj },
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *iface, uint32_t version) {
	struct nx_wl_backend *wl = data;
	nx_log(NX_DEBUG, "Remote wayland global: %s v%" PRIu32, iface, version);

	for (size_t i = 0; i < sizeof(binders) / sizeof(binders[0]); i++) {
		if (strcmp(iface, binders[i].interface->name) == 0) {
			binders[i].bind(wl, name, version);
			return;
		}
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	struct nx_wl_backend *wl = data;
	struct nx_wl_seat *seat;
	wl_list_for_each(seat, &wl->seats, link) {
		if (seat->global_name == name) {
			nested_seat_destroy(seat);
			return;
		}
	}
}

const struct wl_registry_listener nested_registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void release_shm(struct wl_shm *shm) {
	if (wl_shm_get_version(shm) >= WL_SHM_RELEASE_SINCE_VERSION) {
		wl_shm_release(shm);
		return;
	}
	wl_shm_destroy(shm);
}

void nested_globals_destroy(struct nx_wl_backend *wl) {
	if (wl->activation_v1) {
		xdg_activation_v1_destroy(wl->activation_v1);
	}
	if (wl->zxdg_decoration_manager_v1) {
		zxdg_decoration_manager_v1_destroy(wl->zxdg_decoration_manager_v1);
	}
	if (wl->zwp_pointer_gestures_v1) {
		zwp_pointer_gestures_v1_destroy(wl->zwp_pointer_gestures_v1);
	}
	if (wl->presentation) {
		wp_presentation_destroy(wl->presentation);
	}
	if (wl->zwp_linux_dmabuf_v1) {
		zwp_linux_dmabuf_v1_destroy(wl->zwp_linux_dmabuf_v1);
	}
	if (wl->drm_syncobj_manager_v1) {
		wp_linux_drm_syncobj_manager_v1_destroy(wl->drm_syncobj_manager_v1);
	}
	if (wl->shm) {
		release_shm(wl->shm);
	}
	if (wl->zwp_relative_pointer_manager_v1) {
		zwp_relative_pointer_manager_v1_destroy(wl->zwp_relative_pointer_manager_v1);
	}
	xdg_wm_base_destroy(wl->xdg_wm_base);
	wl_compositor_destroy(wl->compositor);
	wl_registry_destroy(wl->registry);
}

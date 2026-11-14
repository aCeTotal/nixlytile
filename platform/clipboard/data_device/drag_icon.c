#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/clipboard/data_device/drag.h"

static void drag_icon_surface_role_commit(struct nx_surface *surface) {
	assert(surface->role == &drag_icon_surface_role);

	pixman_region32_clear(&surface->input_region);
	if (nx_surface_has_buffer(surface)) {
		nx_surface_map(surface);
	}
}

const struct nx_surface_role drag_icon_surface_role = {
	.name = "wl_data_device-icon",
	.no_object = true,
	.commit = drag_icon_surface_role_commit,
};

void drag_icon_destroy(struct nx_drag_icon *icon) {
	icon->drag->icon = NULL;
	wl_list_remove(&icon->surface_destroy.link);
	wl_signal_emit_mutable(&icon->events.destroy, icon);

	assert(wl_list_empty(&icon->events.destroy.listener_list));

	free(icon);
}

static void drag_icon_handle_surface_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_drag_icon *icon =
		wl_container_of(listener, icon, surface_destroy);
	drag_icon_destroy(icon);
}

struct nx_drag_icon *drag_icon_create(struct nx_drag *drag,
		struct nx_surface *surface) {
	struct nx_drag_icon *icon = calloc(1, sizeof(*icon));
	if (!icon) {
		return NULL;
	}

	icon->drag = drag;
	icon->surface = surface;

	wl_signal_init(&icon->events.destroy);

	icon->surface_destroy.notify = drag_icon_handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &icon->surface_destroy);

	drag_icon_surface_role_commit(surface);

	return icon;
}

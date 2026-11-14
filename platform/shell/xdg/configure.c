#include <assert.h>
#include <stdlib.h>
#include "platform/shell/xdg/role.h"
#include "platform/shell/xdg/surface.h"

static void configure_destroy(struct nx_xdg_surface_configure *configure) {
	wl_list_remove(&configure->link);
	free(configure->toplevel_configure);
	free(configure);
}

void shell_xdg_surface_reset(struct nx_xdg_surface *surface) {
	surface->configured = false;
	surface->initialized = false;

	struct nx_xdg_popup *popup, *popup_tmp;
	wl_list_for_each_safe(popup, popup_tmp, &surface->popups, link) {
		nx_xdg_popup_destroy(popup);
	}

	struct nx_xdg_surface_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &surface->configure_list, link) {
		configure_destroy(configure);
	}

	if (surface->configure_idle) {
		wl_event_source_remove(surface->configure_idle);
		surface->configure_idle = NULL;
	}
}

static void send_role_configure(struct nx_xdg_surface *surface,
		struct nx_xdg_surface_configure *configure) {
	switch (surface->role) {
	case NX_XDG_SURFACE_ROLE_NONE:
		assert(0 && "not reached");
		break;
	case NX_XDG_SURFACE_ROLE_TOPLEVEL:
		if (surface->toplevel != NULL) {
			configure->toplevel_configure =
				shell_toplevel_send_configure(surface->toplevel);
		}
		break;
	case NX_XDG_SURFACE_ROLE_POPUP:
		if (surface->popup != NULL) {
			configure->popup_configure =
				shell_popup_send_configure(surface->popup);
		}
		break;
	}
}

static void surface_send_configure(void *user_data) {
	struct nx_xdg_surface *surface = user_data;

	surface->configure_idle = NULL;

	struct nx_xdg_surface_configure *configure = calloc(1, sizeof(*configure));
	if (configure == NULL) {
		wl_client_post_no_memory(surface->client->client);
		return;
	}

	wl_list_insert(surface->configure_list.prev, &configure->link);
	configure->serial = surface->scheduled_serial;

	send_role_configure(surface, configure);
	wl_signal_emit_mutable(&surface->events.configure, configure);
	xdg_surface_send_configure(surface->resource, configure->serial);
}

uint32_t nx_xdg_surface_schedule_configure(struct nx_xdg_surface *surface) {
	assert(surface->initialized);
	if (surface->configure_idle != NULL) {
		return surface->scheduled_serial;
	}

	struct wl_display *display = wl_client_get_display(surface->client->client);
	surface->scheduled_serial = wl_display_next_serial(display);
	surface->configure_idle = wl_event_loop_add_idle(
		wl_display_get_event_loop(display), surface_send_configure, surface);
	if (surface->configure_idle == NULL) {
		wl_client_post_no_memory(surface->client->client);
	}
	return surface->scheduled_serial;
}

static struct nx_xdg_surface_configure *find_configure(
		struct nx_xdg_surface *surface, uint32_t serial) {
	struct nx_xdg_surface_configure *configure;
	wl_list_for_each(configure, &surface->configure_list, link) {
		if (configure->serial == serial) {
			return configure;
		}
	}
	return NULL;
}

static void ack_role_configure(struct nx_xdg_surface *surface,
		const struct nx_xdg_surface_configure *configure) {
	switch (surface->role) {
	case NX_XDG_SURFACE_ROLE_NONE:
		assert(0 && "not reached");
		break;
	case NX_XDG_SURFACE_ROLE_TOPLEVEL:
		if (surface->toplevel != NULL) {
			shell_toplevel_ack_configure(surface->toplevel,
				configure->toplevel_configure);
		}
		break;
	case NX_XDG_SURFACE_ROLE_POPUP:
		if (surface->popup != NULL) {
			shell_popup_ack_configure(surface->popup,
				configure->popup_configure);
		}
		break;
	}
}

void shell_xdg_surface_handle_ack_configure(struct wl_client *client,
		struct wl_resource *resource, uint32_t serial) {
	struct nx_xdg_surface *surface = shell_xdg_surface_from_resource(resource);
	if (surface == NULL) {
		return;
	}

	if (surface->role == NX_XDG_SURFACE_ROLE_NONE) {
		wl_resource_post_error(surface->resource,
			XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
			"xdg_surface must have a role");
		return;
	}

	struct nx_xdg_surface_configure *acked = find_configure(surface, serial);
	if (acked == NULL) {
		wl_resource_post_error(surface->client->resource,
			XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
			"wrong configure serial: %u", serial);
		return;
	}

	// Older configures are implicitly acked.
	struct nx_xdg_surface_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &surface->configure_list, link) {
		if (configure == acked) {
			break;
		}
		wl_signal_emit_mutable(&surface->events.ack_configure, configure);
		configure_destroy(configure);
	}

	ack_role_configure(surface, acked);

	surface->configured = true;
	surface->pending.configure_serial = serial;

	wl_signal_emit_mutable(&surface->events.ack_configure, acked);
	configure_destroy(acked);
}

#include <assert.h>
#include <stdlib.h>
#include "platform/shell/popup/popup_grab.h"
#include "platform/shell/popup/positioner.h"
#include "platform/shell/xdg/role.h"

static const struct xdg_popup_interface xdg_popup_implementation;

struct nx_xdg_popup *nx_xdg_popup_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_popup_interface,
		&xdg_popup_implementation));
	return wl_resource_get_user_data(resource);
}

struct nx_xdg_popup *nx_xdg_popup_try_from_nx_surface(struct nx_surface *surface) {
	struct nx_xdg_surface *xdg_surface = nx_xdg_surface_try_from_nx_surface(surface);
	if (xdg_surface == NULL || xdg_surface->role != NX_XDG_SURFACE_ROLE_POPUP) {
		return NULL;
	}
	return xdg_surface->popup;
}

void shell_popup_ack_configure(struct nx_xdg_popup *popup,
		const struct nx_xdg_popup_configure *configure) {
	popup->pending.geometry = configure->geometry;
}

struct nx_xdg_popup_configure *shell_popup_send_configure(
		struct nx_xdg_popup *popup) {
	struct nx_xdg_popup_configure *configure = calloc(1, sizeof(*configure));
	if (configure == NULL) {
		wl_resource_post_no_memory(popup->resource);
		return NULL;
	}
	*configure = popup->scheduled;

	if ((configure->fields & NX_XDG_POPUP_CONFIGURE_REPOSITION_TOKEN) &&
			wl_resource_get_version(popup->resource) >=
			XDG_POPUP_REPOSITIONED_SINCE_VERSION) {
		xdg_popup_send_repositioned(popup->resource,
			configure->reposition_token);
	}

	const struct nx_box *geometry = &configure->geometry;
	assert(geometry->width > 0 && geometry->height > 0);
	xdg_popup_send_configure(popup->resource,
		geometry->x, geometry->y, geometry->width, geometry->height);

	popup->scheduled.fields = 0;
	return configure;
}

void shell_popup_client_commit(struct nx_xdg_popup *popup) {
	if (popup->parent) {
		return;
	}
	nx_surface_reject_pending(popup->base->surface, popup->base->resource,
		XDG_SURFACE_ERROR_NOT_CONSTRUCTED, "xdg_popup has no parent");
}

static void popup_handle_grab(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *seat_resource,
		uint32_t serial) {
	struct nx_xdg_popup *popup = nx_xdg_popup_from_resource(resource);
	if (!popup) {
		return;
	}

	struct nx_seat_client *seat_client =
		nx_seat_client_from_resource(seat_resource);
	if (seat_client == NULL) {
		nx_xdg_popup_destroy(popup);
		return;
	}
	if (popup->base->surface->mapped) {
		wl_resource_post_error(popup->resource,
			XDG_POPUP_ERROR_INVALID_GRAB,
			"xdg_popup is already mapped");
		return;
	}

	struct nx_xdg_popup_grab *popup_grab = shell_popup_grab_from_seat(
		popup->base->client->shell, seat_client->seat);

	if (!wl_list_empty(&popup->base->popups)) {
		wl_resource_post_error(popup->base->client->resource,
			XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
			"xdg_popup was not created on the topmost popup");
		return;
	}

	shell_popup_grab_add(popup_grab, popup);
}

static void popup_handle_reposition(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *positioner_resource,
		uint32_t token) {
	struct nx_xdg_popup *popup = nx_xdg_popup_from_resource(resource);
	if (!popup) {
		return;
	}

	struct nx_xdg_positioner *positioner =
		shell_positioner_from_resource(positioner_resource);
	if (!shell_positioner_is_complete(positioner)) {
		wl_resource_post_error(popup->base->client->resource,
			XDG_WM_BASE_ERROR_INVALID_POSITIONER, "positioner object is not complete");
		return;
	}

	shell_positioner_rules_get_geometry(&positioner->rules,
		&popup->scheduled.geometry);
	popup->scheduled.rules = positioner->rules;
	popup->scheduled.fields |= NX_XDG_POPUP_CONFIGURE_REPOSITION_TOKEN;
	popup->scheduled.reposition_token = token;

	nx_xdg_surface_schedule_configure(popup->base);
}

static void popup_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_xdg_popup *popup = nx_xdg_popup_from_resource(resource);
	if (popup && !wl_list_empty(&popup->base->popups)) {
		wl_resource_post_error(popup->base->client->resource,
			XDG_WM_BASE_ERROR_NOT_THE_TOPMOST_POPUP,
			"xdg_popup was destroyed while it was not the topmost popup");
		return;
	}

	wl_resource_destroy(resource);
}

static const struct xdg_popup_interface xdg_popup_implementation = {
	.destroy = popup_handle_destroy,
	.grab = popup_handle_grab,
	.reposition = popup_handle_reposition,
};

static const struct nx_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct nx_xdg_popup_state),
};

void shell_popup_create(struct nx_xdg_surface *surface,
		struct nx_xdg_surface *parent, struct nx_xdg_positioner *positioner,
		uint32_t id) {
	if (!shell_positioner_is_complete(positioner)) {
		wl_resource_post_error(surface->client->resource,
			XDG_WM_BASE_ERROR_INVALID_POSITIONER, "positioner object is not complete");
		return;
	}

	if (!shell_xdg_surface_set_role(surface, NX_XDG_SURFACE_ROLE_POPUP)) {
		return;
	}

	if (parent != NULL && parent->role == NX_XDG_SURFACE_ROLE_NONE) {
		wl_resource_post_error(surface->client->resource, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
			"a popup parent must have a role");
		return;
	}

	assert(surface->popup == NULL);
	struct nx_xdg_popup *popup = calloc(1, sizeof(*popup));
	if (!popup) {
		wl_resource_post_no_memory(surface->resource);
		return;
	}
	surface->popup = popup;
	popup->base = surface;

	if (!nx_surface_synced_init(&popup->synced, surface->surface,
			&surface_synced_impl, &popup->pending, &popup->current)) {
		goto error_popup;
	}

	popup->resource = wl_resource_create(surface->client->client,
		&xdg_popup_interface, wl_resource_get_version(surface->resource), id);
	if (popup->resource == NULL) {
		goto error_synced;
	}
	wl_resource_set_implementation(popup->resource,
		&xdg_popup_implementation, popup, NULL);

	shell_positioner_rules_get_geometry(&positioner->rules,
		&popup->scheduled.geometry);
	popup->scheduled.rules = positioner->rules;

	if (parent != NULL) {
		popup->parent = parent->surface;
		wl_list_insert(&parent->popups, &popup->link);
	} else {
		wl_list_init(&popup->link);
	}
	shell_xdg_surface_set_role_object(surface, popup->resource);

	wl_signal_emit_mutable(&surface->client->shell->events.new_popup, popup);
	return;

error_synced:
	nx_surface_synced_finish(&popup->synced);
error_popup:
	free(popup);
	surface->popup = NULL;
	wl_resource_post_no_memory(surface->resource);
}

void shell_popup_reset(struct nx_xdg_popup *popup) {
	if (popup->seat == NULL) {
		return;
	}
	shell_popup_grab_remove(popup);
	popup->seat = NULL;
}

void shell_popup_destroy(struct nx_xdg_popup *popup) {
	nx_surface_unmap(popup->base->surface);
	shell_popup_reset(popup);

	nx_surface_synced_finish(&popup->synced);
	popup->base->popup = NULL;
	wl_list_remove(&popup->link);
	wl_resource_set_user_data(popup->resource, NULL);
	free(popup);
}

void nx_xdg_popup_destroy(struct nx_xdg_popup *popup) {
	if (popup == NULL) {
		return;
	}

	struct nx_xdg_popup *child, *child_tmp;
	wl_list_for_each_safe(child, child_tmp, &popup->base->popups, link) {
		nx_xdg_popup_destroy(child);
	}

	xdg_popup_send_popup_done(popup->resource);
	shell_popup_destroy(popup);
}

static void get_toplevel_offset(struct nx_xdg_popup *popup, int *x, int *y) {
	*x = 0;
	*y = 0;
	struct nx_surface *parent = popup->parent;
	struct nx_xdg_surface *xdg_surface;
	while ((xdg_surface = nx_xdg_surface_try_from_nx_surface(parent))) {
		if (xdg_surface->role != NX_XDG_SURFACE_ROLE_POPUP ||
				xdg_surface->popup == NULL) {
			*x += xdg_surface->geometry.x;
			*y += xdg_surface->geometry.y;
			break;
		}
		*x += xdg_surface->popup->current.geometry.x;
		*y += xdg_surface->popup->current.geometry.y;
		parent = xdg_surface->popup->parent;
	}
	assert(parent);
}

void nx_xdg_popup_unconstrain_from_box(struct nx_xdg_popup *popup,
		const struct nx_box *toplevel_space_box) {
	int toplevel_x, toplevel_y;
	get_toplevel_offset(popup, &toplevel_x, &toplevel_y);
	struct nx_box popup_constraint = {
		.x = toplevel_space_box->x - toplevel_x,
		.y = toplevel_space_box->y - toplevel_y,
		.width = toplevel_space_box->width,
		.height = toplevel_space_box->height,
	};
	shell_positioner_rules_unconstrain_box(&popup->scheduled.rules,
		&popup_constraint, &popup->scheduled.geometry);
	nx_xdg_surface_schedule_configure(popup->base);
}

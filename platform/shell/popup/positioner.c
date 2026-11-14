#include <assert.h>
#include <stdlib.h>
#include "platform/shell/popup/positioner.h"
#include "xdg-shell-protocol.h"

static const struct xdg_positioner_interface xdg_positioner_implementation;

struct nx_xdg_positioner *shell_positioner_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_positioner_interface,
		&xdg_positioner_implementation));
	return wl_resource_get_user_data(resource);
}

bool shell_positioner_is_complete(const struct nx_xdg_positioner *positioner) {
	return positioner->rules.size.width > 0 &&
		positioner->rules.anchor_rect.width > 0;
}

static void positioner_handle_set_size(struct wl_client *client,
		struct wl_resource *resource, int32_t width, int32_t height) {
	struct nx_xdg_positioner *positioner = shell_positioner_from_resource(resource);
	if (width < 1 || height < 1) {
		wl_resource_post_error(resource,
			XDG_POSITIONER_ERROR_INVALID_INPUT,
			"width and height must be positive and non-zero");
		return;
	}

	positioner->rules.size.width = width;
	positioner->rules.size.height = height;
}

static void positioner_handle_set_anchor_rect(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y, int32_t width,
		int32_t height) {
	struct nx_xdg_positioner *positioner = shell_positioner_from_resource(resource);
	if (width < 0 || height < 0) {
		wl_resource_post_error(resource,
			XDG_POSITIONER_ERROR_INVALID_INPUT,
			"width and height must be positive");
		return;
	}

	positioner->rules.anchor_rect = (struct nx_box){
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
}

static void positioner_handle_set_anchor(struct wl_client *client,
		struct wl_resource *resource, uint32_t anchor) {
	struct nx_xdg_positioner *positioner = shell_positioner_from_resource(resource);
	if (!xdg_positioner_anchor_is_valid(anchor, wl_resource_get_version(resource))) {
		wl_resource_post_error(resource,
			XDG_POSITIONER_ERROR_INVALID_INPUT,
			"invalid anchor value");
		return;
	}

	positioner->rules.anchor = anchor;
}

static void positioner_handle_set_gravity(struct wl_client *client,
		struct wl_resource *resource, uint32_t gravity) {
	struct nx_xdg_positioner *positioner = shell_positioner_from_resource(resource);
	if (!xdg_positioner_gravity_is_valid(gravity, wl_resource_get_version(resource))) {
		wl_resource_post_error(resource,
			XDG_POSITIONER_ERROR_INVALID_INPUT,
			"invalid gravity value");
		return;
	}

	positioner->rules.gravity = gravity;
}

static void positioner_handle_set_constraint_adjustment(
		struct wl_client *client, struct wl_resource *resource,
		uint32_t constraint_adjustment) {
	struct nx_xdg_positioner *positioner = shell_positioner_from_resource(resource);
	if (!xdg_positioner_constraint_adjustment_is_valid(constraint_adjustment,
			wl_resource_get_version(resource))) {
		wl_resource_post_error(resource,
			XDG_POSITIONER_ERROR_INVALID_INPUT,
			"invalid constraint_adjustment value");
		return;
	}

	positioner->rules.constraint_adjustment = constraint_adjustment;
}

static void positioner_handle_set_offset(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y) {
	struct nx_xdg_positioner *positioner = shell_positioner_from_resource(resource);
	positioner->rules.offset.x = x;
	positioner->rules.offset.y = y;
}

// Reactive hints go unused.
static void positioner_handle_set_reactive(struct wl_client *client,
		struct wl_resource *resource) {
}

static void positioner_handle_set_parent_configure(struct wl_client *client,
		struct wl_resource *resource, uint32_t serial) {
}

static void positioner_handle_set_parent_size(struct wl_client *client,
		struct wl_resource *resource, int32_t parent_width,
		int32_t parent_height) {
}

static void positioner_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct xdg_positioner_interface xdg_positioner_implementation = {
	.destroy = positioner_handle_destroy,
	.set_size = positioner_handle_set_size,
	.set_anchor_rect = positioner_handle_set_anchor_rect,
	.set_anchor = positioner_handle_set_anchor,
	.set_gravity = positioner_handle_set_gravity,
	.set_constraint_adjustment = positioner_handle_set_constraint_adjustment,
	.set_offset = positioner_handle_set_offset,
	.set_reactive = positioner_handle_set_reactive,
	.set_parent_size = positioner_handle_set_parent_size,
	.set_parent_configure = positioner_handle_set_parent_configure,
};

static void positioner_handle_resource_destroy(struct wl_resource *resource) {
	free(shell_positioner_from_resource(resource));
}

void shell_positioner_create(struct nx_xdg_client *client, uint32_t id) {
	struct nx_xdg_positioner *positioner = calloc(1, sizeof(*positioner));
	if (positioner == NULL) {
		wl_client_post_no_memory(client->client);
		return;
	}

	positioner->resource = wl_resource_create(client->client,
		&xdg_positioner_interface, wl_resource_get_version(client->resource),
		id);
	if (positioner->resource == NULL) {
		free(positioner);
		wl_client_post_no_memory(client->client);
		return;
	}
	wl_resource_set_implementation(positioner->resource,
		&xdg_positioner_implementation, positioner,
		positioner_handle_resource_destroy);
}

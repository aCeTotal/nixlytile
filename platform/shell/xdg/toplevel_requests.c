#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "platform/protocol/utf8.h"
#include "platform/shell/xdg/role.h"

// No dedicated protocol error exists.
#define ERROR_INVALID_TITLE ((uint32_t)-1)

struct nx_xdg_toplevel *nx_xdg_toplevel_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_toplevel_interface,
		&shell_toplevel_impl));
	return wl_resource_get_user_data(resource);
}

static void check_configured(struct nx_xdg_toplevel *toplevel) {
	if (toplevel->base->configured) {
		return;
	}
	wl_resource_post_error(toplevel->base->resource,
		XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
		"surface has not been configured yet");
}

static void toplevel_handle_set_parent(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *parent_resource) {
	struct nx_xdg_toplevel *toplevel = nx_xdg_toplevel_from_resource(resource);
	struct nx_xdg_toplevel *parent = NULL;
	if (parent_resource != NULL) {
		parent = nx_xdg_toplevel_from_resource(parent_resource);
	}

	if (!nx_xdg_toplevel_set_parent(toplevel, parent)) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_PARENT,
			"a toplevel cannot be a parent of itself or its ancestor");
	}
}

static void toplevel_handle_set_title(struct wl_client *client,
		struct wl_resource *resource, const char *title) {
	struct nx_xdg_toplevel *toplevel = nx_xdg_toplevel_from_resource(resource);
	if (!is_utf8(title)) {
		wl_resource_post_error(resource, ERROR_INVALID_TITLE,
			"xdg_toplevel title is not valid UTF-8");
		return;
	}

	char *copy = strdup(title);
	if (copy == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	free(toplevel->title);
	toplevel->title = copy;
	wl_signal_emit_mutable(&toplevel->events.set_title, NULL);
}

static void toplevel_handle_set_app_id(struct wl_client *client,
		struct wl_resource *resource, const char *app_id) {
	struct nx_xdg_toplevel *toplevel = nx_xdg_toplevel_from_resource(resource);
	char *copy = strdup(app_id);
	if (copy == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	free(toplevel->app_id);
	toplevel->app_id = copy;
}

static void toplevel_handle_show_window_menu(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *seat_resource,
		uint32_t serial, int32_t x, int32_t y) {
	check_configured(nx_xdg_toplevel_from_resource(resource));
}

static void toplevel_handle_move(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *seat_resource,
		uint32_t serial) {
	check_configured(nx_xdg_toplevel_from_resource(resource));
}

static void toplevel_handle_resize(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *seat_resource,
		uint32_t serial, uint32_t edges) {
	struct nx_xdg_toplevel *toplevel = nx_xdg_toplevel_from_resource(resource);
	uint32_t version = wl_resource_get_version(toplevel->base->resource);
	if (!xdg_toplevel_resize_edge_is_valid(edges, version)) {
		wl_resource_post_error(toplevel->base->resource,
			XDG_TOPLEVEL_ERROR_INVALID_RESIZE_EDGE,
			"provided value is not a valid variant of the resize_edge enum");
		return;
	}

	check_configured(toplevel);
}

static void toplevel_handle_set_max_size(struct wl_client *client,
		struct wl_resource *resource, int32_t width, int32_t height) {
	struct nx_xdg_toplevel *toplevel = nx_xdg_toplevel_from_resource(resource);
	toplevel->pending.max_width = width;
	toplevel->pending.max_height = height;
}

static void toplevel_handle_set_min_size(struct wl_client *client,
		struct wl_resource *resource, int32_t width, int32_t height) {
	struct nx_xdg_toplevel *toplevel = nx_xdg_toplevel_from_resource(resource);
	toplevel->pending.min_width = width;
	toplevel->pending.min_height = height;
}

static void toplevel_handle_maximize_request(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_xdg_toplevel *toplevel = nx_xdg_toplevel_from_resource(resource);
	wl_signal_emit_mutable(&toplevel->events.request_maximize, NULL);
}

static void toplevel_request_fullscreen(struct wl_resource *resource,
		bool fullscreen) {
	struct nx_xdg_toplevel *toplevel = nx_xdg_toplevel_from_resource(resource);
	toplevel->requested.fullscreen = fullscreen;
	wl_signal_emit_mutable(&toplevel->events.request_fullscreen, NULL);
}

static void toplevel_handle_set_fullscreen(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *output_resource) {
	toplevel_request_fullscreen(resource, true);
}

static void toplevel_handle_unset_fullscreen(struct wl_client *client,
		struct wl_resource *resource) {
	toplevel_request_fullscreen(resource, false);
}

// Minimize requests are ignored.
static void toplevel_handle_set_minimized(struct wl_client *client,
		struct wl_resource *resource) {
}

static void toplevel_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

const struct xdg_toplevel_interface shell_toplevel_impl = {
	.destroy = toplevel_handle_destroy,
	.set_parent = toplevel_handle_set_parent,
	.set_title = toplevel_handle_set_title,
	.set_app_id = toplevel_handle_set_app_id,
	.show_window_menu = toplevel_handle_show_window_menu,
	.move = toplevel_handle_move,
	.resize = toplevel_handle_resize,
	.set_max_size = toplevel_handle_set_max_size,
	.set_min_size = toplevel_handle_set_min_size,
	.set_maximized = toplevel_handle_maximize_request,
	.unset_maximized = toplevel_handle_maximize_request,
	.set_fullscreen = toplevel_handle_set_fullscreen,
	.unset_fullscreen = toplevel_handle_unset_fullscreen,
	.set_minimized = toplevel_handle_set_minimized,
};

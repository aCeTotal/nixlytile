#include <inttypes.h>
#include <wayland-server-protocol.h>
#include "platform/surface/compositor/surface.h"

#define CALLBACK_VERSION 1

static void surface_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_surface *surface = nx_surface_from_resource(resource);
	if (surface->role_resource != NULL) {
		wl_resource_post_error(resource,
			WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
			"surface was destroyed before its role object");
		return;
	}
	wl_resource_destroy(resource);
}

static void surface_handle_attach(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_resource *buffer_resource, int32_t dx, int32_t dy) {
	struct nx_surface *surface = nx_surface_from_resource(resource);
	bool has_offset_request =
		wl_resource_get_version(resource) >= WL_SURFACE_OFFSET_SINCE_VERSION;

	if (has_offset_request && (dx != 0 || dy != 0)) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
			"Offset must be zero on wl_surface.attach version >= %"PRIu32,
			WL_SURFACE_OFFSET_SINCE_VERSION);
		return;
	}

	surface->pending.committed |= NX_SURFACE_STATE_BUFFER;
	surface_set_pending_buffer_resource(surface, buffer_resource);

	if (has_offset_request) {
		return;
	}
	surface->pending.committed |= NX_SURFACE_STATE_OFFSET;
	surface->pending.dx = dx;
	surface->pending.dy = dy;
}

static void surface_handle_damage(struct wl_client *client,
		struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height) {
	struct nx_surface *surface = nx_surface_from_resource(resource);
	if (width < 0 || height < 0) {
		return;
	}
	surface->pending.committed |= NX_SURFACE_STATE_SURFACE_DAMAGE;
	pixman_region32_union_rect(&surface->pending.surface_damage,
		&surface->pending.surface_damage,
		x, y, width, height);
}

static void callback_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void surface_handle_frame(struct wl_client *client,
		struct wl_resource *resource, uint32_t callback) {
	struct nx_surface *surface = nx_surface_from_resource(resource);

	struct wl_resource *callback_resource = wl_resource_create(client,
		&wl_callback_interface, CALLBACK_VERSION, callback);
	if (callback_resource == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}
	wl_resource_set_implementation(callback_resource, NULL, NULL,
		callback_handle_resource_destroy);

	wl_list_insert(surface->pending.frame_callback_list.prev,
		wl_resource_get_link(callback_resource));

	surface->pending.committed |= NX_SURFACE_STATE_FRAME_CALLBACK_LIST;
}

static void surface_handle_set_opaque_region(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_resource *region_resource) {
	struct nx_surface *surface = nx_surface_from_resource(resource);
	surface->pending.committed |= NX_SURFACE_STATE_OPAQUE_REGION;
	if (region_resource == NULL) {
		pixman_region32_clear(&surface->pending.opaque);
		return;
	}
	pixman_region32_copy(&surface->pending.opaque,
		nx_region_from_resource(region_resource));
}

static void surface_handle_set_input_region(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_resource *region_resource) {
	struct nx_surface *surface = nx_surface_from_resource(resource);
	surface->pending.committed |= NX_SURFACE_STATE_INPUT_REGION;
	if (region_resource == NULL) {
		pixman_region32_fini(&surface->pending.input);
		pixman_region32_init_rect(&surface->pending.input,
			INT32_MIN, INT32_MIN, UINT32_MAX, UINT32_MAX);
		return;
	}
	pixman_region32_copy(&surface->pending.input,
		nx_region_from_resource(region_resource));
}

static void surface_handle_set_buffer_transform(struct wl_client *client,
		struct wl_resource *resource, int32_t transform) {
	if (transform < WL_OUTPUT_TRANSFORM_NORMAL ||
			transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
			"Specified transform value (%d) is invalid", transform);
		return;
	}
	struct nx_surface *surface = nx_surface_from_resource(resource);
	surface->pending.committed |= NX_SURFACE_STATE_TRANSFORM;
	surface->pending.transform = transform;
}

static void surface_handle_set_buffer_scale(struct wl_client *client,
		struct wl_resource *resource, int32_t scale) {
	if (scale <= 0) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE,
			"Specified scale value (%d) is not positive", scale);
		return;
	}
	struct nx_surface *surface = nx_surface_from_resource(resource);
	surface->pending.committed |= NX_SURFACE_STATE_SCALE;
	surface->pending.scale = scale;
}

static void surface_handle_damage_buffer(struct wl_client *client,
		struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height) {
	struct nx_surface *surface = nx_surface_from_resource(resource);
	if (width < 0 || height < 0) {
		return;
	}
	surface->pending.committed |= NX_SURFACE_STATE_BUFFER_DAMAGE;
	pixman_region32_union_rect(&surface->pending.buffer_damage,
		&surface->pending.buffer_damage,
		x, y, width, height);
}

static void surface_handle_offset(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y) {
	struct nx_surface *surface = nx_surface_from_resource(resource);

	surface->pending.committed |= NX_SURFACE_STATE_OFFSET;
	surface->pending.dx = x;
	surface->pending.dy = y;
}

const struct wl_surface_interface surface_implementation = {
	.destroy = surface_handle_destroy,
	.attach = surface_handle_attach,
	.damage = surface_handle_damage,
	.frame = surface_handle_frame,
	.set_opaque_region = surface_handle_set_opaque_region,
	.set_input_region = surface_handle_set_input_region,
	.commit = surface_handle_commit,
	.set_buffer_transform = surface_handle_set_buffer_transform,
	.set_buffer_scale = surface_handle_set_buffer_scale,
	.damage_buffer = surface_handle_damage_buffer,
	.offset = surface_handle_offset,
};

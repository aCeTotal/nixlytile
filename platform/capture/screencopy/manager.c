#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include "platform/capture/screencopy.h"
#include "platform/capture/screencopy/frame.h"
#include "platform/geometry/transform.h"
#include "platform/log/log.h"
#include "platform/output/output_interface.h"
#include "platform/render/allocator/allocator.h"
#include "platform/render/pixel_format.h"
#include "platform/render/renderer.h"
#include "platform/render/swapchain.h"
#include "wlr-screencopy-unstable-v1-protocol.h"

#define SCREENCOPY_MANAGER_VERSION 3

static const struct zwlr_screencopy_manager_v1_interface manager_impl;

static struct screencopy_client *client_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_screencopy_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

// NULL without a readable format.
static const struct nx_pixel_format_info *frame_pick_formats(
		struct screencopy_frame *frame) {
	struct nx_output *output = frame->output;
	struct nx_renderer *renderer = output->renderer;
	assert(renderer);

	if (!nx_output_configure_primary_swapchain(output, NULL, &output->swapchain)) {
		return NULL;
	}

	struct nx_buffer *buffer = nx_swapchain_acquire(output->swapchain);
	if (buffer == NULL) {
		return NULL;
	}

	struct nx_texture *texture = nx_texture_from_buffer(renderer, buffer);
	nx_buffer_unlock(buffer);
	if (!texture) {
		return NULL;
	}

	frame->shm_format = nx_texture_preferred_read_format(texture);
	nx_texture_destroy(texture);

	if (frame->shm_format == DRM_FORMAT_INVALID) {
		nx_log(NX_ERROR,
			"Failed to capture output: no read format supported by renderer");
		return NULL;
	}
	const struct nx_pixel_format_info *shm_info =
		drm_get_pixel_format_info(frame->shm_format);
	if (!shm_info) {
		nx_log(NX_ERROR,
			"Failed to capture output: no pixel format info matching read format");
		return NULL;
	}

	frame->dmabuf_format = DRM_FORMAT_INVALID;
	if (output->allocator &&
			(output->allocator->buffer_caps & NX_BUFFER_CAP_DMABUF)) {
		frame->dmabuf_format = output->render_format;
	}
	return shm_info;
}

static struct nx_box output_buffer_box(struct nx_output *output,
		const struct nx_box *box) {
	if (box == NULL) {
		return (struct nx_box){
			.width = output->width,
			.height = output->height,
		};
	}

	int ow, oh;
	nx_output_effective_resolution(output, &ow, &oh);

	struct nx_box buffer_box = *box;
	nx_box_transform(&buffer_box, &buffer_box,
		nx_output_transform_invert(output->transform), ow, oh);
	buffer_box.x = (int)(buffer_box.x * output->scale);
	buffer_box.y = (int)(buffer_box.y * output->scale);
	buffer_box.width = (int)(buffer_box.width * output->scale);
	buffer_box.height = (int)(buffer_box.height * output->scale);
	return buffer_box;
}

static bool frame_send_buffer_info(struct screencopy_frame *frame,
		const struct nx_box *box, uint32_t version) {
	const struct nx_pixel_format_info *shm_info = frame_pick_formats(frame);
	if (shm_info == NULL) {
		return false;
	}

	frame->box = output_buffer_box(frame->output, box);
	frame->shm_stride =
		pixel_format_info_min_stride(shm_info, frame->box.width);

	zwlr_screencopy_frame_v1_send_buffer(frame->resource,
		convert_drm_format_to_wl_shm(frame->shm_format),
		frame->box.width, frame->box.height, frame->shm_stride);

	if (version < ZWLR_SCREENCOPY_FRAME_V1_BUFFER_DONE_SINCE_VERSION) {
		return true;
	}
	if (frame->dmabuf_format != DRM_FORMAT_INVALID) {
		zwlr_screencopy_frame_v1_send_linux_dmabuf(frame->resource,
			frame->dmabuf_format, frame->box.width, frame->box.height);
	}
	zwlr_screencopy_frame_v1_send_buffer_done(frame->resource);
	return true;
}

static void capture_output(struct wl_resource *manager_resource, uint32_t id,
		int32_t overlay_cursor, struct nx_output *output,
		const struct nx_box *box) {
	struct screencopy_frame *frame =
		screencopy_frame_create(manager_resource, id);
	if (frame == NULL) {
		return;
	}
	frame->output = output;
	frame->overlay_cursor = !!overlay_cursor;

	if (output == NULL || !output->enabled) {
		wl_resource_set_user_data(frame->resource, NULL);
		zwlr_screencopy_frame_v1_send_failed(frame->resource);
		free(frame);
		return;
	}

	screencopy_frame_start(frame, client_from_resource(manager_resource));

	if (!frame_send_buffer_info(frame, box,
			wl_resource_get_version(manager_resource))) {
		screencopy_frame_fail(frame);
	}
}

static void manager_handle_capture_output(struct wl_client *wl_client,
		struct wl_resource *manager_resource, uint32_t id,
		int32_t overlay_cursor, struct wl_resource *output_resource) {
	capture_output(manager_resource, id, overlay_cursor,
		nx_output_from_resource(output_resource), NULL);
}

static void manager_handle_capture_output_region(struct wl_client *wl_client,
		struct wl_resource *manager_resource, uint32_t id,
		int32_t overlay_cursor, struct wl_resource *output_resource,
		int32_t x, int32_t y, int32_t width, int32_t height) {
	struct nx_box box = {
		.x = x,
		.y = y,
		.width = width,
		.height = height,
	};
	capture_output(manager_resource, id, overlay_cursor,
		nx_output_from_resource(output_resource), &box);
}

static void manager_handle_destroy(struct wl_client *wl_client,
		struct wl_resource *manager_resource) {
	wl_resource_destroy(manager_resource);
}

static const struct zwlr_screencopy_manager_v1_interface manager_impl = {
	.capture_output = manager_handle_capture_output,
	.capture_output_region = manager_handle_capture_output_region,
	.destroy = manager_handle_destroy,
};

static void manager_handle_resource_destroy(struct wl_resource *resource) {
	screencopy_client_unref(client_from_resource(resource));
}

static void manager_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct screencopy_client *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	struct wl_resource *resource = wl_resource_create(wl_client,
		&zwlr_screencopy_manager_v1_interface, version, id);
	if (resource == NULL) {
		free(client);
		wl_client_post_no_memory(wl_client);
		return;
	}

	client->ref = 1;
	wl_list_init(&client->damages);

	wl_resource_set_implementation(resource, &manager_impl, client,
		manager_handle_resource_destroy);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_screencopy_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_screencopy_manager_v1 *nx_screencopy_manager_v1_create(
		struct wl_display *display) {
	struct nx_screencopy_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	manager->global = wl_global_create(display,
		&zwlr_screencopy_manager_v1_interface, SCREENCOPY_MANAGER_VERSION,
		manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}

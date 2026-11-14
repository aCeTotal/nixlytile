#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "platform/buffer/buffer.h"
#include "platform/capture/source/capture_source.h"
#include "platform/log/log.h"
#include "platform/render/allocator/allocator.h"
#include "platform/render/renderer.h"
#include "platform/render/swapchain.h"
#include "ext-image-capture-source-v1-protocol.h"

static void source_handle_destroy(struct wl_client *client,
		struct wl_resource *source_resource) {
	wl_resource_destroy(source_resource);
}

static const struct ext_image_capture_source_v1_interface source_impl = {
	.destroy = source_handle_destroy,
};

struct capture_source *capture_source_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_image_capture_source_v1_interface, &source_impl));
	return wl_resource_get_user_data(resource);
}

static void source_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

void capture_source_init(struct capture_source *source,
		const struct capture_source_impl *impl) {
	*source = (struct capture_source){
		.impl = impl,
	};
	wl_list_init(&source->resources);
	wl_signal_init(&source->events.destroy);
	wl_signal_init(&source->events.constraints_update);
	wl_signal_init(&source->events.frame);
}

void capture_source_finish(struct capture_source *source) {
	wl_signal_emit_mutable(&source->events.destroy, NULL);

	assert(wl_list_empty(&source->events.destroy.listener_list));
	assert(wl_list_empty(&source->events.constraints_update.listener_list));
	assert(wl_list_empty(&source->events.frame.listener_list));

	struct wl_resource *resource, *resource_tmp;
	wl_resource_for_each_safe(resource, resource_tmp, &source->resources) {
		wl_resource_set_user_data(resource, NULL);
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}

	free(source->shm_formats);
	nx_drm_format_set_finish(&source->dmabuf_formats);
}

void capture_source_create_resource(struct capture_source *source,
		struct wl_client *client, uint32_t new_id) {
	struct wl_resource *resource = wl_resource_create(client,
		&ext_image_capture_source_v1_interface, 1, new_id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &source_impl, source,
		source_handle_resource_destroy);
	if (source == NULL) {
		wl_list_init(wl_resource_get_link(resource));
		return;
	}
	wl_list_insert(&source->resources, wl_resource_get_link(resource));
}

static uint32_t get_swapchain_shm_format(struct nx_swapchain *swapchain,
		struct nx_renderer *renderer) {
	struct nx_buffer *buffer = nx_swapchain_acquire(swapchain);
	if (buffer == NULL) {
		return DRM_FORMAT_INVALID;
	}

	struct nx_texture *texture = nx_texture_from_buffer(renderer, buffer);
	nx_buffer_unlock(buffer);
	if (texture == NULL) {
		return DRM_FORMAT_INVALID;
	}

	uint32_t format = nx_texture_preferred_read_format(texture);
	nx_texture_destroy(texture);

	return format;
}

static void add_drm_format(struct nx_drm_format_set *set,
		const struct nx_drm_format *fmt) {
	for (size_t i = 0; i < fmt->len; i++) {
		nx_drm_format_set_add(set, fmt->format, fmt->modifiers[i]);
	}
}

static bool set_shm_format(struct capture_source *source, uint32_t shm_format) {
	uint32_t *shm_formats = calloc(1, sizeof(shm_formats[0]));
	if (shm_formats == NULL) {
		nx_log(NX_ERROR, "Allocation failed");
		return false;
	}
	shm_formats[0] = shm_format;

	source->shm_formats_len = 1;
	free(source->shm_formats);
	source->shm_formats = shm_formats;
	return true;
}

static void set_dmabuf_formats(struct capture_source *source,
		struct nx_swapchain *swapchain, struct nx_renderer *renderer) {
	nx_drm_format_set_finish(&source->dmabuf_formats);
	source->dmabuf_formats = (struct nx_drm_format_set){0};

	add_drm_format(&source->dmabuf_formats, &swapchain->format);

	const struct nx_drm_format_set *render_formats =
		nx_renderer_get_render_formats(renderer);
	assert(render_formats != NULL);

	// Simple clients need 8-bit RGB.
	static const uint32_t fallback_formats[] = {
		DRM_FORMAT_ARGB8888,
		DRM_FORMAT_XRGB8888,
	};
	for (size_t i = 0; i < sizeof(fallback_formats) / sizeof(fallback_formats[0]); i++) {
		const struct nx_drm_format *fmt =
			nx_drm_format_set_get(render_formats, fallback_formats[i]);
		if (fmt != NULL && swapchain->format.format != fmt->format) {
			add_drm_format(&source->dmabuf_formats, fmt);
		}
	}
}

// False only when fstat fails.
static bool set_dmabuf_constraints(struct capture_source *source,
		struct nx_swapchain *swapchain, struct nx_renderer *renderer) {
	int drm_fd = nx_renderer_get_drm_fd(renderer);
	if (swapchain->allocator == NULL ||
			!(swapchain->allocator->buffer_caps & NX_BUFFER_CAP_DMABUF) ||
			drm_fd < 0) {
		return true;
	}

	struct stat dev_stat;
	if (fstat(drm_fd, &dev_stat) != 0) {
		nx_log_errno(NX_ERROR, "fstat() failed");
		return false;
	}

	source->dmabuf_device = dev_stat.st_rdev;
	set_dmabuf_formats(source, swapchain, renderer);
	return true;
}

void capture_source_set_constraints_from_swapchain(
		struct capture_source *source, struct nx_swapchain *swapchain,
		struct nx_renderer *renderer) {
	source->width = swapchain->width;
	source->height = swapchain->height;

	uint32_t shm_format = get_swapchain_shm_format(swapchain, renderer);
	if (shm_format != DRM_FORMAT_INVALID &&
			!set_shm_format(source, shm_format)) {
		return;
	}
	if (!set_dmabuf_constraints(source, swapchain, renderer)) {
		return;
	}

	wl_signal_emit_mutable(&source->events.constraints_update, NULL);
}

void capture_cursor_source_init(struct capture_cursor_source *source,
		const struct capture_source_impl *impl) {
	*source = (struct capture_cursor_source){0};
	capture_source_init(&source->base, impl);
	wl_signal_init(&source->events.update);
}

void capture_cursor_source_finish(struct capture_cursor_source *source) {
	capture_source_finish(&source->base);
	assert(wl_list_empty(&source->events.update.listener_list));
}

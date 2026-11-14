#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include "platform/buffer/buffer_interface.h"
#include "platform/render/drm_format_set.h"
#include "platform/render/renderer.h"
#include "platform/buffer/buffer.h"
#include "platform/buffer/wl_drm.h"
#include "platform/log/log.h"
#include "drm-protocol.h"

#define NX_DRM_VERSION 2

struct nx_drm_buffer {
	struct nx_buffer base;

	struct wl_resource *resource; // NULL after client destroy
	struct nx_dmabuf_attributes dmabuf;
	struct wl_listener release;
};

struct nx_drm {
	struct wl_global *global;
	char *node_name;
	struct nx_drm_format_set formats;
	struct wl_listener display_destroy;
};

static void buffer_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface wl_buffer_impl = {
	.destroy = buffer_handle_destroy,
};

static const struct nx_buffer_impl buffer_impl;

static struct nx_drm_buffer *drm_buffer_from_buffer(
		struct nx_buffer *nx_buffer) {
	assert(nx_buffer->impl == &buffer_impl);
	struct nx_drm_buffer *buffer = wl_container_of(nx_buffer, buffer, base);
	return buffer;
}

static void buffer_destroy(struct nx_buffer *nx_buffer) {
	struct nx_drm_buffer *buffer = drm_buffer_from_buffer(nx_buffer);
	wl_list_remove(&buffer->release.link);

	nx_buffer_finish(nx_buffer);

	if (buffer->resource != NULL) {
		wl_resource_set_user_data(buffer->resource, NULL);
	}
	nx_dmabuf_attributes_finish(&buffer->dmabuf);
	free(buffer);
}

static bool buffer_get_dmabuf(struct nx_buffer *nx_buffer,
		struct nx_dmabuf_attributes *dmabuf) {
	struct nx_drm_buffer *buffer = drm_buffer_from_buffer(nx_buffer);
	*dmabuf = buffer->dmabuf;
	return true;
}

static const struct nx_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
};

static bool buffer_resource_is_instance(struct wl_resource *resource) {
	return wl_resource_instance_of(resource, &wl_buffer_interface,
		&wl_buffer_impl);
}

static struct nx_drm_buffer *drm_buffer_try_from_resource(
		struct wl_resource *resource) {
	if (!buffer_resource_is_instance(resource)) {
		return NULL;
	}
	return wl_resource_get_user_data(resource);
}

static void buffer_handle_resource_destroy(struct wl_resource *resource) {
	struct nx_drm_buffer *buffer = drm_buffer_try_from_resource(resource);
	assert(buffer != NULL);
	buffer->resource = NULL;
	nx_buffer_drop(&buffer->base);
}

static void buffer_handle_release(struct wl_listener *listener, void *data) {
	struct nx_drm_buffer *buffer = wl_container_of(listener, buffer, release);
	if (buffer->resource != NULL) {
		wl_buffer_send_release(buffer->resource);
	}
}

static void drm_handle_authenticate(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	// Render nodes need no auth
	wl_drm_send_authenticated(resource);
}

static void drm_handle_create_buffer(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, uint32_t name, int32_t width,
		int32_t height, uint32_t stride, uint32_t format) {
	wl_resource_post_error(resource, WL_DRM_ERROR_INVALID_NAME,
		"Flink handles are not supported, use DMA-BUF instead");
}

static void drm_handle_create_planar_buffer(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, uint32_t name, int32_t width,
		int32_t height, uint32_t format, int32_t offset0, int32_t stride0,
		int32_t offset1, int32_t stride1, int32_t offset2, int32_t stride2) {
	wl_resource_post_error(resource, WL_DRM_ERROR_INVALID_NAME,
		"Flink handles are not supported, use DMA-BUF instead");
}

static void drm_handle_create_prime_buffer(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, int fd, int32_t width,
		int32_t height, uint32_t format, int32_t offset0, int32_t stride0,
		int32_t offset1, int32_t stride1, int32_t offset2, int32_t stride2) {
	struct nx_dmabuf_attributes dmabuf = {
		.width = width,
		.height = height,
		.format = format,
		.modifier = DRM_FORMAT_MOD_INVALID,
		.n_planes = 1,
		.offset[0] = offset0,
		.stride[0] = stride0,
		.fd[0] = fd,
	};

	struct nx_drm_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		close(fd);
		wl_resource_post_no_memory(resource);
		return;
	}
	nx_buffer_init(&buffer->base, &buffer_impl, width, height);

	buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (buffer->resource == NULL) {
		free(buffer);
		close(fd);
		wl_resource_post_no_memory(resource);
		return;
	}
	wl_resource_set_implementation(buffer->resource, &wl_buffer_impl, buffer,
		buffer_handle_resource_destroy);

	buffer->dmabuf = dmabuf;

	buffer->release.notify = buffer_handle_release;
	wl_signal_add(&buffer->base.events.release, &buffer->release);
}

static const struct wl_drm_interface drm_impl = {
	.authenticate = drm_handle_authenticate,
	.create_buffer = drm_handle_create_buffer,
	.create_planar_buffer = drm_handle_create_planar_buffer,
	.create_prime_buffer = drm_handle_create_prime_buffer,
};

static void drm_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_drm *drm = data;

	struct wl_resource *resource = wl_resource_create(client,
		&wl_drm_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &drm_impl, drm, NULL);

	wl_drm_send_device(resource, drm->node_name);
	wl_drm_send_capabilities(resource, WL_DRM_CAPABILITY_PRIME);

	for (size_t i = 0; i < drm->formats.len; i++) {
		const struct nx_drm_format *fmt = &drm->formats.formats[i];
		if (nx_drm_format_has(fmt, DRM_FORMAT_MOD_INVALID)) {
			wl_drm_send_format(resource, fmt->format);
		}
	}
}

static struct nx_buffer *buffer_from_resource(struct wl_resource *resource) {
	struct nx_drm_buffer *buffer = drm_buffer_try_from_resource(resource);
	assert(buffer != NULL);
	return &buffer->base;
}

static const struct nx_buffer_resource_interface buffer_resource_interface = {
	.name = "nx_drm_buffer",
	.is_instance = buffer_resource_is_instance,
	.from_resource = buffer_from_resource,
};

static void drm_destroy(struct nx_drm *drm) {
	wl_list_remove(&drm->display_destroy.link);

	nx_drm_format_set_finish(&drm->formats);
	free(drm->node_name);
	wl_global_destroy(drm->global);
	free(drm);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_drm *drm = wl_container_of(listener, drm, display_destroy);
	drm_destroy(drm);
}

struct nx_drm *nx_drm_create(struct wl_display *display,
		struct nx_renderer *renderer) {
	int drm_fd = nx_renderer_get_drm_fd(renderer);
	if (drm_fd < 0) {
		nx_log(NX_ERROR, "Failed to get DRM FD from renderer");
		return NULL;
	}

	drmDevice *dev = NULL;
	if (drmGetDevice2(drm_fd, 0, &dev) != 0) {
		nx_log(NX_ERROR, "drmGetDevice2 failed");
		return NULL;
	}

	char *node_name = NULL;
	if (dev->available_nodes & (1 << DRM_NODE_RENDER)) {
		node_name = strdup(dev->nodes[DRM_NODE_RENDER]);
	} else {
		assert(dev->available_nodes & (1 << DRM_NODE_PRIMARY));
		nx_log(NX_DEBUG, "No DRM render node available, "
			"falling back to primary node '%s'", dev->nodes[DRM_NODE_PRIMARY]);
		node_name = strdup(dev->nodes[DRM_NODE_PRIMARY]);
	}
	drmFreeDevice(&dev);
	if (node_name == NULL) {
		return NULL;
	}

	struct nx_drm *drm = calloc(1, sizeof(*drm));
	if (drm == NULL) {
		free(node_name);
		return NULL;
	}

	drm->node_name = node_name;

	const struct nx_drm_format_set *formats =
		nx_renderer_get_texture_formats(renderer, NX_BUFFER_CAP_DMABUF);
	if (formats == NULL) {
		goto error;
	}

	if (!nx_drm_format_set_copy(&drm->formats, formats)) {
		goto error;
	}

	drm->global = wl_global_create(display, &wl_drm_interface, NX_DRM_VERSION,
		drm, drm_bind);
	if (drm->global == NULL) {
		goto error;
	}

	drm->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &drm->display_destroy);

	nx_buffer_register_resource_interface(&buffer_resource_interface);

	return drm;

error:
	nx_drm_format_set_finish(&drm->formats);
	free(drm->node_name);
	free(drm);
	return NULL;
}

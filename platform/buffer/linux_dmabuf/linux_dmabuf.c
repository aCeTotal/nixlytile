#include <assert.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>
#include "linux-dmabuf-v1-protocol.h"
#include "platform/buffer/buffer_interface.h"
#include "platform/buffer/linux_dmabuf/feedback.h"
#include "platform/buffer/linux_dmabuf/requests.h"
#include "platform/log/log.h"
#include "platform/render/drm_format_set.h"

#define LINUX_DMABUF_VERSION 5

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_impl;

struct nx_linux_dmabuf_v1 *nx_linux_dmabuf_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zwp_linux_dmabuf_v1_interface,
			&linux_dmabuf_impl));

	struct nx_linux_dmabuf_v1 *dmabuf = wl_resource_get_user_data(resource);
	assert(dmabuf);
	return dmabuf;
}

static void linux_dmabuf_get_default_feedback(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	struct nx_linux_dmabuf_v1 *linux_dmabuf =
		nx_linux_dmabuf_from_resource(resource);

	struct wl_resource *feedback_resource =
		nx_linux_dmabuf_feedback_resource_create(resource, id, NULL);
	if (feedback_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	nx_linux_dmabuf_feedback_send(linux_dmabuf->default_feedback, feedback_resource);
}

static void linux_dmabuf_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_impl = {
	.destroy = linux_dmabuf_destroy,
	.create_params = nx_linux_dmabuf_create_params,
	.get_default_feedback = linux_dmabuf_get_default_feedback,
	.get_surface_feedback = nx_linux_dmabuf_get_surface_feedback,
};

static void send_modifier(struct wl_resource *resource, uint32_t format,
		uint64_t modifier) {
	zwp_linux_dmabuf_v1_send_modifier(resource, format,
		modifier >> 32, modifier & 0xFFFFFFFF);
}

static void send_legacy_format(struct wl_resource *resource,
		const struct nx_drm_format *fmt) {
	if (!nx_drm_format_has(fmt, DRM_FORMAT_MOD_INVALID)) {
		return;
	}
	zwp_linux_dmabuf_v1_send_format(resource, fmt->format);
}

static void linux_dmabuf_send_modifiers(struct wl_resource *resource,
		const struct nx_drm_format *fmt) {
	if (wl_resource_get_version(resource) < ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
		send_legacy_format(resource, fmt);
		return;
	}

	// Xwayland mishandles INVALID plus LINEAR
	if (fmt->len == 2 && nx_drm_format_has(fmt, DRM_FORMAT_MOD_INVALID)
			&& nx_drm_format_has(fmt, DRM_FORMAT_MOD_LINEAR)) {
		send_modifier(resource, fmt->format, DRM_FORMAT_MOD_INVALID);
		return;
	}

	for (size_t i = 0; i < fmt->len; i++) {
		send_modifier(resource, fmt->format, fmt->modifiers[i]);
	}
}

static void linux_dmabuf_send_formats(struct nx_linux_dmabuf_v1 *linux_dmabuf,
		struct wl_resource *resource) {
	for (size_t i = 0; i < linux_dmabuf->default_formats.len; i++) {
		const struct nx_drm_format *fmt = &linux_dmabuf->default_formats.formats[i];
		linux_dmabuf_send_modifiers(resource, fmt);
	}
}

static void linux_dmabuf_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_linux_dmabuf_v1 *linux_dmabuf = data;

	struct wl_resource *resource = wl_resource_create(client,
		&zwp_linux_dmabuf_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &linux_dmabuf_impl,
		linux_dmabuf, NULL);

	if (version < ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) {
		linux_dmabuf_send_formats(linux_dmabuf, resource);
	}
}

static void linux_dmabuf_v1_destroy(struct nx_linux_dmabuf_v1 *linux_dmabuf) {
	wl_signal_emit_mutable(&linux_dmabuf->events.destroy, linux_dmabuf);

	assert(wl_list_empty(&linux_dmabuf->events.destroy.listener_list));

	nx_linux_dmabuf_surfaces_destroy(linux_dmabuf);

	nx_linux_dmabuf_feedback_destroy(linux_dmabuf->default_feedback);
	nx_drm_format_set_finish(&linux_dmabuf->default_formats);
	if (linux_dmabuf->main_device_fd >= 0) {
		close(linux_dmabuf->main_device_fd);
	}

	wl_list_remove(&linux_dmabuf->display_destroy.link);

	wl_global_destroy(linux_dmabuf->global);
	free(linux_dmabuf);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_linux_dmabuf_v1 *linux_dmabuf =
		wl_container_of(listener, linux_dmabuf, display_destroy);
	linux_dmabuf_v1_destroy(linux_dmabuf);
}

static bool union_tranche_formats(struct nx_drm_format_set *formats,
		const struct nx_linux_dmabuf_feedback_v1 *feedback) {
	const struct nx_linux_dmabuf_feedback_v1_tranche *tranche;
	wl_array_for_each(tranche, &feedback->tranches) {
		if (!nx_drm_format_set_union(formats, formats, &tranche->formats)) {
			return false;
		}
	}
	return true;
}

/* Render node validates client imports */
static int open_main_device(const drmDevice *device) {
	if (!(device->available_nodes & (1 << DRM_NODE_RENDER))) {
		assert(device->available_nodes & (1 << DRM_NODE_PRIMARY));
		nx_log(NX_DEBUG, "DRM device %s has no render node, "
			"skipping DMA-BUF import checks", device->nodes[DRM_NODE_PRIMARY]);
		return -1;
	}

	const char *name = device->nodes[DRM_NODE_RENDER];
	int fd = open(name, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		nx_log_errno(NX_ERROR, "Failed to open DRM device %s", name);
	}
	return fd;
}

static bool set_default_feedback(struct nx_linux_dmabuf_v1 *linux_dmabuf,
		const struct nx_linux_dmabuf_feedback_v1 *feedback) {
	struct nx_linux_dmabuf_feedback_v1_compiled *compiled =
		nx_linux_dmabuf_feedback_compile(feedback);
	if (compiled == NULL) {
		return false;
	}

	drmDevice *device = NULL;
	if (drmGetDeviceFromDevId(feedback->main_device, 0, &device) != 0) {
		nx_log_errno(NX_ERROR, "drmGetDeviceFromDevId failed");
		goto error_compiled;
	}

	bool has_render_node = device->available_nodes & (1 << DRM_NODE_RENDER);
	int main_device_fd = open_main_device(device);
	drmFreeDevice(&device);
	if (has_render_node && main_device_fd < 0) {
		goto error_compiled;
	}

	struct nx_drm_format_set formats = {0};
	if (!union_tranche_formats(&formats, feedback)) {
		goto error_formats;
	}

	nx_linux_dmabuf_feedback_destroy(linux_dmabuf->default_feedback);
	linux_dmabuf->default_feedback = compiled;

	if (linux_dmabuf->main_device_fd >= 0) {
		close(linux_dmabuf->main_device_fd);
	}
	linux_dmabuf->main_device_fd = main_device_fd;

	nx_drm_format_set_finish(&linux_dmabuf->default_formats);
	linux_dmabuf->default_formats = formats;

	return true;

error_formats:
	nx_drm_format_set_finish(&formats);
error_compiled:
	nx_linux_dmabuf_feedback_destroy(compiled);
	return false;
}

static struct nx_linux_dmabuf_v1 *linux_dmabuf_v1_create(struct wl_display *display,
		uint32_t version, const struct nx_linux_dmabuf_feedback_v1 *default_feedback) {
	assert(version <= LINUX_DMABUF_VERSION);

	struct nx_linux_dmabuf_v1 *linux_dmabuf = calloc(1, sizeof(*linux_dmabuf));
	if (linux_dmabuf == NULL) {
		nx_log(NX_ERROR, "could not create simple dmabuf manager");
		return NULL;
	}
	linux_dmabuf->main_device_fd = -1;

	wl_list_init(&linux_dmabuf->surfaces);

	wl_signal_init(&linux_dmabuf->events.destroy);

	linux_dmabuf->global = wl_global_create(display, &zwp_linux_dmabuf_v1_interface,
		version, linux_dmabuf, linux_dmabuf_bind);
	if (!linux_dmabuf->global) {
		nx_log(NX_ERROR, "could not create linux dmabuf v1 wl global");
		goto error_linux_dmabuf;
	}

	if (!set_default_feedback(linux_dmabuf, default_feedback)) {
		goto error_global;
	}

	linux_dmabuf->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &linux_dmabuf->display_destroy);

	nx_buffer_register_resource_interface(&nx_dmabuf_v1_buffer_resource_interface);

	return linux_dmabuf;

error_global:
	wl_global_destroy(linux_dmabuf->global);
error_linux_dmabuf:
	free(linux_dmabuf);
	return NULL;
}

struct nx_linux_dmabuf_v1 *nx_linux_dmabuf_v1_create_with_renderer(struct wl_display *display,
		uint32_t version, struct nx_renderer *renderer) {
	const struct nx_linux_dmabuf_feedback_v1_init_options options = {
		.main_renderer = renderer,
	};
	struct nx_linux_dmabuf_feedback_v1 feedback = {0};
	if (!nx_linux_dmabuf_feedback_v1_init_with_options(&feedback, &options)) {
		return NULL;
	}
	struct nx_linux_dmabuf_v1 *linux_dmabuf =
		linux_dmabuf_v1_create(display, version, &feedback);
	nx_linux_dmabuf_feedback_v1_finish(&feedback);
	return linux_dmabuf;
}

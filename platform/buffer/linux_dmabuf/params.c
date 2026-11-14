#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>
#include "linux-dmabuf-v1-protocol.h"
#include "platform/buffer/linux_dmabuf/requests.h"
#include "platform/log/log.h"

struct nx_linux_buffer_params_v1 {
	struct wl_resource *resource;
	struct nx_linux_dmabuf_v1 *linux_dmabuf;
	struct nx_dmabuf_attributes attributes;
	bool has_modifier;
};

static const struct zwp_linux_buffer_params_v1_interface buffer_params_impl;

static struct nx_linux_buffer_params_v1 *params_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwp_linux_buffer_params_v1_interface, &buffer_params_impl));
	return wl_resource_get_user_data(resource);
}

static void params_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void params_add(struct wl_client *client,
		struct wl_resource *params_resource, int32_t fd,
		uint32_t plane_idx, uint32_t offset, uint32_t stride,
		uint32_t modifier_hi, uint32_t modifier_lo) {
	struct nx_linux_buffer_params_v1 *params =
		params_from_resource(params_resource);
	if (!params) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
			"params was already used to create a wl_buffer");
		close(fd);
		return;
	}

	if (plane_idx >= NX_DMABUF_MAX_PLANES) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
			"plane index %u > %u", plane_idx, NX_DMABUF_MAX_PLANES);
		close(fd);
		return;
	}

	if (params->attributes.fd[plane_idx] != -1) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
			"a dmabuf with FD %d has already been added for plane %u",
			params->attributes.fd[plane_idx], plane_idx);
		close(fd);
		return;
	}

	uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
	if (params->has_modifier && modifier != params->attributes.modifier) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			"sent modifier %" PRIu64 " for plane %u, expected"
			" modifier %" PRIu64 " like other planes",
			modifier, plane_idx, params->attributes.modifier);
		close(fd);
		return;
	}

	params->attributes.modifier = modifier;
	params->has_modifier = true;

	params->attributes.fd[plane_idx] = fd;
	params->attributes.offset[plane_idx] = offset;
	params->attributes.stride[plane_idx] = stride;
	params->attributes.n_planes++;
}

/* Main device must import planes */
static bool check_import_dmabuf(struct nx_linux_dmabuf_v1 *linux_dmabuf,
		const struct nx_dmabuf_attributes *attribs) {
	if (linux_dmabuf->main_device_fd < 0) {
		return true;
	}

	for (int i = 0; i < attribs->n_planes; i++) {
		uint32_t handle = 0;
		if (drmPrimeFDToHandle(linux_dmabuf->main_device_fd, attribs->fd[i], &handle) != 0) {
			nx_log_errno(NX_ERROR, "Failed to import DMA-BUF FD for plane %d", i);
			return false;
		}
		if (drmCloseBufferHandle(linux_dmabuf->main_device_fd, handle) != 0) {
			nx_log_errno(NX_ERROR, "Failed to close buffer handle for plane %d", i);
			return false;
		}
	}
	return true;
}

static void params_fail(struct wl_resource *params_resource, uint32_t buffer_id) {
	if (buffer_id == 0) {
		zwp_linux_buffer_params_v1_send_failed(params_resource);
		return;
	}
	// Failed create_immed kills the client
	wl_resource_post_error(params_resource,
		ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
		"importing the supplied dmabufs failed");
}

static void params_create_common(struct wl_resource *params_resource,
		uint32_t buffer_id, int32_t width, int32_t height, uint32_t format,
		uint32_t flags) {
	struct nx_linux_buffer_params_v1 *params =
		params_from_resource(params_resource);
	if (!params) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
			"params was already used to create a wl_buffer");
		return;
	}

	struct nx_dmabuf_attributes attribs = params->attributes;
	struct nx_linux_dmabuf_v1 *linux_dmabuf = params->linux_dmabuf;

	// Make the params resource inert
	wl_resource_set_user_data(params_resource, NULL);
	free(params);

	if (!nx_linux_dmabuf_check_planes(params_resource, &attribs)) {
		goto err_out;
	}

	uint32_t version = wl_resource_get_version(params_resource);
	if (!zwp_linux_buffer_params_v1_flags_is_valid(flags, version)) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
			"Unknown dmabuf flags %"PRIu32, flags);
		goto err_out;
	}

	if (flags != 0) {
		nx_log(NX_ERROR, "dmabuf flags aren't supported");
		goto err_failed;
	}

	attribs.width = width;
	attribs.height = height;
	attribs.format = format;

	if (width < 1 || height < 1) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
			"invalid width %d or height %d", width, height);
		goto err_out;
	}

	if (!nx_linux_dmabuf_check_plane_bounds(params_resource, &attribs)) {
		goto err_out;
	}

	if (!check_import_dmabuf(linux_dmabuf, &attribs) ||
			!nx_dmabuf_v1_buffer_create(params_resource, buffer_id, &attribs)) {
		goto err_failed;
	}
	return;

err_failed:
	params_fail(params_resource, buffer_id);
err_out:
	nx_dmabuf_attributes_finish(&attribs);
}

static void params_create(struct wl_client *client,
		struct wl_resource *params_resource,
		int32_t width, int32_t height, uint32_t format, uint32_t flags) {
	params_create_common(params_resource, 0, width, height, format,
		flags);
}

static void params_create_immed(struct wl_client *client,
		struct wl_resource *params_resource, uint32_t buffer_id,
		int32_t width, int32_t height, uint32_t format, uint32_t flags) {
	params_create_common(params_resource, buffer_id, width, height,
		format, flags);
}

static const struct zwp_linux_buffer_params_v1_interface buffer_params_impl = {
	.destroy = params_destroy,
	.add = params_add,
	.create = params_create,
	.create_immed = params_create_immed,
};

static void params_handle_resource_destroy(struct wl_resource *resource) {
	struct nx_linux_buffer_params_v1 *params = params_from_resource(resource);
	if (!params) {
		return;
	}
	nx_dmabuf_attributes_finish(&params->attributes);
	free(params);
}

void nx_linux_dmabuf_create_params(struct wl_client *client,
		struct wl_resource *linux_dmabuf_resource,
		uint32_t params_id) {
	struct nx_linux_dmabuf_v1 *linux_dmabuf =
		nx_linux_dmabuf_from_resource(linux_dmabuf_resource);

	struct nx_linux_buffer_params_v1 *params = calloc(1, sizeof(*params));
	if (!params) {
		wl_resource_post_no_memory(linux_dmabuf_resource);
		return;
	}

	for (int i = 0; i < NX_DMABUF_MAX_PLANES; i++) {
		params->attributes.fd[i] = -1;
	}

	params->linux_dmabuf = linux_dmabuf;

	uint32_t version = wl_resource_get_version(linux_dmabuf_resource);
	params->resource = wl_resource_create(client,
		&zwp_linux_buffer_params_v1_interface, version, params_id);
	if (!params->resource) {
		free(params);
		wl_resource_post_no_memory(linux_dmabuf_resource);
		return;
	}
	wl_resource_set_implementation(params->resource,
		&buffer_params_impl, params, params_handle_resource_destroy);
}

#include <inttypes.h>
#include <unistd.h>
#include "linux-dmabuf-v1-protocol.h"
#include "platform/buffer/linux_dmabuf/requests.h"

bool nx_linux_dmabuf_check_planes(struct wl_resource *params_resource,
		const struct nx_dmabuf_attributes *attribs) {
	if (!attribs->n_planes) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			"no dmabuf has been added to the params");
		return false;
	}

	if (attribs->fd[0] == -1) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			"no dmabuf has been added for plane 0");
		return false;
	}

	if ((attribs->fd[3] >= 0 || attribs->fd[2] >= 0) &&
			(attribs->fd[2] == -1 || attribs->fd[1] == -1)) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
			"gap in dmabuf planes");
		return false;
	}
	return true;
}

static bool check_plane_bounds(struct wl_resource *params_resource,
		const struct nx_dmabuf_attributes *attribs, int i) {
	if ((uint64_t)attribs->offset[i] + attribs->stride[i] > UINT32_MAX ||
			(uint64_t)attribs->offset[i]
			+ (uint64_t)attribs->stride[i] * attribs->height > UINT32_MAX) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			"size overflow for plane %d", i);
		return false;
	}

	off_t size = lseek(attribs->fd[i], 0, SEEK_END);
	if (size == -1) {
		return true;
	}
	if (attribs->offset[i] > size) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			"invalid offset %" PRIu32 " for plane %d",
			attribs->offset[i], i);
		return false;
	}

	if (attribs->offset[i] + attribs->stride[i] > size ||
			attribs->stride[i] == 0) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			"invalid stride %" PRIu32 " for plane %d",
			attribs->stride[i], i);
		return false;
	}

	// Later planes may be subsampled
	if (i == 0 && attribs->offset[i] +
			attribs->stride[i] * attribs->height > size) {
		wl_resource_post_error(params_resource,
			ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
			"invalid buffer stride or height for plane %d", i);
		return false;
	}
	return true;
}

bool nx_linux_dmabuf_check_plane_bounds(struct wl_resource *params_resource,
		const struct nx_dmabuf_attributes *attribs) {
	for (int i = 0; i < attribs->n_planes; i++) {
		if (!check_plane_bounds(params_resource, attribs, i)) {
			return false;
		}
	}
	return true;
}

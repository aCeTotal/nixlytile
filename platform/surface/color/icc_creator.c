#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "color-management-v1-protocol.h"
#include "platform/surface/color/image_description.h"
#include "platform/surface/color/manager_requests.h"

#define ICC_MAX_SIZE (32u * 1024 * 1024) // protocol limit
#define ICC_READ_CHUNK 16384

struct icc_creator {
	bool icc_set;
};

struct icc_rejection {
	enum wp_image_description_creator_icc_v1_error code;
	const char *msg;
};

static const struct wp_image_description_creator_icc_v1_interface icc_creator_impl;

static struct icc_creator *icc_creator_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_image_description_creator_icc_v1_interface,
		&icc_creator_impl));
	return wl_resource_get_user_data(resource);
}

static void icc_creator_handle_create(struct wl_client *client,
		struct wl_resource *creator_resource, uint32_t id) {
	struct icc_creator *creator = icc_creator_from_resource(creator_resource);

	if (!creator->icc_set) {
		wl_resource_post_error(creator_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_INCOMPLETE_SET,
			"ICC file not set");
		return;
	}

	static const struct color_image_desc_failure unsupported = {
		.cause = WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED,
		.msg = "ICC profile support not compiled in (no lcms2)",
	};
	color_image_desc_create_failed(creator_resource, id, &unsupported);
	wl_resource_destroy(creator_resource);
}

// Unparsed; reading validates the fd
static bool drain_icc_data(int fd, uint32_t length) {
	char chunk[ICC_READ_CHUNK];
	uint32_t left = length;
	while (left > 0) {
		ssize_t n = read(fd, chunk, left < sizeof(chunk) ? left : sizeof(chunk));
		if (n <= 0) {
			return false;
		}
		left -= (uint32_t)n;
	}
	return true;
}

static const struct icc_rejection *check_icc_file(int fd, uint32_t offset, uint32_t length) {
	static const struct icc_rejection bad_size = {
		.code = WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_BAD_SIZE,
		.msg = "ICC data length must be between 1 and 32MB",
	};
	static const struct icc_rejection not_seekable = {
		.code = WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_BAD_FD,
		.msg = "fd is not seekable",
	};
	static const struct icc_rejection out_of_file = {
		.code = WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_OUT_OF_FILE,
		.msg = "offset + length exceeds file size",
	};
	static const struct icc_rejection seek_failed = {
		.code = WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_BAD_FD,
		.msg = "failed to seek to offset",
	};
	static const struct icc_rejection read_failed = {
		.code = WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_BAD_FD,
		.msg = "failed to read ICC data",
	};

	if (length == 0 || length > ICC_MAX_SIZE) {
		return &bad_size;
	}
	if (lseek(fd, 0, SEEK_CUR) == (off_t)-1) {
		return &not_seekable;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || (uint64_t)offset + length > (uint64_t)st.st_size) {
		return &out_of_file;
	}
	if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
		return &seek_failed;
	}
	if (!drain_icc_data(fd, length)) {
		return &read_failed;
	}
	return NULL;
}

static void icc_creator_handle_set_icc_file(struct wl_client *client,
		struct wl_resource *creator_resource, int32_t fd,
		uint32_t offset, uint32_t length) {
	struct icc_creator *creator = icc_creator_from_resource(creator_resource);

	if (creator->icc_set) {
		close(fd);
		wl_resource_post_error(creator_resource,
			WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_ALREADY_SET,
			"ICC file already set");
		return;
	}

	const struct icc_rejection *rejection = check_icc_file(fd, offset, length);
	close(fd);
	if (rejection != NULL) {
		wl_resource_post_error(creator_resource, rejection->code, "%s", rejection->msg);
		return;
	}
	creator->icc_set = true;
}

static const struct wp_image_description_creator_icc_v1_interface icc_creator_impl = {
	.create = icc_creator_handle_create,
	.set_icc_file = icc_creator_handle_set_icc_file,
};

static void icc_creator_handle_resource_destroy(struct wl_resource *resource) {
	free(icc_creator_from_resource(resource));
}

void color_manager_create_icc_creator(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id) {
	struct nx_color_manager_v1 *manager = color_manager_from_resource(manager_resource);
	if (!manager->features.icc_v2_v4) {
		wl_resource_post_error(manager_resource,
			WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE,
			"new_icc_creator is not supported");
		return;
	}

	struct icc_creator *creator = calloc(1, sizeof(*creator));
	if (!creator) {
		wl_client_post_no_memory(client);
		return;
	}

	uint32_t version = wl_resource_get_version(manager_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&wp_image_description_creator_icc_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		free(creator);
		return;
	}
	wl_resource_set_implementation(resource, &icc_creator_impl,
		creator, icc_creator_handle_resource_destroy);
}

#include <assert.h>
#include <sys/stat.h>
#include "linux-dmabuf-v1-protocol.h"
#include "platform/backend/backend.h"
#include "platform/buffer/linux_dmabuf.h"
#include "platform/drm/drm.h"
#include "platform/log/log.h"
#include "platform/output/output.h"
#include "platform/render/drm_format_set.h"
#include "platform/render/renderer.h"

static struct nx_linux_dmabuf_feedback_v1_tranche *feedback_add_tranche(
		struct nx_linux_dmabuf_feedback_v1 *feedback) {
	struct nx_linux_dmabuf_feedback_v1_tranche *tranche =
		wl_array_add(&feedback->tranches, sizeof(*tranche));
	if (tranche == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return NULL;
	}
	*tranche = (struct nx_linux_dmabuf_feedback_v1_tranche){0};
	return tranche;
}

void nx_linux_dmabuf_feedback_v1_finish(struct nx_linux_dmabuf_feedback_v1 *feedback) {
	struct nx_linux_dmabuf_feedback_v1_tranche *tranche;
	wl_array_for_each(tranche, &feedback->tranches) {
		nx_drm_format_set_finish(&tranche->formats);
	}
	wl_array_release(&feedback->tranches);
}

static bool devid_from_fd(int fd, dev_t *devid) {
	struct stat stat;
	if (fstat(fd, &stat) != 0) {
		nx_log_errno(NX_ERROR, "fstat failed");
		return false;
	}
	*devid = stat.st_rdev;
	return true;
}

static bool is_secondary_drm_backend(struct nx_backend *backend) {
	return nx_backend_is_drm(backend) &&
		nx_drm_backend_get_parent(backend) != NULL;
}

static bool add_scanout_tranche(struct nx_linux_dmabuf_feedback_v1 *feedback,
		struct nx_output *output, const struct nx_drm_format_set *renderer_formats) {
	int backend_drm_fd = nx_backend_get_drm_fd(output->backend);
	if (backend_drm_fd < 0) {
		nx_log(NX_ERROR, "Failed to get backend DRM FD");
		return false;
	}
	dev_t backend_dev;
	if (!devid_from_fd(backend_drm_fd, &backend_dev)) {
		return false;
	}

	const struct nx_drm_format_set *scanout_formats =
		nx_output_get_primary_formats(output, NX_BUFFER_CAP_DMABUF);
	if (scanout_formats == NULL) {
		nx_log(NX_ERROR, "Failed to get output primary DMA-BUF formats");
		return false;
	}

	struct nx_linux_dmabuf_feedback_v1_tranche *tranche =
		feedback_add_tranche(feedback);
	if (tranche == NULL) {
		return false;
	}

	tranche->target_device = backend_dev;
	tranche->flags = ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT;
	if (!nx_drm_format_set_intersect(&tranche->formats, scanout_formats, renderer_formats)) {
		nx_log(NX_ERROR, "Failed to intersect renderer and scanout formats");
		return false;
	}
	return true;
}

bool nx_linux_dmabuf_feedback_v1_init_with_options(struct nx_linux_dmabuf_feedback_v1 *feedback,
		const struct nx_linux_dmabuf_feedback_v1_init_options *options) {
	assert(options->main_renderer != NULL);

	*feedback = (struct nx_linux_dmabuf_feedback_v1){0};

	int renderer_drm_fd = nx_renderer_get_drm_fd(options->main_renderer);
	if (renderer_drm_fd < 0) {
		nx_log(NX_ERROR, "Failed to get renderer DRM FD");
		goto error;
	}
	dev_t renderer_dev;
	if (!devid_from_fd(renderer_drm_fd, &renderer_dev)) {
		goto error;
	}

	feedback->main_device = renderer_dev;

	const struct nx_drm_format_set *renderer_formats =
		nx_renderer_get_texture_formats(options->main_renderer, NX_BUFFER_CAP_DMABUF);
	if (renderer_formats == NULL) {
		nx_log(NX_ERROR, "Failed to get renderer DMA-BUF texture formats");
		goto error;
	}

	struct nx_output *scanout = options->scanout_primary_output;
	if (scanout != NULL && !is_secondary_drm_backend(scanout->backend) &&
			!add_scanout_tranche(feedback, scanout, renderer_formats)) {
		goto error;
	}

	struct nx_linux_dmabuf_feedback_v1_tranche *tranche =
		feedback_add_tranche(feedback);
	if (tranche == NULL) {
		goto error;
	}

	tranche->target_device = renderer_dev;
	if (!nx_drm_format_set_copy(&tranche->formats, renderer_formats)) {
		goto error;
	}

	return true;

error:
	nx_linux_dmabuf_feedback_v1_finish(feedback);
	return false;
}

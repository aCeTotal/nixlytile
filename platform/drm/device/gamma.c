#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <xf86drmMode.h>
#include "platform/drm/device/device.h"
#include "platform/log/log.h"
#include "platform/render/color.h"

static void fill_linear_gamma(size_t size, uint16_t *r, uint16_t *g, uint16_t *b) {
	assert(0xFFFF < UINT64_MAX / (size - 1));
	for (uint32_t i = 0; i < size; ++i) {
		uint16_t val = (uint64_t)0xFFFF * i / (size - 1);
		r[i] = g[i] = b[i] = val;
	}
}

static bool set_gamma(struct nx_drm_backend *drm, struct nx_drm_crtc *crtc,
		size_t size, const uint16_t *lut) {
	const uint16_t *r = lut, *g = lut + size, *b = lut + 2 * size;
	if (drmModeCrtcSetGamma(drm->fd, crtc->id, size, r, g, b) != 0) {
		nx_log_errno(NX_ERROR, "Failed to set gamma LUT on CRTC %"PRIu32,
			crtc->id);
		return false;
	}
	return true;
}

bool drm_legacy_crtc_set_gamma(struct nx_drm_backend *drm,
		struct nx_drm_crtc *crtc, struct nx_color_transform *transform) {
	struct nx_color_transform_lut_3x1d *lut = transform != NULL ?
		color_transform_lut_3x1d_from_base(transform) : NULL;
	if (lut != NULL && lut->dim != 0) {
		return set_gamma(drm, crtc, lut->dim, lut->lut_3x1d);
	}

	// Legacy can't reset; load identity
	size_t size = crtc->gamma_lut_size;
	if (size == 0) {
		return false;
	}

	uint16_t *linear_lut = malloc(3 * size * sizeof(uint16_t));
	if (linear_lut == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return false;
	}
	fill_linear_gamma(size, linear_lut, linear_lut + size, linear_lut + 2 * size);

	bool ok = set_gamma(drm, crtc, size, linear_lut);
	free(linear_lut);
	return ok;
}

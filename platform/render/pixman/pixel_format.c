#include <drm_fourcc.h>
#include "platform/log/log.h"
#include "platform/render/pixman/pixman_renderer.h"

struct pixman_pixel_format {
	uint32_t drm_format;
	pixman_format_code_t pixman_format;
};

/* Little-endian only: multi-byte channels match */
static const struct pixman_pixel_format formats[] = {
	{ DRM_FORMAT_ARGB8888, PIXMAN_a8r8g8b8 },
	{ DRM_FORMAT_XBGR8888, PIXMAN_x8b8g8r8 },
	{ DRM_FORMAT_XRGB8888, PIXMAN_x8r8g8b8 },
	{ DRM_FORMAT_ABGR8888, PIXMAN_a8b8g8r8 },
	{ DRM_FORMAT_RGBA8888, PIXMAN_r8g8b8a8 },
	{ DRM_FORMAT_RGBX8888, PIXMAN_r8g8b8x8 },
	{ DRM_FORMAT_BGRA8888, PIXMAN_b8g8r8a8 },
	{ DRM_FORMAT_BGRX8888, PIXMAN_b8g8r8x8 },
	{ DRM_FORMAT_RGB565, PIXMAN_r5g6b5 },
	{ DRM_FORMAT_BGR565, PIXMAN_b5g6r5 },
	{ DRM_FORMAT_ARGB2101010, PIXMAN_a2r10g10b10 },
	{ DRM_FORMAT_XRGB2101010, PIXMAN_x2r10g10b10 },
	{ DRM_FORMAT_ABGR2101010, PIXMAN_a2b10g10r10 },
	{ DRM_FORMAT_XBGR2101010, PIXMAN_x2b10g10r10 },
	{ DRM_FORMAT_ABGR16161616, PIXMAN_a16b16g16r16 },
};

#define FORMATS_LEN (sizeof(formats) / sizeof(formats[0]))

pixman_format_code_t get_pixman_format_from_drm(uint32_t fmt) {
	for (size_t i = 0; i < FORMATS_LEN; ++i) {
		if (formats[i].drm_format == fmt) {
			return formats[i].pixman_format;
		}
	}

	nx_log(NX_ERROR, "DRM format 0x%"PRIX32" has no pixman equivalent", fmt);
	return 0;
}

uint32_t get_drm_format_from_pixman(pixman_format_code_t fmt) {
	for (size_t i = 0; i < FORMATS_LEN; ++i) {
		if (formats[i].pixman_format == fmt) {
			return formats[i].drm_format;
		}
	}

	nx_log(NX_ERROR, "pixman format 0x%"PRIX32" has no drm equivalent", fmt);
	return DRM_FORMAT_INVALID;
}

const uint32_t *get_pixman_drm_formats(size_t *len) {
	static uint32_t drm_formats[FORMATS_LEN];
	*len = FORMATS_LEN;
	for (size_t i = 0; i < FORMATS_LEN; i++) {
		drm_formats[i] = formats[i].drm_format;
	}
	return drm_formats;
}

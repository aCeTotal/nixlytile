#include <math.h>
#include <stdlib.h>
#include "platform/buffer/buffer.h"
#include "platform/drm/atomic/atomic.h"
#include "platform/log/log.h"
#include "platform/render/color.h"

#define CTA861_EOTF_ST2084 2
#define CTA861_COORD_SCALE 50000
#define CTA861_MIN_LUMINANCE_SCALE 0.0001

bool drm_atomic_mode_blob(const struct nx_drm_connector_state *state,
		uint32_t *blob_id) {
	if (!state->active) {
		*blob_id = 0;
		return true;
	}

	if (drmModeCreatePropertyBlob(state->connector->backend->fd, &state->mode,
			sizeof(drmModeModeInfo), blob_id)) {
		nx_log_errno(NX_ERROR, "Unable to create mode property blob");
		return false;
	}

	return true;
}

bool drm_atomic_gamma_blob(struct nx_drm_backend *drm,
		struct nx_color_transform *transform, uint32_t *blob_id) {
	struct nx_color_transform_lut_3x1d *lut = transform != NULL ?
		color_transform_lut_3x1d_from_base(transform) : NULL;
	size_t size = lut != NULL ? lut->dim : 0;
	if (size == 0) {
		*blob_id = 0;
		return true;
	}

	struct drm_color_lut *gamma = malloc(size * sizeof(*gamma));
	if (gamma == NULL) {
		nx_log(NX_ERROR, "Failed to allocate gamma table");
		return false;
	}

	const uint16_t *r = lut->lut_3x1d;
	const uint16_t *g = lut->lut_3x1d + size;
	const uint16_t *b = lut->lut_3x1d + 2 * size;
	for (size_t i = 0; i < size; i++) {
		gamma[i].red = r[i];
		gamma[i].green = g[i];
		gamma[i].blue = b[i];
	}

	int ret = drmModeCreatePropertyBlob(drm->fd, gamma,
		size * sizeof(*gamma), blob_id);
	if (ret != 0) {
		nx_log_errno(NX_ERROR, "Unable to create gamma LUT property blob");
	}
	free(gamma);
	return ret == 0;
}

/* Zero if undamaged or failed. */
uint32_t drm_atomic_damage_blob(struct nx_drm_backend *drm,
		const struct nx_buffer *buffer, const pixman_region32_t *damage) {
	pixman_region32_t clipped;
	pixman_region32_init(&clipped);
	pixman_region32_intersect_rect(&clipped, damage, 0, 0, buffer->width, buffer->height);

	int rects_len;
	const pixman_box32_t *rects = pixman_region32_rectangles(&clipped, &rects_len);

	uint32_t blob_id = 0;
	int ret = 0;
	if (rects_len > 0) {
		ret = drmModeCreatePropertyBlob(drm->fd, rects, sizeof(*rects) * rects_len, &blob_id);
	}
	pixman_region32_fini(&clipped);
	if (ret != 0) {
		nx_log_errno(NX_ERROR, "Failed to create FB_DAMAGE_CLIPS property blob");
	}
	return blob_id;
}

/* Outputs only advertise PQ. */
static uint8_t cta861_eotf(enum nx_color_transfer_function tf) {
	if (tf != NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ) {
		abort();
	}
	return CTA861_EOTF_ST2084;
}

static uint16_t cta861_color_coord(double v) {
	if (v < 0) {
		v = 0;
	}
	if (v > 1) {
		v = 1;
	}
	return (uint16_t)round(v * CTA861_COORD_SCALE);
}

static struct hdr_metadata_infoframe hdr_infoframe(
		const struct nx_output_image_description *img_desc) {
	const struct nx_color_primaries *primaries = &img_desc->mastering_display_primaries;
	return (struct hdr_metadata_infoframe){
		.eotf = cta861_eotf(img_desc->transfer_function),
		.metadata_type = 0,
		.display_primaries = {
			{ .x = cta861_color_coord(primaries->red.x), .y = cta861_color_coord(primaries->red.y) },
			{ .x = cta861_color_coord(primaries->green.x), .y = cta861_color_coord(primaries->green.y) },
			{ .x = cta861_color_coord(primaries->blue.x), .y = cta861_color_coord(primaries->blue.y) },
		},
		.white_point = {
			.x = cta861_color_coord(primaries->white.x),
			.y = cta861_color_coord(primaries->white.y),
		},
		.max_display_mastering_luminance = (uint16_t)img_desc->mastering_luminance.max,
		.min_display_mastering_luminance =
			(uint16_t)(img_desc->mastering_luminance.min * CTA861_MIN_LUMINANCE_SCALE),
		.max_cll = (uint16_t)img_desc->max_cll,
		.max_fall = (uint16_t)img_desc->max_fall,
	};
}

bool drm_atomic_hdr_metadata_blob(struct nx_drm_backend *drm,
		const struct nx_output_image_description *img_desc, uint32_t *blob_id) {
	if (img_desc == NULL) {
		*blob_id = 0;
		return true;
	}

	struct hdr_output_metadata metadata = {
		.metadata_type = 0,
		.hdmi_metadata_type1 = hdr_infoframe(img_desc),
	};
	if (drmModeCreatePropertyBlob(drm->fd, &metadata, sizeof(metadata), blob_id) != 0) {
		nx_log_errno(NX_ERROR, "Failed to create HDR_OUTPUT_METADATA property");
		return false;
	}
	return true;
}

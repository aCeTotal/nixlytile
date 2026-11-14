#include <drm_mode.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "platform/drm/connector/connector.h"
#include "platform/log/log.h"

int32_t drm_calculate_refresh_rate(const drmModeModeInfo *mode) {
	int32_t refresh = (mode->clock * 1000000LL / mode->htotal +
		mode->vtotal / 2) / mode->vtotal;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
		refresh *= 2;
	}

	if (mode->flags & DRM_MODE_FLAG_DBLSCAN) {
		refresh /= 2;
	}

	if (mode->vscan > 1) {
		refresh /= mode->vscan;
	}

	return refresh;
}

static enum nx_output_mode_aspect_ratio picture_aspect_ratio(const drmModeModeInfo *mode) {
	switch (mode->flags & DRM_MODE_FLAG_PIC_AR_MASK) {
	case DRM_MODE_FLAG_PIC_AR_NONE:
		return NX_OUTPUT_MODE_ASPECT_RATIO_NONE;
	case DRM_MODE_FLAG_PIC_AR_4_3:
		return NX_OUTPUT_MODE_ASPECT_RATIO_4_3;
	case DRM_MODE_FLAG_PIC_AR_16_9:
		return NX_OUTPUT_MODE_ASPECT_RATIO_16_9;
	case DRM_MODE_FLAG_PIC_AR_64_27:
		return NX_OUTPUT_MODE_ASPECT_RATIO_64_27;
	case DRM_MODE_FLAG_PIC_AR_256_135:
		return NX_OUTPUT_MODE_ASPECT_RATIO_256_135;
	default:
		nx_log(NX_ERROR, "Unknown mode picture aspect ratio: %u",
			mode->flags & DRM_MODE_FLAG_PIC_AR_MASK);
		return NX_OUTPUT_MODE_ASPECT_RATIO_NONE;
	}
}

struct nx_drm_mode *drm_mode_create(const drmModeModeInfo *modeinfo) {
	struct nx_drm_mode *mode = calloc(1, sizeof(*mode));
	if (!mode) {
		return NULL;
	}

	mode->drm_mode = *modeinfo;
	mode->nx_mode.width = mode->drm_mode.hdisplay;
	mode->nx_mode.height = mode->drm_mode.vdisplay;
	mode->nx_mode.refresh = drm_calculate_refresh_rate(modeinfo);
	mode->nx_mode.picture_aspect_ratio = picture_aspect_ratio(modeinfo);
	mode->nx_mode.preferred = modeinfo->type & DRM_MODE_TYPE_PREFERRED;

	return mode;
}

struct nx_output_mode *nx_drm_connector_add_mode(struct nx_output *output,
		const drmModeModeInfo *modeinfo) {
	struct nx_drm_connector *conn = drm_connector_from_output(output);

	if (modeinfo->type != DRM_MODE_TYPE_USERDEF) {
		return NULL;
	}

	struct nx_output_mode *nx_mode;
	wl_list_for_each(nx_mode, &conn->output.modes, link) {
		struct nx_drm_mode *mode = wl_container_of(nx_mode, mode, nx_mode);
		if (memcmp(&mode->drm_mode, modeinfo, sizeof(*modeinfo)) == 0) {
			return nx_mode;
		}
	}

	struct nx_drm_mode *mode = drm_mode_create(modeinfo);
	if (!mode) {
		return NULL;
	}

	wl_list_insert(&conn->output.modes, &mode->nx_mode.link);

	drm_conn_log(conn, NX_INFO, "Registered custom mode "
			"%"PRId32"x%"PRId32"@%"PRId32,
			mode->nx_mode.width, mode->nx_mode.height,
			mode->nx_mode.refresh);

	return &mode->nx_mode;
}

/* For EDID-clock-only panels. */
struct nx_output_mode *nx_drm_connector_add_paced_mode(
		struct nx_output *output, struct nx_output_mode *base,
		int divisor) {
	struct nx_drm_connector *conn = drm_connector_from_output(output);
	struct nx_drm_mode *base_mode = wl_container_of(base, base_mode, nx_mode);

	if (divisor < 2 || base_mode->pace_divisor != 0) {
		return NULL;
	}

	struct nx_output_mode *nx_mode;
	wl_list_for_each(nx_mode, &conn->output.modes, link) {
		struct nx_drm_mode *mode = wl_container_of(nx_mode, mode, nx_mode);
		if (mode->pace_divisor == divisor &&
				memcmp(&mode->drm_mode, &base_mode->drm_mode,
					sizeof(mode->drm_mode)) == 0) {
			return nx_mode;
		}
	}

	struct nx_drm_mode *mode = drm_mode_create(&base_mode->drm_mode);
	if (!mode) {
		return NULL;
	}
	mode->nx_mode.refresh = base->refresh / divisor;
	mode->nx_mode.preferred = false;
	mode->pace_divisor = divisor;

	wl_list_insert(&conn->output.modes, &mode->nx_mode.link);

	drm_conn_log(conn, NX_INFO, "Registered paced mode "
			"%"PRId32"x%"PRId32"@%"PRId32" (scanout @%"PRId32", 1/%d)",
			mode->nx_mode.width, mode->nx_mode.height,
			mode->nx_mode.refresh, base->refresh, divisor);

	return &mode->nx_mode;
}

int nx_drm_connector_mode_pace_divisor(struct nx_output *output,
		struct nx_output_mode *nx_mode) {
	if (output == NULL || nx_mode == NULL || !nx_output_is_drm(output)) {
		return 0;
	}
	const struct nx_drm_mode *mode = wl_container_of(nx_mode, mode, nx_mode);
	return mode->pace_divisor;
}

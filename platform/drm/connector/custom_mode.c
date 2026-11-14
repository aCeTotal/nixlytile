#include <drm_mode.h>
#include <libdisplay-info/cvt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "platform/drm/connector/connector.h"

#define DEFAULT_REFRESH_HZ 60

/* CVT blanking overshoots pixel clocks. */
bool drm_scaled_edid_mode(struct nx_output *output,
		drmModeModeInfo *out, int32_t width, int32_t height,
		int32_t refresh) {
	if (refresh <= 0) {
		return false;
	}

	struct nx_drm_mode *best = NULL;
	struct nx_output_mode *nx_mode;
	wl_list_for_each(nx_mode, &output->modes, link) {
		struct nx_drm_mode *m = wl_container_of(nx_mode, m, nx_mode);
		if (nx_mode->width != width || nx_mode->height != height ||
				(m->drm_mode.flags & DRM_MODE_FLAG_INTERLACE)) {
			continue;
		}
		if (!best || abs(nx_mode->refresh - refresh) <
				abs(best->nx_mode.refresh - refresh)) {
			best = m;
		}
	}
	if (!best) {
		return false;
	}

	*out = best->drm_mode;
	out->clock = (uint64_t)refresh * out->htotal * out->vtotal / 1000000;
	out->vrefresh = (refresh + 500) / 1000;
	snprintf(out->name, sizeof(out->name), "%dx%d@%d",
		width, height, out->vrefresh);
	return true;
}

void drm_generate_cvt_mode(drmModeModeInfo *mode, int hdisplay, int vdisplay,
		float vrefresh) {
	struct di_cvt_options options = {
		.red_blank_ver = DI_CVT_REDUCED_BLANKING_NONE,
		.h_pixels = hdisplay,
		.v_lines = vdisplay,
		.ip_freq_rqd = vrefresh ? vrefresh : DEFAULT_REFRESH_HZ,
	};
	struct di_cvt_timing timing;
	di_cvt_compute(&timing, &options);

	uint16_t hsync_start = (uint16_t)(hdisplay + timing.h_front_porch);
	uint16_t vsync_start = (uint16_t)(timing.v_lines_rnd + timing.v_front_porch);
	uint16_t hsync_end = (uint16_t)(hsync_start + timing.h_sync);
	uint16_t vsync_end = (uint16_t)(vsync_start + timing.v_sync);

	*mode = (drmModeModeInfo){
		.clock = (uint32_t)roundf((float)(timing.act_pixel_freq * 1000)),
		.hdisplay = hdisplay,
		.vdisplay = (uint16_t)timing.v_lines_rnd,
		.hsync_start = hsync_start,
		.vsync_start = vsync_start,
		.hsync_end = hsync_end,
		.vsync_end = vsync_end,
		.htotal = (uint16_t)(hsync_end + timing.h_back_porch),
		.vtotal = (uint16_t)(vsync_end + timing.v_back_porch),
		.vrefresh = (uint32_t)roundf((float)timing.act_frame_rate),
		.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC,
	};
	snprintf(mode->name, sizeof(mode->name), "%dx%d", hdisplay, vdisplay);
}

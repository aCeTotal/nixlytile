#ifndef PLATFORM_DRM_DEVICE_PROPERTIES_H
#define PLATFORM_DRM_DEVICE_PROPERTIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Property ids, zero when missing. */
struct nx_drm_connector_props {
	uint32_t edid;
	uint32_t dpms;
	uint32_t link_status;
	uint32_t vrr_capable;
	uint32_t subconnector;
	uint32_t non_desktop;
	uint32_t content_type;
	uint32_t max_bpc;
	uint32_t crtc_id;
	uint32_t colorspace;
	uint32_t hdr_output_metadata;
};

struct nx_drm_crtc_props {
	uint32_t vrr_enabled;
	uint32_t gamma_lut;
	uint32_t gamma_lut_size;
	uint32_t active;
	uint32_t mode_id;
};

struct nx_drm_plane_props {
	uint32_t type;
	uint32_t in_formats;
	uint32_t size_hints;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t crtc_x;
	uint32_t crtc_y;
	uint32_t crtc_w;
	uint32_t crtc_h;
	uint32_t fb_id;
	uint32_t crtc_id;
	uint32_t fb_damage_clips;
	uint32_t hotspot_x;
	uint32_t hotspot_y;
	uint32_t in_fence_fd;
};

bool drm_get_connector_props(int fd, uint32_t id,
	struct nx_drm_connector_props *out);
bool drm_get_crtc_props(int fd, uint32_t id, struct nx_drm_crtc_props *out);
bool drm_get_plane_props(int fd, uint32_t id, struct nx_drm_plane_props *out);

bool drm_get_prop(int fd, uint32_t obj, uint32_t prop, uint64_t *ret);
void *drm_get_prop_blob(int fd, uint32_t obj, uint32_t prop, size_t *ret_len);
char *drm_get_prop_enum(int fd, uint32_t obj, uint32_t prop);
bool drm_get_prop_range(int fd, uint32_t prop_id, uint64_t *min, uint64_t *max);

#endif

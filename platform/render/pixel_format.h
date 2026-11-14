#ifndef PLATFORM_RENDER_PIXEL_FORMAT_H
#define PLATFORM_RENDER_PIXEL_FORMAT_H

#include <wayland-server-protocol.h>

/* Block layout per DRM fourcc */
struct nx_pixel_format_info {
	uint32_t drm_format;
	uint32_t opaque_substitute; // alpha-less twin, or INVALID
	uint32_t bytes_per_block; // including padding
	uint32_t block_width, block_height; // zero means 1x1
};

/* NULL for unknown formats */
const struct nx_pixel_format_info *drm_get_pixel_format_info(uint32_t fmt);
uint32_t pixel_format_info_pixels_per_block(const struct nx_pixel_format_info *info);
int32_t pixel_format_info_min_stride(const struct nx_pixel_format_info *info, int32_t width);
bool pixel_format_info_check_stride(const struct nx_pixel_format_info *info,
	int32_t stride, int32_t width);

uint32_t convert_wl_shm_format_to_drm(enum wl_shm_format fmt);
enum wl_shm_format convert_drm_format_to_wl_shm(uint32_t fmt);

bool pixel_format_has_alpha(uint32_t fmt);
bool pixel_format_is_ycbcr(uint32_t fmt);

#endif

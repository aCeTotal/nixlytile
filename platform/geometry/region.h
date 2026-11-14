#ifndef PLATFORM_GEOMETRY_REGION_H
#define PLATFORM_GEOMETRY_REGION_H

#include <stdbool.h>
#include <pixman.h>
#include <wayland-server-protocol.h>

// Rounds outwards, never shrinking upscales.
void nx_region_scale(pixman_region32_t *dst, const pixman_region32_t *src,
	float scale);
void nx_region_scale_xy(pixman_region32_t *dst, const pixman_region32_t *src,
	float scale_x, float scale_y);
// Within a width x height box.
void nx_region_transform(pixman_region32_t *dst, const pixman_region32_t *src,
	enum wl_output_transform transform, int width, int height);
// Distance must be non-negative.
void nx_region_expand(pixman_region32_t *dst, const pixman_region32_t *src,
	int distance);

// False when (x1, y1) lies outside.
bool nx_region_confine(const pixman_region32_t *region, double x1, double y1, double x2,
	double y2, double *x2_out, double *y2_out);

#endif

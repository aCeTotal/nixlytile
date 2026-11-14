#ifndef PLATFORM_GEOMETRY_TRANSFORM_H
#define PLATFORM_GEOMETRY_TRANSFORM_H

#include <wayland-server-protocol.h>

enum wl_output_transform nx_output_transform_invert(
	enum wl_output_transform tr);
// Applies tr_a, then tr_b.
enum wl_output_transform nx_output_transform_compose(
	enum wl_output_transform tr_a, enum wl_output_transform tr_b);
void nx_output_transform_coords(enum wl_output_transform tr, int *x, int *y);

#endif

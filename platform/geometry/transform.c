#include "platform/geometry/transform.h"

enum wl_output_transform nx_output_transform_invert(
		enum wl_output_transform tr) {
	if ((tr & WL_OUTPUT_TRANSFORM_90) && !(tr & WL_OUTPUT_TRANSFORM_FLIPPED)) {
		tr ^= WL_OUTPUT_TRANSFORM_180;
	}
	return tr;
}

enum wl_output_transform nx_output_transform_compose(
		enum wl_output_transform tr_a, enum wl_output_transform tr_b) {
	uint32_t flipped = (tr_a ^ tr_b) & WL_OUTPUT_TRANSFORM_FLIPPED;
	uint32_t rotation_mask = WL_OUTPUT_TRANSFORM_90 | WL_OUTPUT_TRANSFORM_180;
	// A flip reverses the earlier rotation.
	uint32_t rotated = (tr_b & WL_OUTPUT_TRANSFORM_FLIPPED) ?
		(tr_b - tr_a) & rotation_mask : (tr_a + tr_b) & rotation_mask;
	return flipped | rotated;
}

void nx_output_transform_coords(enum wl_output_transform tr, int *x, int *y) {
	if (!(tr & WL_OUTPUT_TRANSFORM_90)) {
		return;
	}
	int tmp = *x;
	*x = *y;
	*y = tmp;
}

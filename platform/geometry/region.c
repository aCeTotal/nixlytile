#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "platform/geometry/region.h"

static void replace_rects(pixman_region32_t *dst, pixman_box32_t *rects,
		int nrects) {
	pixman_region32_fini(dst);
	pixman_region32_init_rects(dst, rects, nrects);
	free(rects);
}

void nx_region_scale(pixman_region32_t *dst, const pixman_region32_t *src,
		float scale) {
	nx_region_scale_xy(dst, src, scale, scale);
}

void nx_region_scale_xy(pixman_region32_t *dst, const pixman_region32_t *src,
		float scale_x, float scale_y) {
	if (scale_x == 1.0 && scale_y == 1.0) {
		pixman_region32_copy(dst, src);
		return;
	}

	int nrects;
	const pixman_box32_t *src_rects = pixman_region32_rectangles(src, &nrects);
	pixman_box32_t *dst_rects = malloc(nrects * sizeof(*dst_rects));
	if (dst_rects == NULL) {
		return;
	}

	for (int i = 0; i < nrects; ++i) {
		dst_rects[i].x1 = (int32_t)floor(src_rects[i].x1 * scale_x);
		dst_rects[i].x2 = (int32_t)ceil(src_rects[i].x2 * scale_x);
		dst_rects[i].y1 = (int32_t)floor(src_rects[i].y1 * scale_y);
		dst_rects[i].y2 = (int32_t)ceil(src_rects[i].y2 * scale_y);
	}
	replace_rects(dst, dst_rects, nrects);
}

static pixman_box32_t transform_rect(const pixman_box32_t *r,
		enum wl_output_transform transform, int width, int height) {
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_90:
		return (pixman_box32_t){ height - r->y2, r->x1, height - r->y1, r->x2 };
	case WL_OUTPUT_TRANSFORM_180:
		return (pixman_box32_t){ width - r->x2, height - r->y2,
			width - r->x1, height - r->y1 };
	case WL_OUTPUT_TRANSFORM_270:
		return (pixman_box32_t){ r->y1, width - r->x2, r->y2, width - r->x1 };
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		return (pixman_box32_t){ width - r->x2, r->y1, width - r->x1, r->y2 };
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		return (pixman_box32_t){ r->y1, r->x1, r->y2, r->x2 };
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		return (pixman_box32_t){ r->x1, height - r->y2, r->x2, height - r->y1 };
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		return (pixman_box32_t){ height - r->y2, width - r->x2,
			height - r->y1, width - r->x1 };
	case WL_OUTPUT_TRANSFORM_NORMAL:
		break;
	}
	return *r;
}

void nx_region_transform(pixman_region32_t *dst, const pixman_region32_t *src,
		enum wl_output_transform transform, int width, int height) {
	if (transform == WL_OUTPUT_TRANSFORM_NORMAL) {
		pixman_region32_copy(dst, src);
		return;
	}

	int nrects;
	const pixman_box32_t *src_rects = pixman_region32_rectangles(src, &nrects);
	pixman_box32_t *dst_rects = malloc(nrects * sizeof(*dst_rects));
	if (dst_rects == NULL) {
		return;
	}

	for (int i = 0; i < nrects; ++i) {
		dst_rects[i] = transform_rect(&src_rects[i], transform, width, height);
	}
	replace_rects(dst, dst_rects, nrects);
}

void nx_region_expand(pixman_region32_t *dst, const pixman_region32_t *src,
		int distance) {
	assert(distance >= 0);
	if (distance == 0) {
		pixman_region32_copy(dst, src);
		return;
	}

	int nrects;
	const pixman_box32_t *src_rects = pixman_region32_rectangles(src, &nrects);
	pixman_box32_t *dst_rects = malloc(nrects * sizeof(*dst_rects));
	if (dst_rects == NULL) {
		return;
	}

	for (int i = 0; i < nrects; ++i) {
		dst_rects[i] = (pixman_box32_t){
			src_rects[i].x1 - distance, src_rects[i].y1 - distance,
			src_rects[i].x2 + distance, src_rects[i].y2 + distance,
		};
	}
	replace_rects(dst, dst_rects, nrects);
}

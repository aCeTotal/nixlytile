#include <limits.h>
#include "platform/geometry/rect_union.h"

// Bounds the cost of large damage.
static const int FOLD_THRESHOLD = 256;

static void box_union(pixman_box32_t *dst, const pixman_box32_t *box) {
	dst->x1 = dst->x1 < box->x1 ? dst->x1 : box->x1;
	dst->y1 = dst->y1 < box->y1 ? dst->y1 : box->y1;
	dst->x2 = dst->x2 > box->x2 ? dst->x2 : box->x2;
	dst->y2 = dst->y2 > box->y2 ? dst->y2 : box->y2;
}

static bool box_empty_or_invalid(const pixman_box32_t *box) {
	return box->x1 >= box->x2 || box->y1 >= box->y2;
}

static int unsorted_len(const struct rect_union *ru) {
	return (int)(ru->unsorted.size / sizeof(pixman_box32_t));
}

static void release_unsorted(struct rect_union *ru) {
	wl_array_release(&ru->unsorted);
	wl_array_init(&ru->unsorted);
}

static void handle_alloc_failure(struct rect_union *ru) {
	ru->alloc_failure = true;
	release_unsorted(ru);
}

static bool fold_unsorted(struct rect_union *ru) {
	pixman_region32_t reg;
	if (!pixman_region32_init_rects(&reg, ru->unsorted.data, unsorted_len(ru))) {
		handle_alloc_failure(ru);
		return false;
	}
	bool ok = pixman_region32_union(&ru->region, &ru->region, &reg);
	pixman_region32_fini(&reg);
	if (!ok) {
		handle_alloc_failure(ru);
	}
	return ok;
}

void rect_union_init(struct rect_union *ru) {
	*ru = (struct rect_union){
		.bounding_box = {
			.x1 = INT_MAX,
			.x2 = INT_MIN,
			.y1 = INT_MAX,
			.y2 = INT_MIN,
		},
	};
	pixman_region32_init(&ru->region);
	wl_array_init(&ru->unsorted);
}

void rect_union_finish(struct rect_union *ru) {
	pixman_region32_fini(&ru->region);
	wl_array_release(&ru->unsorted);
}

void rect_union_add(struct rect_union *ru, const pixman_box32_t *box) {
	if (box_empty_or_invalid(box)) {
		return;
	}
	box_union(&ru->bounding_box, box);
	if (ru->alloc_failure) {
		return;
	}

	if (unsorted_len(ru) >= FOLD_THRESHOLD) {
		if (!fold_unsorted(ru)) {
			return;
		}
		// Keeps the allocation for reuse.
		ru->unsorted.size = 0;
	}

	pixman_box32_t *entry = wl_array_add(&ru->unsorted, sizeof(*entry));
	if (entry == NULL) {
		handle_alloc_failure(ru);
		return;
	}
	*entry = *box;
}

const pixman_region32_t *rect_union_evaluate(struct rect_union *ru) {
	if (!ru->alloc_failure && fold_unsorted(ru)) {
		release_unsorted(ru);
		return &ru->region;
	}

	pixman_region32_fini(&ru->region);
	if (box_empty_or_invalid(&ru->bounding_box)) {
		pixman_region32_init(&ru->region);
	} else {
		pixman_region32_init_with_extents(&ru->region, &ru->bounding_box);
	}
	return &ru->region;
}

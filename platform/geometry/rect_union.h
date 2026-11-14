#ifndef PLATFORM_GEOMETRY_RECT_UNION_H
#define PLATFORM_GEOMETRY_RECT_UNION_H

#include <stdlib.h>
#include <stdbool.h>
#include <pixman.h>
#include <wayland-util.h>

// Accumulates rects into a disjoint cover.
struct rect_union {
	pixman_box32_t bounding_box;
	// Valid after rect_union_evaluate().
	pixman_region32_t region;
	struct wl_array unsorted; // pixman_box32_t
	// Degrades evaluation to the bounding box.
	bool alloc_failure;
};

void rect_union_init(struct rect_union *r);
void rect_union_finish(struct rect_union *r);
// Ignores empty or inverted boxes.
void rect_union_add(struct rect_union *r, const pixman_box32_t *box);
// Valid until the next change.
const pixman_region32_t *rect_union_evaluate(struct rect_union *r);

#endif

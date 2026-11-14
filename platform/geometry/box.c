#include <math.h>
#include "platform/geometry/box.h"

// One wl_fixed step inside.
static const double EDGE_INSET = 1 / 256.0;

static int max(int a, int b) {
	return a > b ? a : b;
}

static int min(int a, int b) {
	return a < b ? a : b;
}

bool nx_box_empty(const struct nx_box *box) {
	return box == NULL || box->width <= 0 || box->height <= 0;
}

bool nx_fbox_empty(const struct nx_fbox *box) {
	return box == NULL || box->width <= 0 || box->height <= 0;
}

bool nx_box_equal(const struct nx_box *a, const struct nx_box *b) {
	bool a_empty = nx_box_empty(a);
	bool b_empty = nx_box_empty(b);
	if (a_empty || b_empty) {
		return a_empty == b_empty;
	}
	return a->x == b->x && a->y == b->y &&
		a->width == b->width && a->height == b->height;
}

bool nx_fbox_equal(const struct nx_fbox *a, const struct nx_fbox *b) {
	bool a_empty = nx_fbox_empty(a);
	bool b_empty = nx_fbox_empty(b);
	if (a_empty || b_empty) {
		return a_empty == b_empty;
	}
	return a->x == b->x && a->y == b->y &&
		a->width == b->width && a->height == b->height;
}

static double clamp_inside(double v, int start, int length) {
	double last = start + length - EDGE_INSET;
	if (v < start) {
		return start;
	}
	if (v > last) {
		return last;
	}
	return v;
}

void nx_box_closest_point(const struct nx_box *box, double x, double y,
		double *dest_x, double *dest_y) {
	if (nx_box_empty(box)) {
		*dest_x = NAN;
		*dest_y = NAN;
		return;
	}
	*dest_x = clamp_inside(x, box->x, box->width);
	*dest_y = clamp_inside(y, box->y, box->height);
}

bool nx_box_contains_point(const struct nx_box *box, double x, double y) {
	if (nx_box_empty(box)) {
		return false;
	}
	return x >= box->x && x < box->x + box->width &&
		y >= box->y && y < box->y + box->height;
}

bool nx_box_contains_box(const struct nx_box *bigger, const struct nx_box *smaller) {
	if (nx_box_empty(bigger) || nx_box_empty(smaller)) {
		return false;
	}
	return smaller->x >= bigger->x &&
		smaller->x + smaller->width <= bigger->x + bigger->width &&
		smaller->y >= bigger->y &&
		smaller->y + smaller->height <= bigger->y + bigger->height;
}

bool nx_box_intersects(const struct nx_box *a, const struct nx_box *b) {
	if (nx_box_empty(a) || nx_box_empty(b)) {
		return false;
	}
	return a->x < b->x + b->width && b->x < a->x + a->width &&
		a->y < b->y + b->height && b->y < a->y + a->height;
}

bool nx_box_intersection(struct nx_box *dest, const struct nx_box *box_a,
		const struct nx_box *box_b) {
	if (nx_box_empty(box_a) || nx_box_empty(box_b)) {
		*dest = (struct nx_box){0};
		return false;
	}

	int x1 = max(box_a->x, box_b->x);
	int y1 = max(box_a->y, box_b->y);
	int x2 = min(box_a->x + box_a->width, box_b->x + box_b->width);
	int y2 = min(box_a->y + box_a->height, box_b->y + box_b->height);
	*dest = (struct nx_box){ x1, y1, x2 - x1, y2 - y1 };

	if (nx_box_empty(dest)) {
		*dest = (struct nx_box){0};
		return false;
	}
	return true;
}

enum box_side { SIDE_LEFT, SIDE_TOP, SIDE_RIGHT, SIDE_BOTTOM };

// Source side becoming each origin.
static const struct { enum box_side x, y; } transform_origin[] = {
	[WL_OUTPUT_TRANSFORM_NORMAL] = { SIDE_LEFT, SIDE_TOP },
	[WL_OUTPUT_TRANSFORM_90] = { SIDE_BOTTOM, SIDE_LEFT },
	[WL_OUTPUT_TRANSFORM_180] = { SIDE_RIGHT, SIDE_BOTTOM },
	[WL_OUTPUT_TRANSFORM_270] = { SIDE_TOP, SIDE_RIGHT },
	[WL_OUTPUT_TRANSFORM_FLIPPED] = { SIDE_RIGHT, SIDE_TOP },
	[WL_OUTPUT_TRANSFORM_FLIPPED_90] = { SIDE_TOP, SIDE_LEFT },
	[WL_OUTPUT_TRANSFORM_FLIPPED_180] = { SIDE_LEFT, SIDE_BOTTOM },
	[WL_OUTPUT_TRANSFORM_FLIPPED_270] = { SIDE_BOTTOM, SIDE_RIGHT },
};

void nx_box_transform(struct nx_box *dest, const struct nx_box *box,
		enum wl_output_transform transform, int width, int height) {
	struct nx_box src = box ? *box : (struct nx_box){0};
	int side[] = {
		[SIDE_LEFT] = src.x,
		[SIDE_TOP] = src.y,
		[SIDE_RIGHT] = width - src.x - src.width,
		[SIDE_BOTTOM] = height - src.y - src.height,
	};
	bool rotated = transform % 2;
	*dest = (struct nx_box){
		.x = side[transform_origin[transform].x],
		.y = side[transform_origin[transform].y],
		.width = rotated ? src.height : src.width,
		.height = rotated ? src.width : src.height,
	};
}

void nx_fbox_transform(struct nx_fbox *dest, const struct nx_fbox *box,
		enum wl_output_transform transform, double width, double height) {
	struct nx_fbox src = box ? *box : (struct nx_fbox){0};
	double side[] = {
		[SIDE_LEFT] = src.x,
		[SIDE_TOP] = src.y,
		[SIDE_RIGHT] = width - src.x - src.width,
		[SIDE_BOTTOM] = height - src.y - src.height,
	};
	bool rotated = transform % 2;
	*dest = (struct nx_fbox){
		.x = side[transform_origin[transform].x],
		.y = side[transform_origin[transform].y],
		.width = rotated ? src.height : src.width,
		.height = rotated ? src.width : src.height,
	};
}

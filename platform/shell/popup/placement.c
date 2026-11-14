#include <stdlib.h>
#include "platform/geometry/edges.h"
#include "platform/shell/popup/positioner.h"

// Gravity shares the anchor values.
static const uint32_t anchor_edges[] = {
	[XDG_POSITIONER_ANCHOR_NONE] = NX_EDGE_NONE,
	[XDG_POSITIONER_ANCHOR_TOP] = NX_EDGE_TOP,
	[XDG_POSITIONER_ANCHOR_BOTTOM] = NX_EDGE_BOTTOM,
	[XDG_POSITIONER_ANCHOR_LEFT] = NX_EDGE_LEFT,
	[XDG_POSITIONER_ANCHOR_RIGHT] = NX_EDGE_RIGHT,
	[XDG_POSITIONER_ANCHOR_TOP_LEFT] = NX_EDGE_TOP | NX_EDGE_LEFT,
	[XDG_POSITIONER_ANCHOR_BOTTOM_LEFT] = NX_EDGE_BOTTOM | NX_EDGE_LEFT,
	[XDG_POSITIONER_ANCHOR_TOP_RIGHT] = NX_EDGE_TOP | NX_EDGE_RIGHT,
	[XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT] = NX_EDGE_BOTTOM | NX_EDGE_RIGHT,
};

static const uint32_t anchor_flip_x[] = {
	[XDG_POSITIONER_ANCHOR_NONE] = XDG_POSITIONER_ANCHOR_NONE,
	[XDG_POSITIONER_ANCHOR_TOP] = XDG_POSITIONER_ANCHOR_TOP,
	[XDG_POSITIONER_ANCHOR_BOTTOM] = XDG_POSITIONER_ANCHOR_BOTTOM,
	[XDG_POSITIONER_ANCHOR_LEFT] = XDG_POSITIONER_ANCHOR_RIGHT,
	[XDG_POSITIONER_ANCHOR_RIGHT] = XDG_POSITIONER_ANCHOR_LEFT,
	[XDG_POSITIONER_ANCHOR_TOP_LEFT] = XDG_POSITIONER_ANCHOR_TOP_RIGHT,
	[XDG_POSITIONER_ANCHOR_BOTTOM_LEFT] = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
	[XDG_POSITIONER_ANCHOR_TOP_RIGHT] = XDG_POSITIONER_ANCHOR_TOP_LEFT,
	[XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT] = XDG_POSITIONER_ANCHOR_BOTTOM_LEFT,
};

static const uint32_t anchor_flip_y[] = {
	[XDG_POSITIONER_ANCHOR_NONE] = XDG_POSITIONER_ANCHOR_NONE,
	[XDG_POSITIONER_ANCHOR_TOP] = XDG_POSITIONER_ANCHOR_BOTTOM,
	[XDG_POSITIONER_ANCHOR_BOTTOM] = XDG_POSITIONER_ANCHOR_TOP,
	[XDG_POSITIONER_ANCHOR_LEFT] = XDG_POSITIONER_ANCHOR_LEFT,
	[XDG_POSITIONER_ANCHOR_RIGHT] = XDG_POSITIONER_ANCHOR_RIGHT,
	[XDG_POSITIONER_ANCHOR_TOP_LEFT] = XDG_POSITIONER_ANCHOR_BOTTOM_LEFT,
	[XDG_POSITIONER_ANCHOR_BOTTOM_LEFT] = XDG_POSITIONER_ANCHOR_TOP_LEFT,
	[XDG_POSITIONER_ANCHOR_TOP_RIGHT] = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
	[XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT] = XDG_POSITIONER_ANCHOR_TOP_RIGHT,
};

void shell_positioner_rules_get_geometry(
		const struct nx_xdg_positioner_rules *rules, struct nx_box *box) {
	const struct nx_box *anchor = &rules->anchor_rect;
	box->x = rules->offset.x;
	box->y = rules->offset.y;
	box->width = rules->size.width;
	box->height = rules->size.height;

	uint32_t edges = anchor_edges[rules->anchor];
	if (edges & NX_EDGE_TOP) {
		box->y += anchor->y;
	} else if (edges & NX_EDGE_BOTTOM) {
		box->y += anchor->y + anchor->height;
	} else {
		box->y += anchor->y + anchor->height / 2;
	}
	if (edges & NX_EDGE_LEFT) {
		box->x += anchor->x;
	} else if (edges & NX_EDGE_RIGHT) {
		box->x += anchor->x + anchor->width;
	} else {
		box->x += anchor->x + anchor->width / 2;
	}

	edges = anchor_edges[rules->gravity];
	if (edges & NX_EDGE_TOP) {
		box->y -= box->height;
	} else if (~edges & NX_EDGE_BOTTOM) {
		box->y -= box->height / 2;
	}
	if (edges & NX_EDGE_LEFT) {
		box->x -= box->width;
	} else if (~edges & NX_EDGE_RIGHT) {
		box->x -= box->width / 2;
	}
}

// Positive where the box overflows.
struct constraint_offsets {
	int top;
	int bottom;
	int left;
	int right;
};

static bool is_unconstrained(const struct constraint_offsets *offsets) {
	return offsets->top <= 0 && offsets->bottom <= 0 &&
		offsets->left <= 0 && offsets->right <= 0;
}

static void get_constrained_box_offsets(const struct nx_box *constraint,
		const struct nx_box *box, struct constraint_offsets *offsets) {
	offsets->left = constraint->x - box->x;
	offsets->right = box->x + box->width - constraint->x - constraint->width;
	offsets->top = constraint->y - box->y;
	offsets->bottom = box->y + box->height - constraint->y - constraint->height;
}

static bool unconstrain_by_flip(const struct nx_xdg_positioner_rules *rules,
		const struct nx_box *constraint, struct nx_box *box,
		struct constraint_offsets *offsets) {
	// Flip only one-sided overflow.
	bool flip_x = ((offsets->left > 0) ^ (offsets->right > 0)) &&
		(rules->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X);
	bool flip_y = ((offsets->top > 0) ^ (offsets->bottom > 0)) &&
		(rules->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
	if (!flip_x && !flip_y) {
		return false;
	}

	struct nx_xdg_positioner_rules flipped = *rules;
	if (flip_x) {
		flipped.anchor = anchor_flip_x[flipped.anchor];
		flipped.gravity = anchor_flip_x[flipped.gravity];
	}
	if (flip_y) {
		flipped.anchor = anchor_flip_y[flipped.anchor];
		flipped.gravity = anchor_flip_y[flipped.gravity];
	}

	struct nx_box flipped_box;
	shell_positioner_rules_get_geometry(&flipped, &flipped_box);
	struct constraint_offsets flipped_offsets;
	get_constrained_box_offsets(constraint, &flipped_box, &flipped_offsets);

	// Keep flips that fit.
	if (flipped_offsets.left <= 0 && flipped_offsets.right <= 0) {
		box->x = flipped_box.x;
		offsets->left = flipped_offsets.left;
		offsets->right = flipped_offsets.right;
	}
	if (flipped_offsets.top <= 0 && flipped_offsets.bottom <= 0) {
		box->y = flipped_box.y;
		offsets->top = flipped_offsets.top;
		offsets->bottom = flipped_offsets.bottom;
	}

	return is_unconstrained(offsets);
}

static int slide_distance(int low, int high, bool toward_low) {
	// Both edges out: follow gravity.
	if (low > 0 && high > 0) {
		return toward_low ? -high : low;
	}
	// Shortest slide fixing one edge.
	return abs(low) < abs(high) ? low : -high;
}

static bool unconstrain_by_slide(const struct nx_xdg_positioner_rules *rules,
		const struct nx_box *constraint, struct nx_box *box,
		struct constraint_offsets *offsets) {
	uint32_t gravity = anchor_edges[rules->gravity];

	bool slide_x = (offsets->left > 0 || offsets->right > 0) &&
		(rules->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X);
	bool slide_y = (offsets->top > 0 || offsets->bottom > 0) &&
		(rules->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
	if (!slide_x && !slide_y) {
		return false;
	}

	if (slide_x) {
		box->x += slide_distance(offsets->left, offsets->right,
			gravity & NX_EDGE_LEFT);
	}
	if (slide_y) {
		box->y += slide_distance(offsets->top, offsets->bottom,
			gravity & NX_EDGE_TOP);
	}

	get_constrained_box_offsets(constraint, box, offsets);
	return is_unconstrained(offsets);
}

static void unconstrain_by_resize(const struct nx_xdg_positioner_rules *rules,
		struct nx_box *box, struct constraint_offsets *offsets) {
	bool resize_x = (offsets->left > 0 || offsets->right > 0) &&
		(rules->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X);
	bool resize_y = (offsets->top > 0 || offsets->bottom > 0) &&
		(rules->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y);
	if (!resize_x && !resize_y) {
		return;
	}

	if (offsets->left < 0) {
		offsets->left = 0;
	}
	if (offsets->right < 0) {
		offsets->right = 0;
	}
	if (offsets->top < 0) {
		offsets->top = 0;
	}
	if (offsets->bottom < 0) {
		offsets->bottom = 0;
	}

	struct nx_box resized_box = *box;
	if (resize_x) {
		resized_box.x += offsets->left;
		resized_box.width -= offsets->left + offsets->right;
	}
	if (resize_y) {
		resized_box.y += offsets->top;
		resized_box.height -= offsets->top + offsets->bottom;
	}

	if (!nx_box_empty(&resized_box)) {
		*box = resized_box;
	}
}

void shell_positioner_rules_unconstrain_box(
		const struct nx_xdg_positioner_rules *rules,
		const struct nx_box *constraint, struct nx_box *box) {
	struct constraint_offsets offsets;
	get_constrained_box_offsets(constraint, box, &offsets);
	if (is_unconstrained(&offsets)) {
		return;
	}
	if (unconstrain_by_flip(rules, constraint, box, &offsets) ||
			unconstrain_by_slide(rules, constraint, box, &offsets)) {
		return;
	}
	unconstrain_by_resize(rules, box, &offsets);
}

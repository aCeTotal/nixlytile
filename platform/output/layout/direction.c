#include <assert.h>
#include <float.h>
#include "platform/geometry/box.h"
#include "platform/output/layout/output_box.h"

enum distance_selection_method {
	NEAREST,
	FARTHEST
};

static bool box_in_direction(const struct nx_box *box,
		const struct nx_box *ref_box, enum nx_direction direction) {
	bool match = false;
	if (direction & NX_DIRECTION_LEFT) {
		match = box->x + box->width <= ref_box->x || match;
	}
	if (direction & NX_DIRECTION_RIGHT) {
		match = box->x >= ref_box->x + ref_box->width || match;
	}
	if (direction & NX_DIRECTION_UP) {
		match = box->y + box->height <= ref_box->y || match;
	}
	if (direction & NX_DIRECTION_DOWN) {
		match = box->y >= ref_box->y + ref_box->height || match;
	}
	return match;
}

static struct nx_output *output_in_direction(struct nx_output_layout *layout,
		enum nx_direction direction, struct nx_output *reference,
		double ref_lx, double ref_ly, enum distance_selection_method method) {
	assert(reference);

	struct nx_box ref_box;
	nx_output_layout_get_box(layout, reference, &ref_box);
	// Not part of the layout.
	if (nx_box_empty(&ref_box)) {
		return NULL;
	}

	double best_distance = method == NEAREST ? DBL_MAX : DBL_MIN;
	struct nx_output *best_output = NULL;
	struct nx_output_layout_output *l_output;
	wl_list_for_each(l_output, &layout->outputs, link) {
		if (l_output->output == reference) {
			continue;
		}
		struct nx_box box;
		output_layout_output_get_box(l_output, &box);
		if (!box_in_direction(&box, &ref_box, direction)) {
			continue;
		}

		double x, y;
		nx_output_layout_closest_point(layout, l_output->output,
			ref_lx, ref_ly, &x, &y);
		double distance =
			(x - ref_lx) * (x - ref_lx) + (y - ref_ly) * (y - ref_ly);
		bool better = method == NEAREST ?
			distance < best_distance : distance > best_distance;
		if (better) {
			best_distance = distance;
			best_output = l_output->output;
		}
	}
	return best_output;
}

struct nx_output *nx_output_layout_adjacent_output(
		struct nx_output_layout *layout, enum nx_direction direction,
		struct nx_output *reference, double ref_lx, double ref_ly) {
	return output_in_direction(layout, direction, reference,
		ref_lx, ref_ly, NEAREST);
}

struct nx_output *nx_output_layout_farthest_output(
		struct nx_output_layout *layout, enum nx_direction direction,
		struct nx_output *reference, double ref_lx, double ref_ly) {
	return output_in_direction(layout, direction, reference,
		ref_lx, ref_ly, FARTHEST);
}

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include "platform/geometry/box.h"
#include "platform/output/layout/output_box.h"

void output_layout_output_get_box(struct nx_output_layout_output *l_output,
		struct nx_box *box) {
	box->x = l_output->x;
	box->y = l_output->y;
	nx_output_effective_resolution(l_output->output,
		&box->width, &box->height);
}

bool nx_output_layout_contains_point(struct nx_output_layout *layout,
		struct nx_output *reference, int lx, int ly) {
	return nx_output_layout_output_at(layout, lx, ly) != NULL;
}

struct nx_output *nx_output_layout_output_at(struct nx_output_layout *layout,
		double lx, double ly) {
	struct nx_output_layout_output *l_output;
	wl_list_for_each(l_output, &layout->outputs, link) {
		struct nx_box output_box;
		output_layout_output_get_box(l_output, &output_box);
		if (nx_box_contains_point(&output_box, lx, ly)) {
			return l_output->output;
		}
	}
	return NULL;
}

void nx_output_layout_output_coords(struct nx_output_layout *layout,
		struct nx_output *reference, double *lx, double *ly) {
	assert(layout && reference);

	struct nx_output_layout_output *l_output;
	wl_list_for_each(l_output, &layout->outputs, link) {
		if (l_output->output == reference) {
			*lx -= (double)l_output->x;
			*ly -= (double)l_output->y;
			return;
		}
	}
}

void nx_output_layout_closest_point(struct nx_output_layout *layout,
		struct nx_output *reference, double lx, double ly,
		double *dest_lx, double *dest_ly) {
	double min_x = lx, min_y = ly, min_distance = DBL_MAX;
	struct nx_output_layout_output *l_output;
	wl_list_for_each(l_output, &layout->outputs, link) {
		if (reference != NULL && reference != l_output->output) {
			continue;
		}

		struct nx_box output_box;
		output_layout_output_get_box(l_output, &output_box);
		double output_x, output_y;
		nx_box_closest_point(&output_box, lx, ly, &output_x, &output_y);

		// Squared distance suffices for comparison.
		double distance =
			(lx - output_x) * (lx - output_x) + (ly - output_y) * (ly - output_y);
		if (!isfinite(distance)) {
			distance = DBL_MAX;
		}
		if (distance < min_distance) {
			min_x = output_x;
			min_y = output_y;
			min_distance = distance;
		}
	}

	*dest_lx = min_x;
	*dest_ly = min_y;
}

static void get_layout_extents(struct nx_output_layout *layout,
		struct nx_box *box) {
	if (wl_list_empty(&layout->outputs)) {
		*box = (struct nx_box){0};
		return;
	}

	int min_x = INT_MAX, min_y = INT_MAX;
	int max_x = INT_MIN, max_y = INT_MIN;
	struct nx_output_layout_output *l_output;
	wl_list_for_each(l_output, &layout->outputs, link) {
		struct nx_box output_box;
		output_layout_output_get_box(l_output, &output_box);
		if (output_box.x < min_x) {
			min_x = output_box.x;
		}
		if (output_box.y < min_y) {
			min_y = output_box.y;
		}
		if (output_box.x + output_box.width > max_x) {
			max_x = output_box.x + output_box.width;
		}
		if (output_box.y + output_box.height > max_y) {
			max_y = output_box.y + output_box.height;
		}
	}

	box->x = min_x;
	box->y = min_y;
	box->width = max_x - min_x;
	box->height = max_y - min_y;
}

void nx_output_layout_get_box(struct nx_output_layout *layout,
		struct nx_output *reference, struct nx_box *dest_box) {
	if (reference == NULL) {
		get_layout_extents(layout, dest_box);
		return;
	}

	*dest_box = (struct nx_box){0};

	struct nx_output_layout_output *l_output = nx_output_layout_get(layout, reference);
	if (l_output != NULL) {
		output_layout_output_get_box(l_output, dest_box);
	}
}

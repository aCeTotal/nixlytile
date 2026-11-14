#include <math.h>
#include "platform/geometry/region.h"

static int sign(double v) {
	if (v == 0) {
		return 0;
	}
	return v > 0 ? 1 : -1;
}

static void region_confine(const pixman_region32_t *region, double x1, double y1, double x2,
		double y2, double *x2_out, double *y2_out, pixman_box32_t box) {
	double x_clamped = fmax(fmin(x2, box.x2 - 1), box.x1);
	double y_clamped = fmax(fmin(y2, box.y2 - 1), box.y1);

	// Sub-pixel overshoot stays inside.
	if (floor(x_clamped) == floor(x2) && floor(y_clamped) == floor(y2)) {
		*x2_out = x2;
		*y2_out = y2;
		return;
	}

	double dx = x2 - x1;
	double dy = y2 - y1;

	// fabs avoids negative zero infinities.
	double delta = fmin(fabs(x_clamped - x1) / fabs(dx), fabs(y_clamped - y1) / fabs(dy));

	// Clamped again against precision errors.
	double x = fmax(fmin(delta * dx + x1, box.x2 - 1), box.x1);
	double y = fmax(fmin(delta * dy + y1, box.y2 - 1), box.y1);

	// Probe the adjacent box.
	int x_ext = (int)floor(x) + sign(dx);
	int y_ext = (int)floor(y) + sign(dy);
	if (pixman_region32_contains_point(region, x_ext, y_ext, &box)) {
		region_confine(region, x, y, x2, y2, x2_out, y2_out, box);
		return;
	}

	if (dx == 0 || dy == 0) {
		*x2_out = x;
		*y2_out = y;
		return;
	}

	bool bordering_x = x == box.x1 || x == box.x2 - 1;
	bool bordering_y = y == box.y1 || y == box.y2 - 1;
	if (bordering_x != bordering_y) {
		region_confine(region, x, y, bordering_x ? x : x2, bordering_x ? y2 : y,
			x2_out, y2_out, box);
		return;
	}

	// Slide along the longer axis.
	double x2_potential, y2_potential, unused;
	region_confine(region, x, y, x, y2, &unused, &y2_potential, box);
	region_confine(region, x, y, x2, y, &x2_potential, &unused, box);
	if (fabs(x2_potential - x) > fabs(y2_potential - y)) {
		*x2_out = x2_potential;
		*y2_out = y;
	} else {
		*x2_out = x;
		*y2_out = y2_potential;
	}
}

bool nx_region_confine(const pixman_region32_t *region, double x1, double y1, double x2,
		double y2, double *x2_out, double *y2_out) {
	pixman_box32_t box;
	if (!pixman_region32_contains_point(region, (int)floor(x1), (int)floor(y1), &box)) {
		return false;
	}
	region_confine(region, x1, y1, x2, y2, x2_out, y2_out, box);
	return true;
}

#ifndef PLATFORM_GEOMETRY_BOX_H
#define PLATFORM_GEOMETRY_BOX_H

#include <stdbool.h>
#include <wayland-server-protocol.h>

// Far edges are exclusive.
struct nx_box {
	int x, y;
	int width, height;
};

struct nx_fbox {
	double x, y;
	double width, height;
};

// A NULL box counts as empty.
bool nx_box_empty(const struct nx_box *box);
bool nx_fbox_empty(const struct nx_fbox *box);
bool nx_box_equal(const struct nx_box *a, const struct nx_box *b);
bool nx_fbox_equal(const struct nx_fbox *a, const struct nx_fbox *b);

// NAN for an empty box.
void nx_box_closest_point(const struct nx_box *box, double x, double y,
	double *dest_x, double *dest_y);
bool nx_box_contains_point(const struct nx_box *box, double x, double y);
bool nx_box_contains_box(const struct nx_box *bigger, const struct nx_box *smaller);
bool nx_box_intersects(const struct nx_box *a, const struct nx_box *b);
// Empty dest when disjoint.
bool nx_box_intersection(struct nx_box *dest, const struct nx_box *box_a,
	const struct nx_box *box_b);

// Within a (0, 0, width, height) box.
void nx_box_transform(struct nx_box *dest, const struct nx_box *box,
	enum wl_output_transform transform, int width, int height);
void nx_fbox_transform(struct nx_fbox *dest, const struct nx_fbox *box,
	enum wl_output_transform transform, double width, double height);

#endif

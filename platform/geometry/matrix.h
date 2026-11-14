#ifndef PLATFORM_GEOMETRY_MATRIX_H
#define PLATFORM_GEOMETRY_MATRIX_H

#include <wayland-server-protocol.h>

struct nx_box;

void nx_matrix_identity(float mat[static 9]);
// mat = a * b
void nx_matrix_multiply(float mat[static 9], const float a[static 9],
	const float b[static 9]);
// Maps the box into clip space.
void nx_matrix_project_box(float mat[static 9], const struct nx_box *box,
	enum wl_output_transform transform, const float projection[static 9]);
// Like glOrtho(0, width, 0, height, 1, -1).
void matrix_projection(float mat[static 9], int width, int height,
	enum wl_output_transform transform);
// The matrix must be invertible.
void matrix_invert(float out[static 9], float m[static 9]);

#endif

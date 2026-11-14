#include <assert.h>
#include <math.h>
#include <string.h>
#include "platform/geometry/box.h"
#include "platform/geometry/matrix.h"

static const float transforms[][9] = {
	[WL_OUTPUT_TRANSFORM_NORMAL] = {
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_90] = {
		0.0f, 1.0f, 0.0f,
		-1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_180] = {
		-1.0f, 0.0f, 0.0f,
		0.0f, -1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_270] = {
		0.0f, -1.0f, 0.0f,
		1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED] = {
		-1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_90] = {
		0.0f, 1.0f, 0.0f,
		1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_180] = {
		1.0f, 0.0f, 0.0f,
		0.0f, -1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_270] = {
		0.0f, -1.0f, 0.0f,
		-1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	},
};

void nx_matrix_identity(float mat[static 9]) {
	memcpy(mat, transforms[WL_OUTPUT_TRANSFORM_NORMAL], sizeof(transforms[0]));
}

void nx_matrix_multiply(float mat[static 9], const float a[static 9],
		const float b[static 9]) {
	float product[9];

	product[0] = a[0]*b[0] + a[1]*b[3] + a[2]*b[6];
	product[1] = a[0]*b[1] + a[1]*b[4] + a[2]*b[7];
	product[2] = a[0]*b[2] + a[1]*b[5] + a[2]*b[8];

	product[3] = a[3]*b[0] + a[4]*b[3] + a[5]*b[6];
	product[4] = a[3]*b[1] + a[4]*b[4] + a[5]*b[7];
	product[5] = a[3]*b[2] + a[4]*b[5] + a[5]*b[8];

	product[6] = a[6]*b[0] + a[7]*b[3] + a[8]*b[6];
	product[7] = a[6]*b[1] + a[7]*b[4] + a[8]*b[7];
	product[8] = a[6]*b[2] + a[7]*b[5] + a[8]*b[8];

	memcpy(mat, product, sizeof(product));
}

static void matrix_translate(float mat[static 9], float x, float y) {
	const float translate[9] = {
		1.0f, 0.0f, x,
		0.0f, 1.0f, y,
		0.0f, 0.0f, 1.0f,
	};
	nx_matrix_multiply(mat, mat, translate);
}

static void matrix_scale(float mat[static 9], float x, float y) {
	const float scale[9] = {
		x, 0.0f, 0.0f,
		0.0f, y, 0.0f,
		0.0f, 0.0f, 1.0f,
	};
	nx_matrix_multiply(mat, mat, scale);
}

void matrix_projection(float mat[static 9], int width, int height,
		enum wl_output_transform transform) {
	memset(mat, 0, sizeof(*mat) * 9);

	const float *t = transforms[transform];
	float x = 2.0f / width;
	float y = 2.0f / height;

	mat[0] = x * t[0];
	mat[1] = x * t[1];
	mat[3] = y * -t[3];
	mat[4] = y * -t[4];

	mat[2] = -copysignf(1.0f, mat[0] + mat[1]);
	mat[5] = -copysignf(1.0f, mat[3] + mat[4]);

	mat[8] = 1.0f;
}

void nx_matrix_project_box(float mat[static 9], const struct nx_box *box,
		enum wl_output_transform transform, const float projection[static 9]) {
	nx_matrix_identity(mat);
	matrix_translate(mat, box->x, box->y);
	matrix_scale(mat, box->width, box->height);

	if (transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		matrix_translate(mat, 0.5, 0.5);
		nx_matrix_multiply(mat, mat, transforms[transform]);
		matrix_translate(mat, -0.5, -0.5);
	}

	nx_matrix_multiply(mat, projection, mat);
}

void matrix_invert(float out[static 9], float m[static 9]) {
	float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];

	float det = a*e*i + b*f*g + c*d*h - c*e*g - b*d*i - a*f*h;
	assert(det != 0);
	float inv_det = 1 / det;

	// Adjugate over the determinant.
	float result[] = {
		inv_det * (e*i - f*h),
		inv_det * -(b*i - c*h),
		inv_det * (b*f - c*e),
		inv_det * -(d*i - f*g),
		inv_det * (a*i - c*g),
		inv_det * -(a*f - c*d),
		inv_det * (d*h - e*g),
		inv_det * -(a*h - b*g),
		inv_det * (a*e - b*d),
	};
	memcpy(out, result, sizeof(result));
}

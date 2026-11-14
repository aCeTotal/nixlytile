#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "platform/geometry/matrix.h"
#include "platform/render/color.h"

static const float INV_GAMMA_22 = (float)(1.0 / 2.2);
static const float INV_GAMMA_24 = (float)(1.0 / 2.4);

/* H.273 ColourPrimaries code point 1 */
static const struct nx_color_primaries COLOR_PRIMARIES_SRGB = {
	.red = { 0.640f, 0.330f },
	.green = { 0.300f, 0.600f },
	.blue = { 0.150f, 0.060f },
	.white = { 0.3127f, 0.3290f },
};

/* H.273 ColourPrimaries code point 9 */
static const struct nx_color_primaries COLOR_PRIMARIES_BT2020 = {
	.red = { 0.708f, 0.292f },
	.green = { 0.170f, 0.797f },
	.blue = { 0.131f, 0.046f },
	.white = { 0.3127f, 0.3290f },
};

static float srgb_eval_inverse_eotf(float x) {
	if (x <= 0.0031308) {
		return (float)(12.92 * x);
	}
	return (float)(1.055 * powf(x, INV_GAMMA_24) - 0.055);
}

/* H.273 TransferCharacteristics code point 16 */
static float st2084_pq_eval_inverse_eotf(float x) {
	const float c1 = 0.8359375f;
	const float c2 = 18.8515625f;
	const float c3 = 18.6875f;
	const float m = 78.84375f;
	const float n = 0.1593017578125f;
	if (x < 0) {
		x = 0;
	}
	if (x > 1) {
		x = 1;
	}
	float pow_n = powf(x, n);
	return powf((c1 + c2 * pow_n) / (1 + c3 * pow_n), m);
}

static float bt1886_eval_inverse_eotf(float x) {
	float lb = powf(0.0001f, INV_GAMMA_24);
	float lw = powf(1.0f, INV_GAMMA_24);
	float a = powf(lw - lb, 2.4f);
	float b = lb / (lw - lb);
	return powf(x / a, INV_GAMMA_24) - b;
}

static float transfer_function_eval_inverse_eotf(
		enum nx_color_transfer_function tf, float x) {
	switch (tf) {
	case NX_COLOR_TRANSFER_FUNCTION_SRGB:
		return srgb_eval_inverse_eotf(x);
	case NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		return st2084_pq_eval_inverse_eotf(x);
	case NX_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		return x;
	case NX_COLOR_TRANSFER_FUNCTION_GAMMA22:
		return powf(x, INV_GAMMA_22);
	case NX_COLOR_TRANSFER_FUNCTION_BT1886:
		return bt1886_eval_inverse_eotf(x);
	}
	abort();
}

static void color_transform_inverse_eotf_eval(
		struct nx_color_transform_inverse_eotf *tr,
		float out[static 3], const float in[static 3]) {
	for (size_t i = 0; i < 3; i++) {
		out[i] = transfer_function_eval_inverse_eotf(tr->tf, in[i]);
	}
}

static float lut_1d_get(const uint16_t *lut, size_t len, size_t i) {
	if (i >= len) {
		i = len - 1;
	}
	return (float) lut[i] / UINT16_MAX;
}

static float lut_1d_eval(const uint16_t *lut, size_t len, float x) {
	double pos = x * (len - 1);
	double int_part;
	double frac_part = modf(pos, &int_part);
	size_t i = (size_t) int_part;
	double a = lut_1d_get(lut, len, i);
	double b = lut_1d_get(lut, len, i + 1);
	return (float)(a * (1 - frac_part) + b * frac_part);
}

static void color_transform_lut_3x1d_eval(struct nx_color_transform_lut_3x1d *tr,
		float out[static 3], const float in[static 3]) {
	for (size_t i = 0; i < 3; i++) {
		out[i] = lut_1d_eval(&tr->lut_3x1d[tr->dim * i], tr->dim, in[i]);
	}
}

static void multiply_matrix_vector(float out[static 3], const float m[static 9],
		const float v[static 3]) {
	float result[3] = {
		m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
		m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
		m[6] * v[0] + m[7] * v[1] + m[8] * v[2],
	};
	memcpy(out, result, sizeof(result));
}

static void matrix_eval(struct nx_color_transform *tr,
		float out[static 3], const float in[static 3]) {
	struct nx_color_transform_matrix *matrix = wl_container_of(tr, matrix, base);
	multiply_matrix_vector(out, matrix->matrix, in);
}

static void pipeline_eval(struct nx_color_transform *tr,
		float out[static 3], const float in[static 3]) {
	struct nx_color_transform_pipeline *pipeline = wl_container_of(tr, pipeline, base);
	float color[3];
	memcpy(color, in, sizeof(color));
	for (size_t i = 0; i < pipeline->len; i++) {
		nx_color_transform_eval(pipeline->transforms[i], color, color);
	}
	memcpy(out, color, sizeof(color));
}

void nx_color_transform_eval(struct nx_color_transform *tr,
		float out[static 3], const float in[static 3]) {
	switch (tr->type) {
	case COLOR_TRANSFORM_INVERSE_EOTF:
		color_transform_inverse_eotf_eval(nx_color_transform_inverse_eotf_from_base(tr), out, in);
		break;
	case COLOR_TRANSFORM_LUT_3X1D:
		color_transform_lut_3x1d_eval(color_transform_lut_3x1d_from_base(tr), out, in);
		break;
	case COLOR_TRANSFORM_MATRIX:
		matrix_eval(tr, out, in);
		break;
	case COLOR_TRANSFORM_PIPELINE:
		pipeline_eval(tr, out, in);
		break;
	}
}

void nx_color_primaries_from_named(struct nx_color_primaries *out,
		enum nx_color_named_primaries named) {
	switch (named) {
	case NX_COLOR_NAMED_PRIMARIES_SRGB:
		*out = COLOR_PRIMARIES_SRGB;
		return;
	case NX_COLOR_NAMED_PRIMARIES_BT2020:
		*out = COLOR_PRIMARIES_BT2020;
		return;
	}
	abort();
}

static void xy_to_xyz(float out[static 3], struct nx_color_cie1931_xy src) {
	if (src.y == 0) {
		out[0] = out[1] = out[2] = 0;
		return;
	}

	out[0] = src.x / src.y;
	out[1] = 1;
	out[2] = (1 - src.x - src.y) / src.y;
}

/* RGB to XYZ, Lindbloom's method */
static void primaries_to_xyz(const struct nx_color_primaries *primaries, float matrix[static 9]) {
	float r[3], g[3], b[3], w[3];
	xy_to_xyz(r, primaries->red);
	xy_to_xyz(g, primaries->green);
	xy_to_xyz(b, primaries->blue);
	xy_to_xyz(w, primaries->white);

	float xyz_matrix[9] = {
		r[0], g[0], b[0],
		r[1], g[1], b[1],
		r[2], g[2], b[2],
	};
	matrix_invert(xyz_matrix, xyz_matrix);

	float s[3];
	multiply_matrix_vector(s, xyz_matrix, w);

	float result[] = {
		s[0] * r[0], s[1] * g[0], s[2] * b[0],
		s[0] * r[1], s[1] * g[1], s[2] * b[1],
		s[0] * r[2], s[1] * g[2], s[2] * b[2],
	};
	memcpy(matrix, result, sizeof(result));
}

void nx_color_primaries_transform_absolute_colorimetric(
		const struct nx_color_primaries *source,
		const struct nx_color_primaries *destination, float matrix[static 9]) {
	float source_to_xyz[9];
	primaries_to_xyz(source, source_to_xyz);
	float destination_to_xyz[9];
	primaries_to_xyz(destination, destination_to_xyz);
	float xyz_to_destination[9];
	matrix_invert(xyz_to_destination, destination_to_xyz);
	nx_matrix_multiply(matrix, xyz_to_destination, source_to_xyz);
}

void nx_color_transfer_function_get_default_luminance(enum nx_color_transfer_function tf,
		struct nx_color_luminances *lum) {
	switch (tf) {
	case NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		*lum = (struct nx_color_luminances){
			.min = 0.005f,
			.max = 10000,
			.reference = 203,
		};
		break;
	case NX_COLOR_TRANSFER_FUNCTION_BT1886:
		*lum = (struct nx_color_luminances){
			.min = 0.01f,
			.max = 100,
			.reference = 100,
		};
		break;
	default:
		*lum = (struct nx_color_luminances){
			.min = 0.2f,
			.max = 80,
			.reference = 80,
		};
		break;
	}
}

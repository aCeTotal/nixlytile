#ifndef PLATFORM_RENDER_COLOR_H
#define PLATFORM_RENDER_COLOR_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "platform/container/addon.h"

enum nx_color_named_primaries {
	NX_COLOR_NAMED_PRIMARIES_SRGB = 1 << 0,
	NX_COLOR_NAMED_PRIMARIES_BT2020 = 1 << 1,
};

enum nx_color_transfer_function {
	NX_COLOR_TRANSFER_FUNCTION_SRGB = 1 << 0,
	NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ = 1 << 1,
	NX_COLOR_TRANSFER_FUNCTION_EXT_LINEAR = 1 << 2,
	NX_COLOR_TRANSFER_FUNCTION_GAMMA22 = 1 << 3,
	NX_COLOR_TRANSFER_FUNCTION_BT1886 = 1 << 4,
};

/* Default is premultiplied electrical */
enum nx_alpha_mode {
	NX_COLOR_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL,
	NX_COLOR_ALPHA_MODE_PREMULTIPLIED_OPTICAL,
	NX_COLOR_ALPHA_MODE_STRAIGHT,
};

/* YCbCr to RGB matrix coefficients */
enum nx_color_encoding {
	NX_COLOR_ENCODING_NONE = 0,
	NX_COLOR_ENCODING_IDENTITY = 1 << 0,
	NX_COLOR_ENCODING_BT709 = 1 << 1,
	NX_COLOR_ENCODING_FCC = 1 << 2,
	NX_COLOR_ENCODING_BT601 = 1 << 3,
	NX_COLOR_ENCODING_SMPTE240 = 1 << 4,
	NX_COLOR_ENCODING_BT2020 = 1 << 5,
	NX_COLOR_ENCODING_BT2020_CL = 1 << 6,
	NX_COLOR_ENCODING_ICTCP = 1 << 7,
};

enum nx_color_range {
	NX_COLOR_RANGE_NONE,
	NX_COLOR_RANGE_LIMITED,
	NX_COLOR_RANGE_FULL,
};

/* H.273 Chroma420SampleLocType */
enum nx_color_chroma_location {
	NX_COLOR_CHROMA_LOCATION_NONE,
	NX_COLOR_CHROMA_LOCATION_TYPE0,
	NX_COLOR_CHROMA_LOCATION_TYPE1,
	NX_COLOR_CHROMA_LOCATION_TYPE2,
	NX_COLOR_CHROMA_LOCATION_TYPE3,
	NX_COLOR_CHROMA_LOCATION_TYPE4,
	NX_COLOR_CHROMA_LOCATION_TYPE5,
};

struct nx_color_cie1931_xy {
	float x, y;
};

struct nx_color_primaries {
	struct nx_color_cie1931_xy red, green, blue, white;
};

/* In cd/m² */
struct nx_color_luminances {
	float min, max, reference;
};

enum nx_color_transform_type {
	COLOR_TRANSFORM_INVERSE_EOTF,
	COLOR_TRANSFORM_LUT_3X1D,
	COLOR_TRANSFORM_MATRIX,
	COLOR_TRANSFORM_PIPELINE,
};

/* Refcounted map from linear sRGB */
struct nx_color_transform {
	int ref_count;
	struct nx_addon_set addons; // per-renderer state

	enum nx_color_transform_type type;
};

struct nx_color_transform_inverse_eotf {
	struct nx_color_transform base;

	enum nx_color_transfer_function tf;
};

/* Indexed channel * dim + i */
struct nx_color_transform_lut_3x1d {
	struct nx_color_transform base;

	uint16_t *lut_3x1d;
	size_t dim;
};

struct nx_color_transform_matrix {
	struct nx_color_transform base;

	float matrix[9];
};

struct nx_color_transform_pipeline {
	struct nx_color_transform base;

	struct nx_color_transform **transforms;
	size_t len;
};

struct nx_color_transform *nx_color_transform_init_linear_to_inverse_eotf(
	enum nx_color_transfer_function tf);

/* dim entries per channel */
struct nx_color_transform *nx_color_transform_init_lut_3x1d(size_t dim,
	const uint16_t *r, const uint16_t *g, const uint16_t *b);

struct nx_color_transform *nx_color_transform_init_matrix(const float matrix[static 9]);

/* Applies transforms in order */
struct nx_color_transform *nx_color_transform_init_pipeline(
	struct nx_color_transform **transforms, size_t len);

struct nx_color_transform *nx_color_transform_ref(struct nx_color_transform *tr);

void nx_color_transform_unref(struct nx_color_transform *tr);

void nx_color_transform_eval(struct nx_color_transform *tr,
	float out[static 3], const float in[static 3]);

struct nx_color_transform_inverse_eotf *nx_color_transform_inverse_eotf_from_base(
	struct nx_color_transform *tr);

struct nx_color_transform_lut_3x1d *color_transform_lut_3x1d_from_base(
	struct nx_color_transform *tr);

/* Flattens pipelines, drops NULL entries */
bool color_transform_compose(struct nx_color_transform **result,
	struct nx_color_transform **transforms, size_t len);

void nx_color_primaries_from_named(struct nx_color_primaries *out,
	enum nx_color_named_primaries named);

/* Linear RGB conversion between primaries */
void nx_color_primaries_transform_absolute_colorimetric(
	const struct nx_color_primaries *source,
	const struct nx_color_primaries *destination, float matrix[static 9]);

void nx_color_transfer_function_get_default_luminance(enum nx_color_transfer_function tf,
	struct nx_color_luminances *lum);

#endif

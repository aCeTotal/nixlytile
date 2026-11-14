#include "platform/render/color.h"
#include "platform/output/cursor/cursor.h"

bool output_cursor_refresh_color_transform(struct nx_output_cursor *output_cursor,
		const struct nx_output_image_description *img_desc) {
	nx_color_transform_unref(output_cursor->color_transform);
	output_cursor->color_transform = NULL;
	if (img_desc == NULL) {
		return true;
	}

	struct nx_color_primaries primaries_srgb;
	nx_color_primaries_from_named(&primaries_srgb, NX_COLOR_NAMED_PRIMARIES_SRGB);
	struct nx_color_primaries primaries;
	nx_color_primaries_from_named(&primaries, img_desc->primaries);
	float matrix[9];
	nx_color_primaries_transform_absolute_colorimetric(&primaries_srgb, &primaries, matrix);

	// sRGB reference equals its max.
	struct nx_color_luminances dst_lum;
	nx_color_transfer_function_get_default_luminance(img_desc->transfer_function, &dst_lum);
	float luminance_multiplier = dst_lum.reference / dst_lum.max;
	for (int i = 0; i < 9; ++i) {
		matrix[i] *= luminance_multiplier;
	}

	struct nx_color_transform *transforms[] = {
		nx_color_transform_init_matrix(matrix),
		nx_color_transform_init_linear_to_inverse_eotf(img_desc->transfer_function),
	};
	if (transforms[0] == NULL || transforms[1] == NULL) {
		nx_color_transform_unref(transforms[0]);
		nx_color_transform_unref(transforms[1]);
		return false;
	}
	output_cursor->color_transform = nx_color_transform_init_pipeline(transforms,
		sizeof(transforms) / sizeof(transforms[0]));
	nx_color_transform_unref(transforms[0]);
	nx_color_transform_unref(transforms[1]);
	return output_cursor->color_transform != NULL;
}

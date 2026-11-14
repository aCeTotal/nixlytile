#include "platform/output/output.h"
#include "platform/render/color.h"
#include "platform/scene/render/frame.h"

float scene_luminance_multiplier(const struct nx_color_luminances *src_lum,
		const struct nx_color_luminances *dst_lum) {
	return (dst_lum->reference / src_lum->reference) * (src_lum->max / dst_lum->max);
}

static struct nx_color_transform *srgb_to_output_matrix(
		const struct nx_output_image_description *img_desc) {
	struct nx_color_primaries primaries_srgb;
	nx_color_primaries_from_named(&primaries_srgb, NX_COLOR_NAMED_PRIMARIES_SRGB);
	struct nx_color_primaries primaries;
	nx_color_primaries_from_named(&primaries, img_desc->primaries);
	float matrix[9];
	nx_color_primaries_transform_absolute_colorimetric(&primaries_srgb, &primaries, matrix);

	struct nx_color_luminances srgb_lum, dst_lum;
	nx_color_transfer_function_get_default_luminance(
		NX_COLOR_TRANSFER_FUNCTION_SRGB, &srgb_lum);
	nx_color_transfer_function_get_default_luminance(img_desc->transfer_function, &dst_lum);
	float luminance_multiplier = scene_luminance_multiplier(&srgb_lum, &dst_lum);
	for (int i = 0; i < 9; ++i) {
		matrix[i] *= luminance_multiplier;
	}

	return nx_color_transform_init_matrix(matrix);
}

bool scene_output_combine_color_transforms(struct nx_scene_output *scene_output,
		const struct nx_output_image_description *img_desc,
		struct nx_color_transform *render_gamma) {
	bool result = false;
	struct nx_color_transform *color_matrix = NULL;
	struct nx_color_transform *user_gamma = NULL;
	if (img_desc != NULL) {
		color_matrix = srgb_to_output_matrix(img_desc);
	}

	struct nx_color_transform *inv_eotf = nx_color_transform_init_linear_to_inverse_eotf(
		img_desc != NULL ? img_desc->transfer_function : NX_COLOR_TRANSFER_FUNCTION_GAMMA22);
	if (inv_eotf == NULL || (img_desc != NULL && color_matrix == NULL)) {
		goto cleanup_transforms;
	}

	if (render_gamma != NULL) {
		user_gamma = nx_color_transform_ref(render_gamma);
	}

	struct nx_color_transform *combined;
	struct nx_color_transform *transforms[] = {
		color_matrix,
		inv_eotf,
		user_gamma,
	};
	const size_t transforms_len = sizeof(transforms) / sizeof(transforms[0]);
	if (!color_transform_compose(&combined, transforms, transforms_len)) {
		goto cleanup_transforms;
	}

	struct nx_color_transform *gamma_lut = scene_output->gamma_lut_color_transform;
	nx_color_transform_unref(scene_output->prev_gamma_lut_color_transform);
	scene_output->prev_gamma_lut_color_transform = gamma_lut ? nx_color_transform_ref(gamma_lut) : NULL;
	nx_color_transform_unref(scene_output->combined_color_transform);
	scene_output->combined_color_transform = combined;

	result = true;

cleanup_transforms:
	nx_color_transform_unref(color_matrix);
	nx_color_transform_unref(inv_eotf);
	nx_color_transform_unref(user_gamma);
	return result;
}

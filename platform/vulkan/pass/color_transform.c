#include <stdlib.h>
#include <string.h>
#include "platform/geometry/matrix.h"
#include "platform/log/log.h"
#include "platform/render/color.h"
#include "platform/vulkan/pass/color_transform.h"
#include "platform/vulkan/pipeline/descriptor_pool.h"
#include "platform/vulkan/renderer.h"

#define LUT_DIM 33

static const struct nx_addon_interface vk_color_transform_impl = {
	.name = "vk_color_transform",
	.destroy = vulkan_color_transform_destroy,
};

// False when needing a LUT.
static bool unwrap_color_transform(struct nx_color_transform *transform,
		float matrix[static 9], enum nx_color_transfer_function *tf) {
	struct nx_color_transform_matrix *as_matrix;
	struct nx_color_transform_pipeline *pipeline;
	switch (transform->type) {
	case COLOR_TRANSFORM_INVERSE_EOTF:
		nx_matrix_identity(matrix);
		*tf = nx_color_transform_inverse_eotf_from_base(transform)->tf;
		return true;
	case COLOR_TRANSFORM_MATRIX:
		as_matrix = wl_container_of(transform, as_matrix, base);
		memcpy(matrix, as_matrix->matrix, sizeof(float[9]));
		*tf = NX_COLOR_TRANSFER_FUNCTION_EXT_LINEAR;
		return true;
	case COLOR_TRANSFORM_PIPELINE:
		pipeline = wl_container_of(transform, pipeline, base);
		if (pipeline->len != 2 ||
				pipeline->transforms[0]->type != COLOR_TRANSFORM_MATRIX ||
				pipeline->transforms[1]->type != COLOR_TRANSFORM_INVERSE_EOTF) {
			return false;
		}
		as_matrix = wl_container_of(pipeline->transforms[0], as_matrix, base);
		memcpy(matrix, as_matrix->matrix, sizeof(float[9]));
		*tf = nx_color_transform_inverse_eotf_from_base(pipeline->transforms[1])->tf;
		return true;
	case COLOR_TRANSFORM_LUT_3X1D:
		return false;
	}
	return false;
}

struct nx_vk_color_transform *vulkan_get_color_transform(
		struct nx_color_transform *transform, struct nx_vk_renderer *renderer) {
	struct nx_addon *addon =
		nx_addon_find(&transform->addons, renderer, &vk_color_transform_impl);
	if (!addon) {
		return NULL;
	}
	struct nx_vk_color_transform *vk_transform =
		wl_container_of(addon, vk_transform, addon);
	return vk_transform;
}

static struct nx_vk_color_transform *color_transform_create(
		struct nx_vk_renderer *renderer, struct nx_color_transform *transform) {
	struct nx_vk_color_transform *vk_transform = calloc(1, sizeof(*vk_transform));
	if (!vk_transform) {
		return NULL;
	}

	bool need_lut = !unwrap_color_transform(transform, vk_transform->color_matrix,
		&vk_transform->inverse_eotf);
	if (need_lut) {
		vk_transform->lut_3d.dim = LUT_DIM;
	}
	if (need_lut && !vulkan_lut3d_bake(renderer, &vk_transform->lut_3d, transform)) {
		free(vk_transform);
		return NULL;
	}

	nx_addon_init(&vk_transform->addon, &transform->addons, renderer,
		&vk_color_transform_impl);
	wl_list_insert(&renderer->color_transforms, &vk_transform->link);
	return vk_transform;
}

bool vulkan_ensure_color_transform(struct nx_vk_renderer *renderer,
		struct nx_color_transform *transform) {
	if (!transform || vulkan_get_color_transform(transform, renderer)) {
		return true;
	}
	if (color_transform_create(renderer, transform)) {
		return true;
	}
	nx_log(NX_ERROR, "Failed to create color transform");
	return false;
}

void vulkan_color_transform_destroy(struct nx_addon *addon) {
	struct nx_vk_renderer *renderer = (struct nx_vk_renderer *)addon->owner;
	struct nx_vk_color_transform *transform = wl_container_of(addon, transform, addon);

	if (transform->lut_3d.image) {
		vulkan_lut3d_finish(&transform->lut_3d, renderer->dev->dev);
		vulkan_free_ds(renderer, transform->lut_3d.ds_pool, transform->lut_3d.ds);
	}

	wl_list_remove(&transform->link);
	nx_addon_finish(&transform->addon);
	free(transform);
}

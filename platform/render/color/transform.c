#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "platform/render/color.h"

static void color_transform_init(struct nx_color_transform *tr, enum nx_color_transform_type type) {
	*tr = (struct nx_color_transform){
		.type = type,
		.ref_count = 1,
	};
	nx_addon_set_init(&tr->addons);
}

static struct nx_color_transform_pipeline *pipeline_from_base(struct nx_color_transform *tr) {
	struct nx_color_transform_pipeline *pipeline = wl_container_of(tr, pipeline, base);
	return pipeline;
}

struct nx_color_transform *nx_color_transform_init_linear_to_inverse_eotf(
		enum nx_color_transfer_function tf) {
	struct nx_color_transform_inverse_eotf *tx = calloc(1, sizeof(*tx));
	if (!tx) {
		return NULL;
	}
	color_transform_init(&tx->base, COLOR_TRANSFORM_INVERSE_EOTF);
	tx->tf = tf;
	return &tx->base;
}

struct nx_color_transform *nx_color_transform_init_lut_3x1d(size_t dim,
		const uint16_t *r, const uint16_t *g, const uint16_t *b) {
	uint16_t *lut_3x1d = malloc(3 * dim * sizeof(lut_3x1d[0]));
	if (lut_3x1d == NULL) {
		return NULL;
	}

	memcpy(&lut_3x1d[0 * dim], r, dim * sizeof(lut_3x1d[0]));
	memcpy(&lut_3x1d[1 * dim], g, dim * sizeof(lut_3x1d[0]));
	memcpy(&lut_3x1d[2 * dim], b, dim * sizeof(lut_3x1d[0]));

	struct nx_color_transform_lut_3x1d *tx = calloc(1, sizeof(*tx));
	if (!tx) {
		free(lut_3x1d);
		return NULL;
	}
	color_transform_init(&tx->base, COLOR_TRANSFORM_LUT_3X1D);
	tx->lut_3x1d = lut_3x1d;
	tx->dim = dim;
	return &tx->base;
}

struct nx_color_transform *nx_color_transform_init_matrix(const float matrix[static 9]) {
	struct nx_color_transform_matrix *tx = calloc(1, sizeof(*tx));
	if (!tx) {
		return NULL;
	}
	color_transform_init(&tx->base, COLOR_TRANSFORM_MATRIX);
	memcpy(tx->matrix, matrix, sizeof(tx->matrix));
	return &tx->base;
}

struct nx_color_transform *nx_color_transform_init_pipeline(
		struct nx_color_transform **transforms, size_t len) {
	assert(len > 0);

	struct nx_color_transform **copy = calloc(len, sizeof(copy[0]));
	if (copy == NULL) {
		return NULL;
	}

	struct nx_color_transform_pipeline *tx = calloc(1, sizeof(*tx));
	if (!tx) {
		free(copy);
		return NULL;
	}
	color_transform_init(&tx->base, COLOR_TRANSFORM_PIPELINE);

	for (size_t i = 0; i < len; i++) {
		assert(transforms[i] != NULL);
		copy[i] = nx_color_transform_ref(transforms[i]);
	}

	tx->transforms = copy;
	tx->len = len;

	return &tx->base;
}

static void pipeline_finish(struct nx_color_transform_pipeline *pipeline) {
	for (size_t i = 0; i < pipeline->len; i++) {
		nx_color_transform_unref(pipeline->transforms[i]);
	}
	free(pipeline->transforms);
}

static void color_transform_destroy(struct nx_color_transform *tr) {
	switch (tr->type) {
	case COLOR_TRANSFORM_INVERSE_EOTF:
	case COLOR_TRANSFORM_MATRIX:
		break;
	case COLOR_TRANSFORM_LUT_3X1D:
		free(color_transform_lut_3x1d_from_base(tr)->lut_3x1d);
		break;
	case COLOR_TRANSFORM_PIPELINE:
		pipeline_finish(pipeline_from_base(tr));
		break;
	}
	nx_addon_set_finish(&tr->addons);
	free(tr);
}

struct nx_color_transform *nx_color_transform_ref(struct nx_color_transform *tr) {
	tr->ref_count += 1;
	return tr;
}

void nx_color_transform_unref(struct nx_color_transform *tr) {
	if (!tr) {
		return;
	}
	assert(tr->ref_count > 0);
	tr->ref_count -= 1;
	if (tr->ref_count == 0) {
		color_transform_destroy(tr);
	}
}

struct nx_color_transform_inverse_eotf *nx_color_transform_inverse_eotf_from_base(
		struct nx_color_transform *tr) {
	assert(tr->type == COLOR_TRANSFORM_INVERSE_EOTF);
	struct nx_color_transform_inverse_eotf *inverse_eotf = wl_container_of(tr, inverse_eotf, base);
	return inverse_eotf;
}

struct nx_color_transform_lut_3x1d *color_transform_lut_3x1d_from_base(
		struct nx_color_transform *tr) {
	assert(tr->type == COLOR_TRANSFORM_LUT_3X1D);
	struct nx_color_transform_lut_3x1d *lut_3x1d = wl_container_of(tr, lut_3x1d, base);
	return lut_3x1d;
}

/* Counts and refs leaf transforms */
static size_t compose_collect(struct nx_color_transform **out,
		size_t out_capacity, struct nx_color_transform **transforms, size_t len) {
	size_t count = 0;
	for (size_t i = 0; i < len; i++) {
		struct nx_color_transform *transform = transforms[i];
		if (transform == NULL) {
			continue;
		}

		if (transform->type == COLOR_TRANSFORM_PIPELINE) {
			struct nx_color_transform_pipeline *pipeline = pipeline_from_base(transform);
			count += compose_collect(out, out_capacity, pipeline->transforms, pipeline->len);
			continue;
		}

		if (out_capacity > 0) {
			*out = nx_color_transform_ref(transform);
			out++;
			out_capacity--;
		}
		count++;
	}
	return count;
}

bool color_transform_compose(struct nx_color_transform **result,
		struct nx_color_transform **transforms, size_t len) {
	size_t result_len = compose_collect(NULL, 0, transforms, len);
	if (result_len == 0) {
		*result = NULL;
		return true;
	}

	struct nx_color_transform **flat = calloc(result_len, sizeof(flat[0]));
	if (flat == NULL) {
		return false;
	}
	compose_collect(flat, result_len, transforms, len);

	*result = result_len == 1 ? nx_color_transform_ref(flat[0]) :
		nx_color_transform_init_pipeline(flat, result_len);

	for (size_t i = 0; i < result_len; i++) {
		nx_color_transform_unref(flat[i]);
	}
	free(flat);
	return *result != NULL;
}

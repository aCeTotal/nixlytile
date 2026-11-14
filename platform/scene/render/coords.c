#include <math.h>
#include "platform/geometry/region.h"
#include "platform/geometry/transform.h"
#include "platform/scene/output/scene_output.h"
#include "platform/scene/render/frame.h"

static void region_to_buffer(pixman_region32_t *region, const struct render_data *data) {
	nx_region_transform(region, region, nx_output_transform_invert(data->transform),
		data->trans_width, data->trans_height);
}

void scene_logical_to_buffer(pixman_region32_t *region, const struct render_data *data) {
	nx_region_scale(region, region, data->scale);
	region_to_buffer(region, data);
}

void scene_logical_to_buffer_outward(pixman_region32_t *region,
		const struct render_data *data) {
	scene_region_scale_out(region, data->scale);
	region_to_buffer(region, data);
}

static int scale_length(int length, int offset, float scale) {
	return (int)(round((offset + length) * scale) - round(offset * scale));
}

static void scale_box(struct nx_box *box, float scale) {
	box->width = scale_length(box->width, box->x, scale);
	box->height = scale_length(box->height, box->y, scale);
	box->x = (int)round(box->x * scale);
	box->y = (int)round(box->y * scale);
}

void scene_transform_output_box(struct nx_box *box, const struct render_data *data) {
	enum wl_output_transform transform = nx_output_transform_invert(data->transform);
	scale_box(box, data->scale);
	nx_box_transform(box, box, transform, data->trans_width, data->trans_height);
}

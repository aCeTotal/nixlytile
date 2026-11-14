#include "platform/buffer/buffer.h"
#include "platform/geometry/transform.h"
#include "platform/surface/alpha_modifier.h"
#include "platform/surface/color_management.h"
#include "platform/surface/color_representation.h"
#include "platform/surface/compositor.h"
#include "platform/surface/syncobj.h"
#include "platform/scene/surface/scene_surface.h"

struct surface_color {
	enum nx_color_transfer_function transfer_function;
	enum nx_color_named_primaries primaries;
	enum nx_color_encoding encoding;
	enum nx_color_range range;
};

// Our lock must not block reuse.
static void client_buffer_mark_next_can_damage(struct nx_client_buffer *buffer) {
	buffer->n_ignore_locks++;
}

void scene_buffer_unmark_client_buffer(struct nx_scene_buffer *scene_buffer) {
	if (!scene_buffer->buffer) {
		return;
	}

	struct nx_client_buffer *buffer = nx_client_buffer_get(scene_buffer->buffer);
	// Single-pixel buffers were never marked.
	if (!buffer || buffer->n_ignore_locks == 0) {
		return;
	}

	buffer->n_ignore_locks--;
}

static bool client_buffer_is_single_pixel(struct nx_client_buffer *buffer) {
	return buffer->source != NULL &&
		nx_single_pixel_buffer_v1_try_from_buffer(buffer->source) != NULL;
}

static void clip_source_box(const struct nx_scene_surface *scene_surface,
		struct nx_fbox *src_box) {
	const struct nx_surface_state *state = &scene_surface->surface->current;
	const struct nx_box *clip = &scene_surface->clip;
	int width, height;
	scene_surface_clipped_size(scene_surface, &width, &height);

	int buffer_width = state->buffer_width;
	int buffer_height = state->buffer_height;
	nx_fbox_transform(src_box, src_box, state->transform,
		buffer_width, buffer_height);
	nx_output_transform_coords(state->transform, &buffer_width, &buffer_height);

	src_box->x += (double)(clip->x * src_box->width) / state->width;
	src_box->y += (double)(clip->y * src_box->height) / state->height;
	src_box->width *= (double)width / state->width;
	src_box->height *= (double)height / state->height;

	nx_fbox_transform(src_box, src_box, nx_output_transform_invert(state->transform),
		buffer_width, buffer_height);
}

static float surface_opacity(struct nx_surface *surface) {
	const struct nx_alpha_modifier_surface_v1_state *alpha_modifier_state =
		nx_alpha_modifier_v1_get_surface_state(surface);
	return alpha_modifier_state != NULL ?
		(float)alpha_modifier_state->multiplier : 1.0f;
}

static struct surface_color surface_color(struct nx_surface *surface) {
	struct surface_color color = {
		.transfer_function = NX_COLOR_TRANSFER_FUNCTION_GAMMA22,
		.primaries = NX_COLOR_NAMED_PRIMARIES_SRGB,
		.encoding = NX_COLOR_ENCODING_NONE,
		.range = NX_COLOR_RANGE_NONE,
	};

	const struct nx_image_description_v1_data *img_desc =
		nx_surface_get_image_description_v1_data(surface);
	if (img_desc != NULL) {
		color.transfer_function = nx_color_manager_v1_transfer_function_to_nx(img_desc->tf_named);
		color.primaries = nx_color_manager_v1_primaries_to_nx(img_desc->primaries_named);
	}

	const struct nx_color_representation_v1_surface_state *color_repr =
		nx_color_representation_v1_get_surface_state(surface);
	if (color_repr != NULL && color_repr->coefficients != 0) {
		color.encoding = nx_color_representation_v1_color_encoding_to_nx(color_repr->coefficients);
	}
	if (color_repr != NULL && color_repr->range != 0) {
		color.range = nx_color_representation_v1_color_range_to_nx(color_repr->range);
	}
	return color;
}

static void scene_buffer_attach_surface_buffer(struct nx_scene_buffer *scene_buffer,
		struct nx_surface *surface) {
	if (!surface->buffer) {
		nx_scene_buffer_set_buffer(scene_buffer, NULL);
		return;
	}

	// Cached single-pixel colors ignore in-place updates.
	if (!client_buffer_is_single_pixel(surface->buffer)) {
		client_buffer_mark_next_can_damage(surface->buffer);
	}

	struct nx_linux_drm_syncobj_surface_v1_state *syncobj_surface_state =
		nx_linux_drm_syncobj_v1_get_surface_state(surface);

	struct nx_scene_buffer_set_buffer_options options = {
		.damage = &surface->buffer_damage,
	};
	if (syncobj_surface_state != NULL) {
		options.wait_timeline = syncobj_surface_state->acquire_timeline;
		options.wait_point = syncobj_surface_state->acquire_point;
	}
	nx_scene_buffer_set_buffer_with_options(scene_buffer,
		&surface->buffer->base, &options);
}

void scene_surface_reconfigure(struct nx_scene_surface *scene_surface) {
	struct nx_scene_buffer *scene_buffer = scene_surface->buffer;
	struct nx_surface *surface = scene_surface->surface;
	struct nx_surface_state *state = &surface->current;

	struct nx_fbox src_box;
	nx_surface_get_buffer_source_box(surface, &src_box);

	pixman_region32_t opaque;
	pixman_region32_init(&opaque);
	pixman_region32_copy(&opaque, &surface->opaque_region);

	int width, height;
	scene_surface_clipped_size(scene_surface, &width, &height);
	if (!nx_box_empty(&scene_surface->clip)) {
		clip_source_box(scene_surface, &src_box);
		pixman_region32_translate(&opaque, -scene_surface->clip.x, -scene_surface->clip.y);
		pixman_region32_intersect_rect(&opaque, &opaque, 0, 0, width, height);
	}

	if (width <= 0 || height <= 0) {
		nx_scene_buffer_set_buffer(scene_buffer, NULL);
		pixman_region32_fini(&opaque);
		return;
	}

	float opacity = surface_opacity(surface);
	struct surface_color color = surface_color(surface);

	nx_scene_buffer_set_opaque_region(scene_buffer, &opaque);
	nx_scene_buffer_set_source_box(scene_buffer, &src_box);
	nx_scene_buffer_set_dest_size(scene_buffer, width, height);
	nx_scene_buffer_set_transform(scene_buffer, state->transform);
	nx_scene_buffer_set_opacity(scene_buffer, opacity);
	nx_scene_buffer_set_transfer_function(scene_buffer, color.transfer_function);
	nx_scene_buffer_set_primaries(scene_buffer, color.primaries);
	nx_scene_buffer_set_color_encoding(scene_buffer, color.encoding);
	nx_scene_buffer_set_color_range(scene_buffer, color.range);

	scene_buffer_unmark_client_buffer(scene_buffer);
	scene_buffer_attach_surface_buffer(scene_buffer, surface);

	pixman_region32_fini(&opaque);
}

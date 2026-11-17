#include "platform/buffer/buffer.h"
#include "platform/geometry/transform.h"
#include "platform/output/output.h"
#include "platform/output/commit/commit.h"
#include "platform/scene/graph/node.h"
#include "platform/scene/render/frame.h"

static bool color_management_is_scanout_allowed(const struct nx_output_image_description *img_desc,
		const struct nx_scene_buffer *buffer) {
	// Untagged buffers need an untagged output.
	if (buffer->transfer_function == 0 && buffer->primaries == 0) {
		return img_desc == NULL;
	}

	if (img_desc != NULL) {
		return img_desc->transfer_function == buffer->transfer_function &&
				img_desc->primaries == buffer->primaries;
	}

	// Untagged outputs show gamma 2.2 sRGB.
	return buffer->transfer_function == NX_COLOR_TRANSFER_FUNCTION_GAMMA22 &&
			buffer->primaries == NX_COLOR_NAMED_PRIMARIES_SRGB;
}

static bool color_representation_is_scanout_allowed(const struct nx_scene_buffer *buffer) {
	bool is_color_repr_none = buffer->color_encoding == NX_COLOR_ENCODING_NONE &&
			buffer->color_range == NX_COLOR_RANGE_NONE;
	bool is_color_repr_identity_full = buffer->color_encoding == NX_COLOR_ENCODING_IDENTITY &&
			buffer->color_range == NX_COLOR_RANGE_FULL;
	return is_color_repr_none || is_color_repr_identity_full;
}

static bool scene_entry_scanout_eligible(const struct render_list_entry *entry,
		const struct nx_output_state *state, const struct render_data *data) {
	struct nx_scene_output *scene_output = data->output;
	struct nx_scene_node *node = entry->node;
	if (!scene_output->scene->direct_scanout || node->type != NX_SCENE_NODE_BUFFER) {
		return false;
	}

	// Legacy DRM breaks modesets with scanout.
	if (state->committed & (NX_OUTPUT_STATE_MODE |
			NX_OUTPUT_STATE_ENABLED |
			NX_OUTPUT_STATE_RENDER_FORMAT)) {
		return false;
	}

	if (!nx_output_is_direct_scanout_allowed(scene_output->output)) {
		return false;
	}

	struct nx_scene_buffer *buffer = nx_scene_buffer_from_node(node);
	if (buffer->buffer == NULL || buffer->transform != data->transform ||
			buffer->corner_radius > 0) {
		return false;
	}

	const struct nx_output_image_description *img_desc =
		output_pending_image_description(scene_output->output, state);
	return color_management_is_scanout_allowed(img_desc, buffer) &&
		color_representation_is_scanout_allowed(buffer);
}

static void scanout_state_set_buffer(struct nx_output_state *pending,
		const struct render_list_entry *entry, const struct render_data *data) {
	struct nx_scene_output *scene_output = data->output;
	struct nx_scene_buffer *buffer = nx_scene_buffer_from_node(entry->node);

	// Native size after the transform.
	int default_width = buffer->buffer->width;
	int default_height = buffer->buffer->height;
	nx_output_transform_coords(buffer->transform, &default_width, &default_height);
	struct nx_fbox default_box = {
		.width = default_width,
		.height = default_height,
	};
	if (!nx_fbox_empty(&buffer->src_box) &&
			!nx_fbox_equal(&buffer->src_box, &default_box)) {
		pending->buffer_src_box = buffer->src_box;
	}

	pending->buffer_dst_box.x = entry->x - scene_output->x;
	pending->buffer_dst_box.y = entry->y - scene_output->y;
	scene_node_get_size(entry->node, &pending->buffer_dst_box.width,
		&pending->buffer_dst_box.height);
	scene_transform_output_box(&pending->buffer_dst_box, data);

	// Prefer the client's still-locked source buffer.
	struct nx_buffer *nx_buffer = buffer->buffer;
	struct nx_client_buffer *client_buffer = nx_client_buffer_get(nx_buffer);
	if (client_buffer != NULL && client_buffer->source != NULL &&
			client_buffer->source->n_locks > 0) {
		nx_buffer = client_buffer->source;
	}

	nx_output_state_set_buffer(pending, nx_buffer);
	if (buffer->wait_timeline != NULL) {
		nx_output_state_set_wait_timeline(pending, buffer->wait_timeline, buffer->wait_point);
	}
}

enum scene_direct_scanout_result scene_entry_try_direct_scanout(
		const struct render_list_entry *entry, struct nx_output_state *state,
		const struct render_data *data) {
	if (!scene_entry_scanout_eligible(entry, state, data)) {
		return SCANOUT_INELIGIBLE;
	}

	struct nx_scene_output *scene_output = data->output;
	struct nx_scene_buffer *buffer = nx_scene_buffer_from_node(entry->node);
	scene_buffer_send_scanout_feedback(scene_output, buffer);

	struct nx_output_state pending;
	nx_output_state_init(&pending);
	if (!nx_output_state_copy(&pending, state)) {
		return SCANOUT_CANDIDATE;
	}

	scanout_state_set_buffer(&pending, entry, data);
	if (scene_output->out_timeline) {
		scene_output->out_point++;
		nx_output_state_set_signal_timeline(&pending, scene_output->out_timeline,
			scene_output->out_point);
	}

	if (!nx_output_test_state(scene_output->output, &pending)) {
		nx_output_state_finish(&pending);
		return SCANOUT_CANDIDATE;
	}

	nx_output_state_copy(state, &pending);
	nx_output_state_finish(&pending);

	struct nx_scene_output_sample_event sample_event = {
		.output = scene_output,
		.direct_scanout = true,
		.release_timeline = scene_output->out_timeline,
		.release_point = scene_output->out_point,
	};
	wl_signal_emit_mutable(&buffer->events.output_sample, &sample_event);
	return SCANOUT_SUCCESS;
}

#include <stdlib.h>
#include "platform/render/drm_syncobj.h"
#include "platform/render/color.h"
#include "platform/output/commit/state.h"

void nx_output_state_init(struct nx_output_state *state) {
	*state = (struct nx_output_state){0};
	pixman_region32_init(&state->damage);
}

void nx_output_state_finish(struct nx_output_state *state) {
	nx_buffer_unlock(state->buffer);
	// Pointer may outlive the unlock.
	state->buffer = NULL;
	pixman_region32_fini(&state->damage);
	nx_drm_syncobj_timeline_unref(state->wait_timeline);
	nx_drm_syncobj_timeline_unref(state->signal_timeline);
	nx_color_transform_unref(state->color_transform);
	free(state->image_description);
}

void nx_output_state_set_enabled(struct nx_output_state *state,
		bool enabled) {
	state->committed |= NX_OUTPUT_STATE_ENABLED;
	state->enabled = enabled;
	state->allow_reconfiguration = true;
}

void nx_output_state_set_mode(struct nx_output_state *state,
		struct nx_output_mode *mode) {
	state->committed |= NX_OUTPUT_STATE_MODE;
	state->mode_type = NX_OUTPUT_STATE_MODE_FIXED;
	state->mode = mode;
	state->allow_reconfiguration = true;
}

void nx_output_state_set_custom_mode(struct nx_output_state *state,
		int32_t width, int32_t height, int32_t refresh) {
	state->committed |= NX_OUTPUT_STATE_MODE;
	state->mode_type = NX_OUTPUT_STATE_MODE_CUSTOM;
	state->custom_mode.width = width;
	state->custom_mode.height = height;
	state->custom_mode.refresh = refresh;
	state->allow_reconfiguration = true;
}

void nx_output_state_set_scale(struct nx_output_state *state, float scale) {
	state->committed |= NX_OUTPUT_STATE_SCALE;
	state->scale = scale;
}

void nx_output_state_set_transform(struct nx_output_state *state,
		enum wl_output_transform transform) {
	state->committed |= NX_OUTPUT_STATE_TRANSFORM;
	state->transform = transform;
}

void nx_output_state_set_adaptive_sync_enabled(struct nx_output_state *state,
		bool enabled) {
	state->committed |= NX_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
	state->adaptive_sync_enabled = enabled;
}

void nx_output_state_set_render_format(struct nx_output_state *state,
		uint32_t format) {
	state->committed |= NX_OUTPUT_STATE_RENDER_FORMAT;
	state->render_format = format;
}

void nx_output_state_set_buffer(struct nx_output_state *state,
		struct nx_buffer *buffer) {
	state->committed |= NX_OUTPUT_STATE_BUFFER;
	nx_buffer_unlock(state->buffer);
	state->buffer = nx_buffer_lock(buffer);
}

void nx_output_state_set_damage(struct nx_output_state *state,
		const pixman_region32_t *damage) {
	state->committed |= NX_OUTPUT_STATE_DAMAGE;
	pixman_region32_copy(&state->damage, damage);
}

void nx_output_state_set_wait_timeline(struct nx_output_state *state,
		struct nx_drm_syncobj_timeline *timeline, uint64_t src_point) {
	state->committed |= NX_OUTPUT_STATE_WAIT_TIMELINE;
	nx_drm_syncobj_timeline_unref(state->wait_timeline);
	state->wait_timeline = nx_drm_syncobj_timeline_ref(timeline);
	state->wait_point = src_point;
}

void nx_output_state_set_signal_timeline(struct nx_output_state *state,
		struct nx_drm_syncobj_timeline *timeline, uint64_t dst_point) {
	state->committed |= NX_OUTPUT_STATE_SIGNAL_TIMELINE;
	nx_drm_syncobj_timeline_unref(state->signal_timeline);
	state->signal_timeline = nx_drm_syncobj_timeline_ref(timeline);
	state->signal_point = dst_point;
}

void nx_output_state_set_color_transform(struct nx_output_state *state,
		struct nx_color_transform *tr) {
	state->committed |= NX_OUTPUT_STATE_COLOR_TRANSFORM;
	nx_color_transform_unref(state->color_transform);
	state->color_transform = tr != NULL ? nx_color_transform_ref(tr) : NULL;
}

bool nx_output_state_set_image_description(struct nx_output_state *state,
		const struct nx_output_image_description *image_desc) {
	struct nx_output_image_description *copy = NULL;
	if (image_desc != NULL) {
		copy = malloc(sizeof(*copy));
		if (copy == NULL) {
			return false;
		}
		*copy = *image_desc;
	}

	state->committed |= NX_OUTPUT_STATE_IMAGE_DESCRIPTION;
	free(state->image_description);
	state->image_description = copy;
	return true;
}

void nx_output_state_set_content_type(struct nx_output_state *state,
		uint32_t content_type) {
	state->committed |= NX_OUTPUT_STATE_CONTENT_TYPE;
	state->content_type = content_type;
}

// Fields that hold references.
static const uint32_t STATE_REFERENCE_FIELDS = NX_OUTPUT_STATE_BUFFER |
	NX_OUTPUT_STATE_DAMAGE | NX_OUTPUT_STATE_WAIT_TIMELINE |
	NX_OUTPUT_STATE_SIGNAL_TIMELINE | NX_OUTPUT_STATE_COLOR_TRANSFORM |
	NX_OUTPUT_STATE_IMAGE_DESCRIPTION;

bool nx_output_state_copy(struct nx_output_state *dst,
		const struct nx_output_state *src) {
	struct nx_output_state copy = *src;
	copy.committed &= ~STATE_REFERENCE_FIELDS;
	copy.buffer = NULL;
	copy.buffer_src_box = (struct nx_fbox){0};
	copy.buffer_dst_box = (struct nx_box){0};
	pixman_region32_init(&copy.damage);
	copy.wait_timeline = NULL;
	copy.signal_timeline = NULL;
	copy.color_transform = NULL;
	copy.image_description = NULL;

	if (src->committed & NX_OUTPUT_STATE_BUFFER) {
		nx_output_state_set_buffer(&copy, src->buffer);
		copy.buffer_src_box = src->buffer_src_box;
		copy.buffer_dst_box = src->buffer_dst_box;
	}
	if (src->committed & NX_OUTPUT_STATE_DAMAGE) {
		nx_output_state_set_damage(&copy, &src->damage);
	}
	if (src->committed & NX_OUTPUT_STATE_WAIT_TIMELINE) {
		nx_output_state_set_wait_timeline(&copy, src->wait_timeline,
			src->wait_point);
	}
	if (src->committed & NX_OUTPUT_STATE_SIGNAL_TIMELINE) {
		nx_output_state_set_signal_timeline(&copy, src->signal_timeline,
			src->signal_point);
	}
	if (src->committed & NX_OUTPUT_STATE_COLOR_TRANSFORM) {
		nx_output_state_set_color_transform(&copy, src->color_transform);
	}

	bool ok = !(src->committed & NX_OUTPUT_STATE_IMAGE_DESCRIPTION) ||
		nx_output_state_set_image_description(&copy, src->image_description);
	nx_output_state_finish(dst);
	if (ok) {
		*dst = copy;
	}
	return ok;
}

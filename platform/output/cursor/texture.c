#include <assert.h>
#include <math.h>
#include "platform/render/drm_syncobj.h"
#include "platform/log/log.h"
#include "platform/output/cursor/cursor.h"

bool nx_output_cursor_set_buffer(struct nx_output_cursor *cursor,
		struct nx_buffer *buffer, int32_t hotspot_x, int32_t hotspot_y) {
	struct nx_output *output = cursor->output;
	struct nx_renderer *renderer = output->renderer;
	assert(renderer != NULL);

	// Kept for direct plane passthrough.
	nx_buffer_unlock(cursor->source_buffer);
	cursor->source_buffer = buffer != NULL ? nx_buffer_lock(buffer) : NULL;

	struct nx_texture *texture = NULL;
	struct nx_fbox src_box = {0};
	int dst_width = 0, dst_height = 0;
	if (buffer != NULL) {
		texture = nx_texture_from_buffer(renderer, buffer);
		if (texture == NULL) {
			return false;
		}
		src_box = (struct nx_fbox){
			.width = texture->width,
			.height = texture->height,
		};
		dst_width = (int)((float)texture->width / output->scale);
		dst_height = (int)((float)texture->height / output->scale);
	}

	hotspot_x = (int32_t)((float)hotspot_x / output->scale);
	hotspot_y = (int32_t)((float)hotspot_y / output->scale);

	return output_cursor_set_texture(cursor, texture, true, &src_box,
		dst_width, dst_height, WL_OUTPUT_TRANSFORM_NORMAL, hotspot_x, hotspot_y,
		NULL, 0);
}

static void output_cursor_handle_renderer_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_output_cursor *cursor = wl_container_of(listener, cursor, renderer_destroy);
	output_cursor_set_texture(cursor, NULL, false, NULL, 0, 0,
		WL_OUTPUT_TRANSFORM_NORMAL, 0, 0, NULL, 0);
}

static void replace_texture(struct nx_output_cursor *cursor,
		struct nx_texture *texture, bool own_texture) {
	if (cursor->own_texture) {
		nx_texture_destroy(cursor->texture);
	}
	cursor->texture = texture;
	cursor->own_texture = own_texture;

	wl_list_remove(&cursor->renderer_destroy.link);
	if (texture != NULL) {
		cursor->renderer_destroy.notify = output_cursor_handle_renderer_destroy;
		wl_signal_add(&texture->renderer->events.destroy, &cursor->renderer_destroy);
	} else {
		wl_list_init(&cursor->renderer_destroy.link);
	}
}

static void replace_wait_timeline(struct nx_output_cursor *cursor,
		struct nx_drm_syncobj_timeline *timeline, uint64_t point) {
	nx_drm_syncobj_timeline_unref(cursor->wait_timeline);
	cursor->wait_timeline = NULL;
	cursor->wait_point = 0;
	if (timeline != NULL) {
		cursor->wait_timeline = nx_drm_syncobj_timeline_ref(timeline);
		cursor->wait_point = point;
	}
}

bool output_cursor_set_texture(struct nx_output_cursor *cursor,
		struct nx_texture *texture, bool own_texture, const struct nx_fbox *src_box,
		int dst_width, int dst_height, enum wl_output_transform transform,
		int32_t hotspot_x, int32_t hotspot_y,
		struct nx_drm_syncobj_timeline *wait_timeline, uint64_t wait_point) {
	// Still disabled.
	if (texture == NULL && !cursor->enabled) {
		return true;
	}

	struct nx_output *output = cursor->output;
	if (output->hardware_cursor != cursor) {
		output_cursor_damage_whole(cursor);
	}

	cursor->enabled = texture != NULL;
	cursor->width = 0;
	cursor->height = 0;
	if (texture != NULL) {
		cursor->width = (int)roundf((float)dst_width * output->scale);
		cursor->height = (int)roundf((float)dst_height * output->scale);
		cursor->src_box = *src_box;
		cursor->transform = transform;
	}
	cursor->hotspot_x = (int)roundf((float)hotspot_x * output->scale);
	cursor->hotspot_y = (int)roundf((float)hotspot_y * output->scale);
	output_cursor_update_visible(cursor);

	replace_texture(cursor, texture, own_texture);
	replace_wait_timeline(cursor, wait_timeline, wait_point);

	if (output->hardware_cursor != NULL && output->hardware_cursor != cursor) {
		output_cursor_damage_whole(cursor);
		return true;
	}
	if (output_cursor_attempt_hardware(cursor)) {
		return true;
	}

	nx_log(NX_DEBUG, "Falling back to software cursor on output '%s'", output->name);
	output_cursor_disable_hardware(output);
	output_cursor_damage_whole(cursor);
	return true;
}

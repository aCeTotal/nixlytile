#include <stdlib.h>
#include "platform/render/drm_syncobj.h"
#include "platform/output/cursor/cursor.h"

struct nx_output_cursor *nx_output_cursor_create(struct nx_output *output) {
	struct nx_output_cursor *cursor = calloc(1, sizeof(*cursor));
	if (cursor == NULL) {
		return NULL;
	}
	cursor->output = output;
	wl_list_insert(&output->cursors, &cursor->link);
	// (0, 0) is on-screen.
	cursor->visible = true;
	wl_list_init(&cursor->renderer_destroy.link);
	output_cursor_refresh_color_transform(cursor, output->image_description);
	return cursor;
}

void nx_output_cursor_destroy(struct nx_output_cursor *cursor) {
	if (cursor == NULL) {
		return;
	}
	if (cursor->output->hardware_cursor == cursor) {
		output_cursor_disable_hardware(cursor->output);
	} else {
		output_cursor_damage_whole(cursor);
	}
	wl_list_remove(&cursor->renderer_destroy.link);
	if (cursor->own_texture) {
		nx_texture_destroy(cursor->texture);
	}
	nx_buffer_unlock(cursor->source_buffer);
	nx_drm_syncobj_timeline_unref(cursor->wait_timeline);
	wl_list_remove(&cursor->link);
	nx_color_transform_unref(cursor->color_transform);
	free(cursor);
}

bool nx_output_cursor_move(struct nx_output_cursor *cursor,
		double x, double y) {
	struct nx_output *output = cursor->output;
	x *= output->scale;
	y *= output->scale;
	if (cursor->x == x && cursor->y == y) {
		return true;
	}

	if (output->hardware_cursor != cursor) {
		output_cursor_damage_whole(cursor);
	}

	cursor->x = x;
	cursor->y = y;
	bool was_visible = cursor->visible;
	output_cursor_update_visible(cursor);
	if (!was_visible && !cursor->visible) {
		return true;
	}

	if (output->hardware_cursor != cursor) {
		output_cursor_damage_whole(cursor);
		return true;
	}
	return output_cursor_move_hardware(output, (int)x, (int)y);
}

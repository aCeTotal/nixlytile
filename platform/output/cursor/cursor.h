#ifndef PLATFORM_OUTPUT_CURSOR_CURSOR_H
#define PLATFORM_OUTPUT_CURSOR_CURSOR_H

#include "platform/output/output.h"

void output_cursor_damage_whole(struct nx_output_cursor *cursor);
void output_cursor_update_visible(struct nx_output_cursor *cursor);
struct nx_buffer *output_cursor_render_buffer(struct nx_output_cursor *cursor);
bool output_cursor_attempt_hardware(struct nx_output_cursor *cursor);
bool output_cursor_move_hardware(struct nx_output *output, int x, int y);
void output_cursor_disable_hardware(struct nx_output *output);

bool output_cursor_set_texture(struct nx_output_cursor *cursor,
	struct nx_texture *texture, bool own_texture, const struct nx_fbox *src_box,
	int dst_width, int dst_height, enum wl_output_transform transform,
	int32_t hotspot_x, int32_t hotspot_y, struct nx_drm_syncobj_timeline *wait_timeline,
	uint64_t wait_point);
bool output_cursor_refresh_color_transform(struct nx_output_cursor *cursor,
	const struct nx_output_image_description *img_desc);

#endif

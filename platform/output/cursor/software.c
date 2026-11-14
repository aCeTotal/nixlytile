#include "platform/geometry/transform.h"
#include "platform/output/cursor/cursor.h"

// Cursor box in output coordinates.
static void output_cursor_get_box(struct nx_output_cursor *cursor,
		struct nx_box *box) {
	box->x = (int)(cursor->x - cursor->hotspot_x);
	box->y = (int)(cursor->y - cursor->hotspot_y);
	box->width = cursor->width;
	box->height = cursor->height;
}

void nx_output_add_software_cursors_to_render_pass(struct nx_output *output,
		struct nx_render_pass *render_pass, const pixman_region32_t *damage) {
	int width, height;
	nx_output_transformed_resolution(output, &width, &height);
	enum wl_output_transform inverted =
		nx_output_transform_invert(output->transform);

	struct nx_output_cursor *cursor;
	wl_list_for_each(cursor, &output->cursors, link) {
		if (!cursor->enabled || !cursor->visible ||
				output->hardware_cursor == cursor || cursor->texture == NULL) {
			continue;
		}

		struct nx_box box;
		output_cursor_get_box(cursor, &box);
		nx_box_transform(&box, &box, inverted, width, height);

		pixman_region32_t cursor_damage;
		pixman_region32_init_rect(&cursor_damage,
			box.x, box.y, box.width, box.height);
		if (damage != NULL) {
			pixman_region32_intersect(&cursor_damage, &cursor_damage, damage);
		}
		if (!pixman_region32_empty(&cursor_damage)) {
			nx_render_pass_add_texture(render_pass, &(struct nx_render_texture_options) {
				.texture = cursor->texture,
				.src_box = cursor->src_box,
				.dst_box = box,
				.clip = &cursor_damage,
				.transform = output->transform,
			});
		}
		pixman_region32_fini(&cursor_damage);
	}
}

void output_cursor_damage_whole(struct nx_output_cursor *cursor) {
	struct nx_box box;
	output_cursor_get_box(cursor, &box);

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, box.x, box.y, box.width, box.height);

	struct nx_output_event_damage event = {
		.damage = &damage,
	};
	wl_signal_emit_mutable(&cursor->output->events.damage, &event);

	pixman_region32_fini(&damage);
}

void output_cursor_update_visible(struct nx_output_cursor *cursor) {
	struct nx_box output_box = {0};
	nx_output_transformed_resolution(cursor->output, &output_box.width,
		&output_box.height);

	struct nx_box cursor_box;
	output_cursor_get_box(cursor, &cursor_box);
	cursor->visible = nx_box_intersects(&output_box, &cursor_box);
}

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include "platform/input/cursor/state.h"
#include "platform/input/xcursor/manager.h"
#include "platform/log/log.h"
#include "platform/output/cursor/cursor.h"
#include "platform/surface/compositor.h"
#include "platform/surface/fractional_scale.h"
#include "platform/surface/syncobj.h"

static void reset_image(struct nx_cursor_output_cursor *output_cursor) {
	output_cursor->xcursor = NULL;
	output_cursor->xcursor_index = 0;
	if (output_cursor->xcursor_timer != NULL) {
		wl_event_source_remove(output_cursor->xcursor_timer);
	}
	output_cursor->xcursor_timer = NULL;
}

static void set_xcursor_image(struct nx_cursor_output_cursor *output_cursor,
		size_t i);

static int handle_xcursor_timer(void *data) {
	struct nx_cursor_output_cursor *output_cursor = data;
	size_t next = (output_cursor->xcursor_index + 1) %
		output_cursor->xcursor->image_count;
	set_xcursor_image(output_cursor, next);
	return 0;
}

static void set_xcursor_image(struct nx_cursor_output_cursor *output_cursor,
		size_t i) {
	struct nx_xcursor_image *image = output_cursor->xcursor->images[i];
	nx_output_cursor_set_buffer(output_cursor->output_cursor,
		nx_xcursor_image_get_buffer(image), image->hotspot_x, image->hotspot_y);
	output_cursor->xcursor_index = i;

	if (output_cursor->xcursor->image_count == 1 || image->delay == 0) {
		return;
	}
	if (output_cursor->xcursor_timer == NULL) {
		output_cursor->xcursor_timer = wl_event_loop_add_timer(
			output_cursor->output_cursor->output->event_loop,
			handle_xcursor_timer, output_cursor);
	}
	if (output_cursor->xcursor_timer == NULL) {
		nx_log(NX_ERROR, "wl_event_loop_add_timer failed");
		return;
	}
	wl_event_source_timer_update(output_cursor->xcursor_timer, image->delay);
}

static void update_from_buffer(struct nx_cursor_output_cursor *output_cursor) {
	struct nx_cursor_state *state = output_cursor->cursor->state;
	struct nx_output_cursor *oc = output_cursor->output_cursor;
	struct nx_renderer *renderer = oc->output->renderer;
	assert(renderer != NULL);

	struct nx_texture *texture = nx_texture_from_buffer(renderer, state->buffer);
	struct nx_fbox src_box = {0};
	int dst_width = 0, dst_height = 0;
	if (texture != NULL) {
		src_box.width = texture->width;
		src_box.height = texture->height;
		dst_width = (int)(texture->width / state->buffer_scale);
		dst_height = (int)(texture->height / state->buffer_scale);
	}

	// For direct hardware cursor passthrough.
	oc->source_buffer = nx_buffer_lock(state->buffer);

	output_cursor_set_texture(oc, texture, true, &src_box, dst_width,
		dst_height, WL_OUTPUT_TRANSFORM_NORMAL, state->buffer_hotspot.x,
		state->buffer_hotspot.y, NULL, 0);
}

static float max_output_scale(struct nx_surface *surface) {
	float scale = 1;
	struct nx_surface_output *surface_output;
	wl_list_for_each(surface_output, &surface->current_outputs, link) {
		if (surface_output->output->scale > scale) {
			scale = surface_output->output->scale;
		}
	}
	return scale;
}

static void update_from_surface(struct nx_cursor_output_cursor *output_cursor) {
	struct nx_cursor_state *state = output_cursor->cursor->state;
	struct nx_output_cursor *oc = output_cursor->output_cursor;
	struct nx_surface *surface = state->surface;

	struct nx_texture *texture = nx_surface_get_texture(surface);
	struct nx_fbox src_box;
	nx_surface_get_buffer_source_box(surface, &src_box);
	struct nx_linux_drm_syncobj_surface_v1_state *syncobj =
		nx_linux_drm_syncobj_v1_get_surface_state(surface);

	output_cursor_set_texture(oc, texture, false, &src_box,
		surface->current.width, surface->current.height,
		surface->current.transform, state->surface_hotspot.x,
		state->surface_hotspot.y,
		syncobj != NULL ? syncobj->acquire_timeline : NULL,
		syncobj != NULL ? syncobj->acquire_point : 0);

	if (syncobj != NULL && surface->buffer != NULL &&
			surface->buffer->source != NULL &&
			(surface->current.committed & NX_SURFACE_STATE_BUFFER)) {
		nx_linux_drm_syncobj_v1_state_signal_release_with_buffer(syncobj,
			surface->buffer->source);
	}

	if (oc->visible) {
		nx_surface_send_enter(surface, oc->output);
	} else {
		nx_surface_send_leave(surface, oc->output);
	}

	float scale = max_output_scale(surface);
	nx_fractional_scale_v1_notify_scale(surface, scale);
	nx_surface_set_preferred_buffer_scale(surface, (int32_t)ceil(scale));
}

// A wrong image beats none.
static struct nx_xcursor *find_xcursor(struct nx_xcursor_manager *manager,
		const char *name, float scale) {
	struct nx_xcursor *xcursor =
		nx_xcursor_manager_get_xcursor(manager, name, scale);
	if (xcursor != NULL) {
		return xcursor;
	}

	nx_log(NX_DEBUG, "XCursor theme is missing '%s' cursor, "
		"falling back to 'default'", name);
	xcursor = nx_xcursor_manager_get_xcursor(manager, "default", scale);
	if (xcursor == NULL) {
		nx_log(NX_DEBUG, "XCursor theme is missing a 'default' cursor");
	}
	return xcursor;
}

static void update_from_xcursor(struct nx_cursor_output_cursor *output_cursor) {
	struct nx_cursor_state *state = output_cursor->cursor->state;
	float scale = output_cursor->output_cursor->output->scale;
	nx_xcursor_manager_load(state->xcursor_manager, scale);

	struct nx_xcursor *xcursor =
		find_xcursor(state->xcursor_manager, state->xcursor_name, scale);
	if (xcursor == NULL) {
		nx_output_cursor_set_buffer(output_cursor->output_cursor, NULL, 0, 0);
		return;
	}
	output_cursor->xcursor = xcursor;
	set_xcursor_image(output_cursor, 0);
}

void cursor_output_update(struct nx_cursor_output_cursor *output_cursor) {
	struct nx_cursor_state *state = output_cursor->cursor->state;
	struct nx_output_cursor *oc = output_cursor->output_cursor;
	reset_image(output_cursor);

	// Buffer images set it again.
	nx_buffer_unlock(oc->source_buffer);
	oc->source_buffer = NULL;

	if (state->buffer != NULL) {
		update_from_buffer(output_cursor);
		return;
	}
	if (state->surface != NULL) {
		update_from_surface(output_cursor);
		return;
	}
	if (state->xcursor_name != NULL) {
		update_from_xcursor(output_cursor);
		return;
	}
	nx_output_cursor_set_buffer(oc, NULL, 0, 0);
}

void cursor_output_move(struct nx_cursor_output_cursor *output_cursor) {
	struct nx_cursor *cur = output_cursor->cursor;
	double x = cur->x, y = cur->y;
	nx_output_layout_output_coords(cur->state->layout,
		output_cursor->output_cursor->output, &x, &y);
	nx_output_cursor_move(output_cursor->output_cursor, x, y);
}

static void handle_output_commit(struct wl_listener *listener, void *data) {
	struct nx_cursor_output_cursor *output_cursor =
		wl_container_of(listener, output_cursor, output_commit);
	const struct nx_output_event_commit *event = data;

	if (event->state->committed & (NX_OUTPUT_STATE_SCALE |
			NX_OUTPUT_STATE_TRANSFORM | NX_OUTPUT_STATE_ENABLED |
			NX_OUTPUT_STATE_IMAGE_DESCRIPTION)) {
		cursor_output_update(output_cursor);
	}

	struct nx_surface *surface = output_cursor->cursor->state->surface;
	if (surface != NULL && output_cursor->output_cursor->visible &&
			(event->state->committed & NX_OUTPUT_STATE_BUFFER)) {
		nx_surface_send_frame_done(surface, &event->when);
	}
}

void cursor_output_destroy(struct nx_cursor_output_cursor *output_cursor) {
	reset_image(output_cursor);
	wl_list_remove(&output_cursor->layout_output_destroy.link);
	wl_list_remove(&output_cursor->link);
	wl_list_remove(&output_cursor->output_commit.link);
	nx_output_cursor_destroy(output_cursor->output_cursor);
	free(output_cursor);
}

static void handle_layout_output_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_cursor_output_cursor *output_cursor =
		wl_container_of(listener, output_cursor, layout_output_destroy);
	cursor_output_destroy(output_cursor);
}

void cursor_output_create(struct nx_cursor_state *state,
		struct nx_output_layout_output *l_output) {
	struct nx_cursor_output_cursor *output_cursor =
		calloc(1, sizeof(*output_cursor));
	if (output_cursor == NULL) {
		nx_log(NX_ERROR, "Failed to allocate nx_cursor_output_cursor");
		return;
	}
	output_cursor->cursor = &state->cursor;

	output_cursor->output_cursor = nx_output_cursor_create(l_output->output);
	if (output_cursor->output_cursor == NULL) {
		nx_log(NX_ERROR, "Failed to create nx_output_cursor");
		free(output_cursor);
		return;
	}

	output_cursor->layout_output_destroy.notify = handle_layout_output_destroy;
	wl_signal_add(&l_output->events.destroy,
		&output_cursor->layout_output_destroy);
	wl_list_insert(&state->output_cursors, &output_cursor->link);
	wl_signal_add(&l_output->output->events.commit,
		&output_cursor->output_commit);
	output_cursor->output_commit.notify = handle_output_commit;

	cursor_output_move(output_cursor);
	cursor_output_update(output_cursor);
}

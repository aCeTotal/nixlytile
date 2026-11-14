#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include "platform/output/layout/output_box.h"

static const struct nx_addon_interface addon_impl;

static void output_layout_output_destroy(
		struct nx_output_layout_output *l_output) {
	wl_signal_emit_mutable(&l_output->events.destroy, l_output);

	assert(wl_list_empty(&l_output->events.destroy.listener_list));

	nx_output_destroy_global(l_output->output);
	wl_list_remove(&l_output->commit.link);
	wl_list_remove(&l_output->link);
	nx_addon_finish(&l_output->addon);
	free(l_output);
}

static void output_layout_destroy(struct nx_output_layout *layout) {
	wl_signal_emit_mutable(&layout->events.destroy, layout);

	assert(wl_list_empty(&layout->events.add.listener_list));
	assert(wl_list_empty(&layout->events.change.listener_list));
	assert(wl_list_empty(&layout->events.destroy.listener_list));

	struct nx_output_layout_output *l_output, *temp;
	wl_list_for_each_safe(l_output, temp, &layout->outputs, link) {
		output_layout_output_destroy(l_output);
	}

	wl_list_remove(&layout->display_destroy.link);
	free(layout);
}

static void output_layout_handle_display_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_output_layout *layout = wl_container_of(listener, layout, display_destroy);
	output_layout_destroy(layout);
}

struct nx_output_layout *nx_output_layout_create(struct wl_display *display) {
	struct nx_output_layout *layout = calloc(1, sizeof(*layout));
	if (layout == NULL) {
		return NULL;
	}
	wl_list_init(&layout->outputs);
	layout->display = display;

	wl_signal_init(&layout->events.add);
	wl_signal_init(&layout->events.change);
	wl_signal_init(&layout->events.destroy);

	layout->display_destroy.notify = output_layout_handle_display_destroy;
	wl_display_add_destroy_listener(display, &layout->display_destroy);
	return layout;
}

// Places auto outputs right of the rest.
static void output_layout_reconfigure(struct nx_output_layout *layout) {
	int max_x = INT_MIN;
	int max_x_y = INT_MIN;

	struct nx_output_layout_output *l_output;
	struct nx_box output_box;
	wl_list_for_each(l_output, &layout->outputs, link) {
		if (l_output->auto_configured) {
			continue;
		}
		output_layout_output_get_box(l_output, &output_box);
		if (output_box.x + output_box.width > max_x) {
			max_x = output_box.x + output_box.width;
			max_x_y = output_box.y;
		}
	}

	if (max_x == INT_MIN) {
		max_x = 0;
		max_x_y = 0;
	}

	wl_list_for_each(l_output, &layout->outputs, link) {
		if (!l_output->auto_configured) {
			continue;
		}
		output_layout_output_get_box(l_output, &output_box);
		l_output->x = max_x;
		l_output->y = max_x_y;
		max_x += output_box.width;
	}

	wl_signal_emit_mutable(&layout->events.change, layout);
}

// Modeless outputs stay hidden.
static void output_update_global(struct nx_output_layout *layout,
		struct nx_output *output) {
	if (output->width > 0 && output->height > 0) {
		nx_output_create_global(output, layout->display);
		return;
	}
	nx_output_destroy_global(output);
}

static void handle_output_commit(struct wl_listener *listener, void *data) {
	struct nx_output_layout_output *l_output =
		wl_container_of(listener, l_output, commit);
	struct nx_output_event_commit *event = data;

	if (event->state->committed & (NX_OUTPUT_STATE_SCALE |
			NX_OUTPUT_STATE_TRANSFORM | NX_OUTPUT_STATE_MODE)) {
		output_layout_reconfigure(l_output->layout);
		output_update_global(l_output->layout, l_output->output);
	}
}

static void addon_destroy(struct nx_addon *addon) {
	assert(addon->impl == &addon_impl);
	struct nx_output_layout_output *l_output =
		wl_container_of(addon, l_output, addon);
	struct nx_output_layout *layout = l_output->layout;
	output_layout_output_destroy(l_output);
	output_layout_reconfigure(layout);
}

static const struct nx_addon_interface addon_impl = {
	.name = "nx_output_layout_output",
	.destroy = addon_destroy,
};

static struct nx_output_layout_output *output_layout_output_create(
		struct nx_output_layout *layout, struct nx_output *output) {
	struct nx_output_layout_output *l_output = calloc(1, sizeof(*l_output));
	if (l_output == NULL) {
		return NULL;
	}
	l_output->layout = layout;
	l_output->output = output;

	wl_signal_init(&l_output->events.destroy);

	// Append to keep auto placement.
	wl_list_insert(layout->outputs.prev, &l_output->link);

	wl_signal_add(&output->events.commit, &l_output->commit);
	l_output->commit.notify = handle_output_commit;

	nx_addon_init(&l_output->addon, &output->addons, layout, &addon_impl);
	return l_output;
}

static struct nx_output_layout_output *output_layout_add(struct nx_output_layout *layout,
		struct nx_output *output, int lx, int ly, bool auto_configured) {
	struct nx_output_layout_output *l_output =
		nx_output_layout_get(layout, output);
	bool is_new = l_output == NULL;
	if (is_new) {
		l_output = output_layout_output_create(layout, output);
	}
	if (l_output == NULL) {
		return NULL;
	}

	l_output->x = lx;
	l_output->y = ly;
	l_output->auto_configured = auto_configured;

	output_layout_reconfigure(layout);
	output_update_global(layout, output);

	if (is_new) {
		wl_signal_emit_mutable(&layout->events.add, l_output);
	}
	return l_output;
}

struct nx_output_layout_output *nx_output_layout_add(struct nx_output_layout *layout,
		struct nx_output *output, int lx, int ly) {
	return output_layout_add(layout, output, lx, ly, false);
}

struct nx_output_layout_output *nx_output_layout_add_auto(struct nx_output_layout *layout,
		struct nx_output *output) {
	return output_layout_add(layout, output, 0, 0, true);
}

void nx_output_layout_remove(struct nx_output_layout *layout,
		struct nx_output *output) {
	struct nx_output_layout_output *l_output =
		nx_output_layout_get(layout, output);
	if (l_output == NULL) {
		return;
	}
	output_layout_output_destroy(l_output);
	output_layout_reconfigure(layout);
}

struct nx_output_layout_output *nx_output_layout_get(
		struct nx_output_layout *layout, struct nx_output *reference) {
	struct nx_addon *addon =
		nx_addon_find(&reference->addons, layout, &addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct nx_output_layout_output *l_output =
		wl_container_of(addon, l_output, addon);
	return l_output;
}

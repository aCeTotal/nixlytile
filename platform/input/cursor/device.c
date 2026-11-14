#include <stdlib.h>
#include "platform/input/cursor/state.h"
#include "platform/input/device/input_device.h"
#include "platform/input/pointer/pointer.h"
#include "platform/log/log.h"
#include "platform/output/output.h"

struct nx_cursor_device {
	struct nx_cursor *cursor;
	struct nx_input_device *device;
	struct wl_list link;
	struct nx_output *mapped_output;

	struct wl_listener motion;
	struct wl_listener motion_absolute;
	struct wl_listener button;
	struct wl_listener axis;
	struct wl_listener frame;
	struct wl_listener destroy;
};

static struct nx_cursor_device *get_cursor_device(struct nx_cursor *cur,
		struct nx_input_device *device) {
	struct nx_cursor_device *c_device;
	wl_list_for_each(c_device, &cur->state->devices, link) {
		if (c_device->device == device) {
			return c_device;
		}
	}
	return NULL;
}

struct nx_output *cursor_device_mapped_output(struct nx_cursor *cur,
		struct nx_input_device *dev) {
	struct nx_cursor_device *c_device = get_cursor_device(cur, dev);
	return c_device != NULL ? c_device->mapped_output : NULL;
}

// Transforms normalized coordinates.
static void apply_output_transform(double *x, double *y,
		enum wl_output_transform transform) {
	double dx = 0.0, dy = 0.0;
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
		dx = *x;
		dy = *y;
		break;
	case WL_OUTPUT_TRANSFORM_90:
		dx = 1.0 - *y;
		dy = *x;
		break;
	case WL_OUTPUT_TRANSFORM_180:
		dx = 1.0 - *x;
		dy = 1.0 - *y;
		break;
	case WL_OUTPUT_TRANSFORM_270:
		dx = *y;
		dy = 1.0 - *x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		dx = 1.0 - *x;
		dy = *y;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		dx = *y;
		dy = *x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		dx = *x;
		dy = 1.0 - *y;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		dx = 1.0 - *y;
		dy = 1.0 - *x;
		break;
	}
	*x = dx;
	*y = dy;
}

static void handle_motion(struct wl_listener *listener, void *data) {
	struct nx_cursor_device *device = wl_container_of(listener, device, motion);
	wl_signal_emit_mutable(&device->cursor->events.motion, data);
}

static void handle_motion_absolute(struct wl_listener *listener, void *data) {
	struct nx_pointer_motion_absolute_event *event = data;
	struct nx_cursor_device *device =
		wl_container_of(listener, device, motion_absolute);
	if (device->mapped_output != NULL) {
		apply_output_transform(&event->x, &event->y,
			device->mapped_output->transform);
	}
	wl_signal_emit_mutable(&device->cursor->events.motion_absolute, event);
}

static void handle_button(struct wl_listener *listener, void *data) {
	struct nx_cursor_device *device = wl_container_of(listener, device, button);
	wl_signal_emit_mutable(&device->cursor->events.button, data);
}

static void handle_axis(struct wl_listener *listener, void *data) {
	struct nx_cursor_device *device = wl_container_of(listener, device, axis);
	wl_signal_emit_mutable(&device->cursor->events.axis, data);
}

static void handle_frame(struct wl_listener *listener, void *data) {
	struct nx_cursor_device *device = wl_container_of(listener, device, frame);
	wl_signal_emit_mutable(&device->cursor->events.frame, device->cursor);
}

static void cursor_device_destroy(struct nx_cursor_device *c_device) {
	wl_list_remove(&c_device->motion.link);
	wl_list_remove(&c_device->motion_absolute.link);
	wl_list_remove(&c_device->button.link);
	wl_list_remove(&c_device->axis.link);
	wl_list_remove(&c_device->frame.link);
	wl_list_remove(&c_device->link);
	wl_list_remove(&c_device->destroy.link);
	free(c_device);
}

static void handle_device_destroy(struct wl_listener *listener, void *data) {
	struct nx_cursor_device *c_device =
		wl_container_of(listener, c_device, destroy);
	cursor_device_destroy(c_device);
}

static void cursor_device_create(struct nx_cursor *cursor,
		struct nx_input_device *device) {
	struct nx_cursor_device *c_device = calloc(1, sizeof(*c_device));
	if (c_device == NULL) {
		nx_log(NX_ERROR, "Failed to allocate nx_cursor_device");
		return;
	}
	c_device->cursor = cursor;
	c_device->device = device;

	wl_signal_add(&device->events.destroy, &c_device->destroy);
	c_device->destroy.notify = handle_device_destroy;

	struct nx_pointer *pointer = nx_pointer_from_input_device(device);
	wl_signal_add(&pointer->events.motion, &c_device->motion);
	c_device->motion.notify = handle_motion;
	wl_signal_add(&pointer->events.motion_absolute, &c_device->motion_absolute);
	c_device->motion_absolute.notify = handle_motion_absolute;
	wl_signal_add(&pointer->events.button, &c_device->button);
	c_device->button.notify = handle_button;
	wl_signal_add(&pointer->events.axis, &c_device->axis);
	c_device->axis.notify = handle_axis;
	wl_signal_add(&pointer->events.frame, &c_device->frame);
	c_device->frame.notify = handle_frame;

	wl_list_insert(&cursor->state->devices, &c_device->link);
}

void nx_cursor_attach_input_device(struct nx_cursor *cur,
		struct nx_input_device *dev) {
	switch (dev->type) {
	case NX_INPUT_DEVICE_POINTER:
		break;
	case NX_INPUT_DEVICE_TABLET:
		return; // no cursor mapping for tablets
	default:
		nx_log(NX_ERROR, "only device types of pointer or tablet tool"
			" are supported");
		return;
	}

	if (get_cursor_device(cur, dev) == NULL) {
		cursor_device_create(cur, dev);
	}
}

void nx_cursor_map_input_to_output(struct nx_cursor *cur,
		struct nx_input_device *dev, struct nx_output *output) {
	struct nx_cursor_device *c_device = get_cursor_device(cur, dev);
	if (c_device == NULL) {
		nx_log(NX_ERROR, "Cannot map device \"%s\" to output"
			" (not found in this cursor)", dev->name);
		return;
	}
	c_device->mapped_output = output;
}

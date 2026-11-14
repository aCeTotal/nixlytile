#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include "platform/input/pointer/virtual_pointer_device.h"
#include "wlr-virtual-pointer-unstable-v1-protocol.h"

static const struct nx_pointer_impl pointer_impl = {
	.name = "virtual-pointer",
};

static const struct zwlr_virtual_pointer_v1_interface virtual_pointer_impl;

// NULL once the pointer is gone.
static struct nx_virtual_pointer_v1 *virtual_pointer_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwlr_virtual_pointer_v1_interface, &virtual_pointer_impl));
	return wl_resource_get_user_data(resource);
}

static void virtual_pointer_motion(struct wl_client *client,
		struct wl_resource *resource, uint32_t time,
		wl_fixed_t dx, wl_fixed_t dy) {
	struct nx_virtual_pointer_v1 *pointer =
		virtual_pointer_from_resource(resource);
	if (pointer == NULL) {
		return;
	}
	struct nx_pointer_motion_event event = {
		.pointer = &pointer->pointer,
		.time_msec = time,
		.delta_x = wl_fixed_to_double(dx),
		.delta_y = wl_fixed_to_double(dy),
		.unaccel_dx = wl_fixed_to_double(dx),
		.unaccel_dy = wl_fixed_to_double(dy),
	};
	wl_signal_emit_mutable(&pointer->pointer.events.motion, &event);
}

static void virtual_pointer_motion_absolute(struct wl_client *client,
		struct wl_resource *resource, uint32_t time, uint32_t x, uint32_t y,
		uint32_t x_extent, uint32_t y_extent) {
	struct nx_virtual_pointer_v1 *pointer =
		virtual_pointer_from_resource(resource);
	if (pointer == NULL || x_extent == 0 || y_extent == 0) {
		return;
	}
	struct nx_pointer_motion_absolute_event event = {
		.pointer = &pointer->pointer,
		.time_msec = time,
		.x = (double)x / x_extent,
		.y = (double)y / y_extent,
	};
	wl_signal_emit_mutable(&pointer->pointer.events.motion_absolute, &event);
}

static void virtual_pointer_button(struct wl_client *client,
		struct wl_resource *resource, uint32_t time, uint32_t button,
		uint32_t state) {
	struct nx_virtual_pointer_v1 *pointer =
		virtual_pointer_from_resource(resource);
	if (pointer == NULL) {
		return;
	}
	struct nx_pointer_button_event event = {
		.pointer = &pointer->pointer,
		.time_msec = time,
		.button = button,
		.state = state ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
	};
	nx_pointer_notify_button(&pointer->pointer, &event);
}

// NULL if inert or invalid.
static struct nx_virtual_pointer_v1 *axis_pointer(struct wl_resource *resource,
		uint32_t axis) {
	if (axis > WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
		wl_resource_post_error(resource,
			ZWLR_VIRTUAL_POINTER_V1_ERROR_INVALID_AXIS,
			"Invalid enumeration value %" PRIu32, axis);
		return NULL;
	}
	return virtual_pointer_from_resource(resource);
}

// Queues the axis until frame.
static struct nx_pointer_axis_event *pending_axis(
		struct nx_virtual_pointer_v1 *pointer, uint32_t time, uint32_t axis) {
	struct nx_pointer_axis_event *event = &pointer->axis_event[axis];
	pointer->axis_valid[axis] = true;
	event->pointer = &pointer->pointer;
	event->time_msec = time;
	event->orientation = axis;
	return event;
}

static void virtual_pointer_axis(struct wl_client *client,
		struct wl_resource *resource, uint32_t time, uint32_t axis,
		wl_fixed_t value) {
	struct nx_virtual_pointer_v1 *pointer = axis_pointer(resource, axis);
	if (pointer == NULL) {
		return;
	}
	pending_axis(pointer, time, axis)->delta = wl_fixed_to_double(value);
}

static void virtual_pointer_axis_stop(struct wl_client *client,
		struct wl_resource *resource, uint32_t time, uint32_t axis) {
	struct nx_virtual_pointer_v1 *pointer = axis_pointer(resource, axis);
	if (pointer == NULL) {
		return;
	}
	struct nx_pointer_axis_event *event = pending_axis(pointer, time, axis);
	event->delta = 0;
	event->delta_discrete = 0;
}

static void virtual_pointer_axis_discrete(struct wl_client *client,
		struct wl_resource *resource, uint32_t time, uint32_t axis,
		wl_fixed_t value, int32_t discrete) {
	struct nx_virtual_pointer_v1 *pointer = axis_pointer(resource, axis);
	if (pointer == NULL) {
		return;
	}
	struct nx_pointer_axis_event *event = pending_axis(pointer, time, axis);
	event->delta = wl_fixed_to_double(value);
	event->delta_discrete = discrete * NX_POINTER_AXIS_DISCRETE_STEP;
}

static void virtual_pointer_axis_source(struct wl_client *client,
		struct wl_resource *resource, uint32_t source) {
	if (source > WL_POINTER_AXIS_SOURCE_WHEEL_TILT) {
		wl_resource_post_error(resource,
			ZWLR_VIRTUAL_POINTER_V1_ERROR_INVALID_AXIS_SOURCE,
			"Invalid enumeration value %" PRIu32, source);
		return;
	}
	struct nx_virtual_pointer_v1 *pointer =
		virtual_pointer_from_resource(resource);
	if (pointer == NULL) {
		return;
	}
	for (size_t i = 0; i < NX_VIRTUAL_POINTER_AXIS_COUNT; i++) {
		pointer->axis_event[i].pointer = &pointer->pointer;
		pointer->axis_event[i].source = source;
	}
}

static void virtual_pointer_frame(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_virtual_pointer_v1 *pointer =
		virtual_pointer_from_resource(resource);
	if (pointer == NULL) {
		return;
	}

	for (size_t i = 0; i < NX_VIRTUAL_POINTER_AXIS_COUNT; ++i) {
		if (!pointer->axis_valid[i]) {
			continue;
		}
		wl_signal_emit_mutable(&pointer->pointer.events.axis,
			&pointer->axis_event[i]);
		pointer->axis_event[i] = (struct nx_pointer_axis_event){0};
		pointer->axis_valid[i] = false;
	}

	wl_signal_emit_mutable(&pointer->pointer.events.frame, &pointer->pointer);
}

static void virtual_pointer_destroy_resource(struct wl_resource *resource) {
	struct nx_virtual_pointer_v1 *pointer =
		virtual_pointer_from_resource(resource);
	if (pointer == NULL) {
		return;
	}

	nx_pointer_finish(&pointer->pointer);

	wl_resource_set_user_data(pointer->resource, NULL);
	wl_list_remove(&pointer->link);
	free(pointer);
}

static void virtual_pointer_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwlr_virtual_pointer_v1_interface virtual_pointer_impl = {
	.motion = virtual_pointer_motion,
	.motion_absolute = virtual_pointer_motion_absolute,
	.button = virtual_pointer_button,
	.axis = virtual_pointer_axis,
	.frame = virtual_pointer_frame,
	.axis_source = virtual_pointer_axis_source,
	.axis_stop = virtual_pointer_axis_stop,
	.axis_discrete = virtual_pointer_axis_discrete,
	.destroy = virtual_pointer_destroy,
};

struct nx_virtual_pointer_v1 *input_virtual_pointer_create(
		struct wl_resource *manager_resource, uint32_t id) {
	struct wl_client *client = wl_resource_get_client(manager_resource);
	struct nx_virtual_pointer_v1 *virtual_pointer = calloc(1, sizeof(*virtual_pointer));
	if (!virtual_pointer) {
		wl_client_post_no_memory(client);
		return NULL;
	}

	nx_pointer_init(&virtual_pointer->pointer, &pointer_impl,
		"nx_virtual_pointer_v1");

	struct wl_resource *pointer_resource = wl_resource_create(client,
		&zwlr_virtual_pointer_v1_interface,
		wl_resource_get_version(manager_resource), id);
	if (!pointer_resource) {
		free(virtual_pointer);
		wl_client_post_no_memory(client);
		return NULL;
	}

	wl_resource_set_implementation(pointer_resource, &virtual_pointer_impl,
		virtual_pointer, virtual_pointer_destroy_resource);
	virtual_pointer->resource = pointer_resource;
	return virtual_pointer;
}

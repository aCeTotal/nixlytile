#include <assert.h>
#include <wayland-server-core.h>
#include "platform/input/seat/client.h"

struct axis_send {
	uint32_t time;
	enum wl_pointer_axis orientation;
	double value;
	int32_t value_discrete;
	enum wl_pointer_axis_source source;
	enum wl_pointer_axis_relative_direction relative_direction;
	bool send_source;
	double low_res_value;
	int32_t low_res_discrete;
};

void nx_seat_pointer_send_motion(struct nx_seat *seat, uint32_t time,
		double sx, double sy) {
	struct nx_seat_client *client = seat->pointer_state.focused_client;
	if (client == NULL) {
		return;
	}

	// Dedupe at client precision.
	wl_fixed_t sx_fixed = wl_fixed_from_double(sx);
	wl_fixed_t sy_fixed = wl_fixed_from_double(sy);
	bool moved = wl_fixed_from_double(seat->pointer_state.sx) != sx_fixed ||
		wl_fixed_from_double(seat->pointer_state.sy) != sy_fixed;
	nx_seat_pointer_warp(seat, sx, sy);
	if (!moved) {
		return;
	}

	struct wl_resource *resource;
	wl_resource_for_each(resource, &client->pointers) {
		if (nx_seat_client_from_pointer_resource(resource) == NULL) {
			continue;
		}
		wl_pointer_send_motion(resource, time, sx_fixed, sy_fixed);
	}
}

uint32_t nx_seat_pointer_send_button(struct nx_seat *seat, uint32_t time,
		uint32_t button, enum wl_pointer_button_state state) {
	struct nx_seat_client *client = seat->pointer_state.focused_client;
	if (client == NULL) {
		return 0;
	}

	uint32_t serial = nx_seat_client_next_serial(client);
	struct wl_resource *resource;
	wl_resource_for_each(resource, &client->pointers) {
		if (nx_seat_client_from_pointer_resource(resource) == NULL) {
			continue;
		}
		wl_pointer_send_button(resource, serial, time, button, state);
	}
	return serial;
}

// Restart on direction change.
static bool should_reset_value120(int32_t current, int32_t last) {
	if (last == 0) {
		return true;
	}
	return (current < 0 && last > 0) || (current > 0 && last < 0);
}

static void accumulate_value120(struct nx_seat_client *client,
		struct axis_send *axis) {
	if (axis->value_discrete == 0) {
		return;
	}

	int32_t *acc_discrete = &client->value120.acc_discrete[axis->orientation];
	int32_t *last_discrete = &client->value120.last_discrete[axis->orientation];
	double *acc_axis = &client->value120.acc_axis[axis->orientation];

	if (should_reset_value120(axis->value_discrete, *last_discrete)) {
		*acc_discrete = 0;
		*acc_axis = 0;
	}
	*acc_discrete += axis->value_discrete;
	*last_discrete = axis->value_discrete;
	*acc_axis += axis->value;

	axis->low_res_discrete = *acc_discrete / NX_POINTER_AXIS_DISCRETE_STEP;
	if (axis->low_res_discrete == 0) {
		return;
	}
	*acc_discrete -= axis->low_res_discrete * NX_POINTER_AXIS_DISCRETE_STEP;
	axis->low_res_value = *acc_axis;
	*acc_axis = 0;
}

// First axis event per frame.
static bool claim_axis_source(struct nx_seat_pointer_state *state,
		enum wl_pointer_axis_source source) {
	if (state->sent_axis_source) {
		assert(state->cached_axis_source == source);
		return false;
	}
	state->sent_axis_source = true;
	state->cached_axis_source = source;
	return true;
}

static void resource_send_axis_value(struct wl_resource *resource,
		uint32_t version, const struct axis_send *axis) {
	if (version >= WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION) {
		wl_pointer_send_axis_relative_direction(resource,
			axis->orientation, axis->relative_direction);
	}
	if (axis->value_discrete == 0) {
		wl_pointer_send_axis(resource, axis->time, axis->orientation,
			wl_fixed_from_double(axis->value));
		return;
	}
	if (version >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
		wl_pointer_send_axis_value120(resource, axis->orientation,
			axis->value_discrete);
		wl_pointer_send_axis(resource, axis->time, axis->orientation,
			wl_fixed_from_double(axis->value));
		return;
	}
	if (version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION) {
		wl_pointer_send_axis_discrete(resource, axis->orientation,
			axis->low_res_discrete);
	}
	wl_pointer_send_axis(resource, axis->time, axis->orientation,
		wl_fixed_from_double(axis->low_res_value));
}

static void resource_send_axis(struct wl_resource *resource,
		const struct axis_send *axis) {
	uint32_t version = wl_resource_get_version(resource);
	// Low-res clients wait whole clicks.
	if (version < WL_POINTER_AXIS_VALUE120_SINCE_VERSION &&
			axis->value_discrete != 0 && axis->low_res_discrete == 0) {
		return;
	}

	if (axis->send_source && version >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION) {
		wl_pointer_send_axis_source(resource, axis->source);
	}
	if (axis->value != 0) {
		resource_send_axis_value(resource, version, axis);
	} else if (version >= WL_POINTER_AXIS_STOP_SINCE_VERSION) {
		wl_pointer_send_axis_stop(resource, axis->time, axis->orientation);
	}
}

void nx_seat_pointer_send_axis(struct nx_seat *seat, uint32_t time,
		enum wl_pointer_axis orientation, double value,
		int32_t value_discrete, enum wl_pointer_axis_source source,
		enum wl_pointer_axis_relative_direction relative_direction) {
	struct nx_seat_client *client = seat->pointer_state.focused_client;
	if (client == NULL) {
		return;
	}

	struct axis_send axis = {
		.time = time,
		.orientation = orientation,
		.value = value,
		.value_discrete = value_discrete,
		.source = source,
		.relative_direction = relative_direction,
		.send_source = claim_axis_source(&seat->pointer_state, source),
	};
	accumulate_value120(client, &axis);

	struct wl_resource *resource;
	wl_resource_for_each(resource, &client->pointers) {
		if (nx_seat_client_from_pointer_resource(resource) == NULL) {
			continue;
		}
		resource_send_axis(resource, &axis);
	}
}

void nx_seat_pointer_send_frame(struct nx_seat *seat) {
	struct nx_seat_client *client = seat->pointer_state.focused_client;
	if (client == NULL) {
		return;
	}

	seat->pointer_state.sent_axis_source = false;

	struct wl_resource *resource;
	wl_resource_for_each(resource, &client->pointers) {
		if (nx_seat_client_from_pointer_resource(resource) == NULL) {
			continue;
		}
		seat_pointer_resource_send_frame(resource);
	}
}

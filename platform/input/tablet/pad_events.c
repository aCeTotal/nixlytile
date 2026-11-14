#include "platform/input/tablet/tablet_v2.h"

#define STRIP_WIRE_MAX 65535

void nx_send_tablet_v2_tablet_pad_button(
		struct nx_tablet_v2_tablet_pad *pad, size_t button,
		uint32_t time, enum zwp_tablet_pad_v2_button_state state) {
	if (pad->current_client) {
		zwp_tablet_pad_v2_send_button(pad->current_client->resource,
			time, button, state);
	}
}

void nx_send_tablet_v2_tablet_pad_strip(struct nx_tablet_v2_tablet_pad *pad,
		uint32_t strip, double position, bool finger, uint32_t time) {
	if (!pad->current_client ||
			!pad->current_client->strips ||
			!pad->current_client->strips[strip]) {
		return;
	}
	struct wl_resource *resource = pad->current_client->strips[strip];

	if (finger) {
		zwp_tablet_pad_strip_v2_send_source(resource,
			ZWP_TABLET_PAD_STRIP_V2_SOURCE_FINGER);
	}

	if (position < 0) {
		zwp_tablet_pad_strip_v2_send_stop(resource);
	} else {
		zwp_tablet_pad_strip_v2_send_position(resource,
			(uint32_t)(position * STRIP_WIRE_MAX));
	}
	zwp_tablet_pad_strip_v2_send_frame(resource, time);
}

void nx_send_tablet_v2_tablet_pad_ring(struct nx_tablet_v2_tablet_pad *pad,
		uint32_t ring, double position, bool finger, uint32_t time) {
	if (!pad->current_client ||
			!pad->current_client->rings ||
			!pad->current_client->rings[ring]) {
		return;
	}
	struct wl_resource *resource = pad->current_client->rings[ring];

	if (finger) {
		zwp_tablet_pad_ring_v2_send_source(resource,
			ZWP_TABLET_PAD_RING_V2_SOURCE_FINGER);
	}

	if (position < 0) {
		zwp_tablet_pad_ring_v2_send_stop(resource);
	} else {
		zwp_tablet_pad_ring_v2_send_angle(resource,
			wl_fixed_from_double(position));
	}
	zwp_tablet_pad_ring_v2_send_frame(resource, time);
}

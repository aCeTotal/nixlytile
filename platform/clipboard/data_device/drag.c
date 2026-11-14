#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/clipboard/data_device/drag.h"
#include "platform/log/log.h"

static void drag_handle_seat_client_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_drag *drag =
		wl_container_of(listener, drag, seat_client_destroy);
	drag->focus_client = NULL;
	wl_list_remove(&drag->seat_client_destroy.link);
}

static void drag_handle_focus_destroy(struct wl_listener *listener, void *data) {
	struct nx_drag *drag = wl_container_of(listener, drag, focus_destroy);
	drag_set_focus(drag, NULL, 0, 0);
}

static void drag_leave(struct nx_drag *drag) {
	struct nx_seat_client *focus_client = drag->focus_client;
	wl_list_remove(&drag->seat_client_destroy.link);

	// Retire offers without destroying source.
	struct nx_data_offer *offer, *tmp;
	wl_list_for_each_safe(offer, tmp, &focus_client->seat->drag_offers, link) {
		struct wl_client *client = wl_resource_get_client(offer->resource);
		if (!drag->dropped && offer->source == drag->source &&
				client == focus_client->client) {
			offer->source = NULL;
			data_offer_destroy(offer);
		}
	}

	struct wl_resource *resource;
	wl_resource_for_each(resource, &focus_client->data_devices) {
		wl_data_device_send_leave(resource);
	}

	drag->focus_client = NULL;
}

static struct nx_seat_client *drag_focus_client(struct nx_drag *drag,
		struct nx_surface *surface) {
	struct wl_client *client = wl_resource_get_client(surface->resource);
	// Sourceless drags stay client-local.
	if (!drag->source && drag->seat_client &&
			client != drag->seat_client->client) {
		return NULL;
	}
	return nx_seat_client_for_wl_client(drag->seat, client);
}

static struct nx_data_offer *drag_offer_create(struct nx_drag *drag,
		struct wl_resource *device_resource) {
	struct nx_data_offer *offer = data_offer_create(device_resource,
		drag->source, NX_DATA_OFFER_DRAG);
	if (offer == NULL) {
		wl_resource_post_no_memory(device_resource);
		return NULL;
	}

	data_offer_update_action(offer);

	if (wl_resource_get_version(offer->resource) >=
			WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION) {
		wl_data_offer_send_source_actions(offer->resource,
			drag->source->actions);
	}
	return offer;
}

// False when out of memory.
static bool drag_enter(struct nx_drag *drag, struct nx_seat_client *client,
		struct nx_surface *surface, double sx, double sy) {
	drag->source->accepted = false;

	uint32_t serial = wl_display_next_serial(drag->seat->display);
	struct wl_resource *device_resource;
	wl_resource_for_each(device_resource, &client->data_devices) {
		struct nx_data_offer *offer = drag_offer_create(drag, device_resource);
		if (offer == NULL) {
			return false;
		}
		wl_data_device_send_enter(device_resource, serial, surface->resource,
			wl_fixed_from_double(sx), wl_fixed_from_double(sy),
			offer->resource);
	}
	return true;
}

void drag_set_focus(struct nx_drag *drag, struct nx_surface *surface,
		double sx, double sy) {
	if (drag->focus == surface) {
		return;
	}

	if (drag->focus_client) {
		drag_leave(drag);
	}

	wl_list_remove(&drag->focus_destroy.link);
	wl_list_init(&drag->focus_destroy.link);
	drag->focus = NULL;

	struct nx_seat_client *focus_client =
		surface != NULL ? drag_focus_client(drag, surface) : NULL;
	if (focus_client == NULL) {
		wl_signal_emit_mutable(&drag->events.focus, drag);
		return;
	}

	if (drag->source != NULL &&
			!drag_enter(drag, focus_client, surface, sx, sy)) {
		return;
	}

	drag->focus = surface;
	drag->focus_client = focus_client;
	drag->focus_destroy.notify = drag_handle_focus_destroy;
	wl_signal_add(&surface->events.destroy, &drag->focus_destroy);
	drag->seat_client_destroy.notify = drag_handle_seat_client_destroy;
	wl_signal_add(&focus_client->events.destroy, &drag->seat_client_destroy);

	wl_signal_emit_mutable(&drag->events.focus, drag);
}

void drag_destroy(struct nx_drag *drag) {
	if (drag->cancelling) {
		return;
	}
	drag->cancelling = true;

	if (drag->started) {
		nx_seat_keyboard_end_grab(drag->seat);
		nx_seat_pointer_end_grab(drag->seat);
		drag_set_focus(drag, NULL, 0, 0);

		assert(drag->seat->drag == drag);
		drag->seat->drag = NULL;
	}

	// Emit after leave: handlers refocus.
	wl_signal_emit_mutable(&drag->events.destroy, drag);

	assert(wl_list_empty(&drag->events.focus.listener_list));
	assert(wl_list_empty(&drag->events.motion.listener_list));
	assert(wl_list_empty(&drag->events.drop.listener_list));
	assert(wl_list_empty(&drag->events.destroy.listener_list));

	if (drag->source) {
		wl_list_remove(&drag->source_destroy.link);
	}
	wl_list_remove(&drag->focus_destroy.link);

	if (drag->icon != NULL) {
		drag_icon_destroy(drag->icon);
	}
	free(drag);
}

static void drag_handle_icon_destroy(struct wl_listener *listener, void *data) {
	struct nx_drag *drag = wl_container_of(listener, drag, icon_destroy);
	drag->icon = NULL;
	wl_list_remove(&drag->icon_destroy.link);
}

static void drag_handle_drag_source_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_drag *drag = wl_container_of(listener, drag, source_destroy);
	drag_destroy(drag);
}

// False on allocation failure.
static bool drag_attach_icon(struct nx_drag *drag,
		struct nx_surface *icon_surface) {
	if (icon_surface == NULL) {
		return true;
	}

	struct nx_drag_icon *icon = drag_icon_create(drag, icon_surface);
	if (icon == NULL) {
		return false;
	}

	drag->icon = icon;
	drag->icon_destroy.notify = drag_handle_icon_destroy;
	wl_signal_add(&icon->events.destroy, &drag->icon_destroy);
	return true;
}

struct nx_drag *drag_create(struct nx_seat_client *seat_client,
		struct nx_data_source *source, struct nx_surface *icon_surface) {
	struct nx_drag *drag = calloc(1, sizeof(*drag));
	if (drag == NULL) {
		return NULL;
	}

	wl_signal_init(&drag->events.focus);
	wl_signal_init(&drag->events.motion);
	wl_signal_init(&drag->events.drop);
	wl_signal_init(&drag->events.destroy);

	wl_list_init(&drag->focus_destroy.link);

	drag->seat = seat_client->seat;
	drag->seat_client = seat_client;

	if (!drag_attach_icon(drag, icon_surface)) {
		free(drag);
		return NULL;
	}

	drag->source = source;
	if (source != NULL) {
		drag->source_destroy.notify = drag_handle_drag_source_destroy;
		wl_signal_add(&source->events.destroy, &drag->source_destroy);
	}

	drag->pointer_grab.data = drag;
	drag->pointer_grab.interface = &drag_pointer_grab_impl;

	drag->keyboard_grab.data = drag;
	drag->keyboard_grab.interface = &drag_keyboard_grab_impl;

	return drag;
}

void nx_seat_request_start_drag(struct nx_seat *seat, struct nx_drag *drag,
		struct nx_surface *origin, uint32_t serial) {
	assert(drag->seat == seat);

	if (seat->drag != NULL) {
		nx_log(NX_DEBUG, "Rejecting start_drag request, "
			"another drag-and-drop operation is already in progress");
		return;
	}

	struct nx_seat_request_start_drag_event event = {
		.drag = drag,
		.origin = origin,
		.serial = serial,
	};
	wl_signal_emit_mutable(&seat->events.request_start_drag, &event);
}

static void seat_handle_drag_source_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_seat *seat =
		wl_container_of(listener, seat, drag_source_destroy);
	wl_list_remove(&seat->drag_source_destroy.link);
	seat->drag_source = NULL;
}

static void seat_start_drag(struct nx_seat *seat, struct nx_drag *drag) {
	assert(drag->seat == seat);
	assert(!drag->started);
	drag->started = true;

	nx_seat_keyboard_start_grab(seat, &drag->keyboard_grab);
	seat->drag = drag;

	// Listeners expect one drag source.
	nx_data_source_destroy(seat->drag_source);
	seat->drag_source = drag->source;
	if (drag->source != NULL) {
		seat->drag_source_destroy.notify = seat_handle_drag_source_destroy;
		wl_signal_add(&drag->source->events.destroy, &seat->drag_source_destroy);
	}

	wl_signal_emit_mutable(&seat->events.start_drag, drag);
}

void nx_seat_start_pointer_drag(struct nx_seat *seat, struct nx_drag *drag,
		uint32_t serial) {
	nx_seat_pointer_clear_focus(seat);
	nx_seat_pointer_start_grab(seat, &drag->pointer_grab);
	seat_start_drag(seat, drag);
}

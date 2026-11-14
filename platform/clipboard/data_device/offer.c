#undef _POSIX_C_SOURCE
#define _XOPEN_SOURCE 700 // for ffs()
#include <assert.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/log/log.h"

static const struct wl_data_offer_interface data_offer_impl;

static struct nx_data_offer *data_offer_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_data_offer_interface,
		&data_offer_impl));
	return wl_resource_get_user_data(resource);
}

static uint32_t data_offer_choose_action(struct nx_data_offer *offer) {
	uint32_t offer_actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	uint32_t preferred_action = 0;
	if (wl_resource_get_version(offer->resource) >=
			WL_DATA_OFFER_ACTION_SINCE_VERSION) {
		offer_actions = offer->actions;
		preferred_action = offer->preferred_action;
	}

	uint32_t source_actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	if (offer->source->actions >= 0) {
		source_actions = offer->source->actions;
	}

	uint32_t available_actions = offer_actions & source_actions;
	if (!available_actions) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
	}

	if ((preferred_action & available_actions) != 0) {
		return preferred_action;
	}

	// First available action, in bit order.
	return 1 << (ffs(available_actions) - 1);
}

void data_offer_update_action(struct nx_data_offer *offer) {
	assert(offer->type == NX_DATA_OFFER_DRAG);

	uint32_t action = data_offer_choose_action(offer);
	if (offer->source->current_dnd_action == action) {
		return;
	}
	offer->source->current_dnd_action = action;

	nx_data_source_dnd_action(offer->source, action);

	if (wl_resource_get_version(offer->resource) >=
			WL_DATA_OFFER_ACTION_SINCE_VERSION) {
		wl_data_offer_send_action(offer->resource, action);
	}
}

static void data_offer_handle_accept(struct wl_client *client,
		struct wl_resource *resource, uint32_t serial, const char *mime_type) {
	struct nx_data_offer *offer = data_offer_from_resource(resource);
	if (offer == NULL) {
		return;
	}

	if (offer->type != NX_DATA_OFFER_DRAG) {
		nx_log(NX_DEBUG, "Ignoring wl_data_offer.accept request on a "
			"non-drag-and-drop offer");
		return;
	}

	nx_data_source_accept(offer->source, serial, mime_type);
}

static void data_offer_handle_receive(struct wl_client *client,
		struct wl_resource *resource, const char *mime_type, int32_t fd) {
	struct nx_data_offer *offer = data_offer_from_resource(resource);
	if (offer == NULL) {
		close(fd);
		return;
	}

	nx_data_source_send(offer->source, mime_type, fd);
}

static void data_offer_source_dnd_finish(struct nx_data_offer *offer) {
	if (offer->source->actions < 0) {
		return;
	}
	nx_data_source_dnd_finish(offer->source);
}

static void data_offer_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void data_offer_handle_finish(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_data_offer *offer = data_offer_from_resource(resource);
	if (offer == NULL) {
		return;
	}

	if (offer->type != NX_DATA_OFFER_DRAG) {
		wl_resource_post_error(offer->resource,
			WL_DATA_OFFER_ERROR_INVALID_FINISH, "Offer is not drag-and-drop");
		return;
	}
	if (!offer->source->accepted) {
		wl_resource_post_error(offer->resource,
			WL_DATA_OFFER_ERROR_INVALID_FINISH, "Premature finish request");
		return;
	}
	enum wl_data_device_manager_dnd_action action =
		offer->source->current_dnd_action;
	if (action == WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE ||
			action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) {
		wl_resource_post_error(offer->resource,
			WL_DATA_OFFER_ERROR_INVALID_FINISH,
			"Offer finished with an invalid action");
		return;
	}

	data_offer_source_dnd_finish(offer);
	data_offer_destroy(offer);
}

static void data_offer_handle_set_actions(struct wl_client *client,
		struct wl_resource *resource, uint32_t actions,
		uint32_t preferred_action) {
	struct nx_data_offer *offer = data_offer_from_resource(resource);
	if (offer == NULL) {
		return;
	}

	if (actions & ~DATA_DEVICE_ALL_ACTIONS) {
		wl_resource_post_error(offer->resource,
			WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK,
			"invalid action mask %x", actions);
		return;
	}

	if (preferred_action && (!(preferred_action & actions) ||
			__builtin_popcount(preferred_action) > 1)) {
		wl_resource_post_error(offer->resource,
			WL_DATA_OFFER_ERROR_INVALID_ACTION,
			"invalid action %x", preferred_action);
		return;
	}

	if (offer->type != NX_DATA_OFFER_DRAG) {
		wl_resource_post_error(offer->resource,
			WL_DATA_OFFER_ERROR_INVALID_OFFER,
			"set_action can only be sent to drag-and-drop offers");
		return;
	}

	offer->actions = actions;
	offer->preferred_action = preferred_action;

	data_offer_update_action(offer);
}

static void data_offer_release_drag_source(struct nx_data_offer *offer) {
	// Pre-v3 targets never send finish.
	if (wl_resource_get_version(offer->resource) <
			WL_DATA_OFFER_ACTION_SINCE_VERSION) {
		data_offer_source_dnd_finish(offer);
		return;
	}
	if (offer->source->impl->dnd_finish) {
		nx_data_source_destroy(offer->source);
	}
}

void data_offer_destroy(struct nx_data_offer *offer) {
	if (offer == NULL) {
		return;
	}

	wl_list_remove(&offer->source_destroy.link);
	wl_list_remove(&offer->link);

	if (offer->type == NX_DATA_OFFER_DRAG && offer->source) {
		data_offer_release_drag_source(offer);
	}

	wl_resource_set_user_data(offer->resource, NULL);
	free(offer);
}

static const struct wl_data_offer_interface data_offer_impl = {
	.accept = data_offer_handle_accept,
	.receive = data_offer_handle_receive,
	.destroy = data_offer_handle_destroy,
	.finish = data_offer_handle_finish,
	.set_actions = data_offer_handle_set_actions,
};

static void data_offer_handle_resource_destroy(struct wl_resource *resource) {
	data_offer_destroy(data_offer_from_resource(resource));
}

static void data_offer_handle_source_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_data_offer *offer =
		wl_container_of(listener, offer, source_destroy);
	// Source is already being destroyed.
	offer->source = NULL;
	data_offer_destroy(offer);
}

struct nx_data_offer *data_offer_create(struct wl_resource *device_resource,
		struct nx_data_source *source, enum nx_data_offer_type type) {
	struct nx_seat_client *seat_client =
		seat_client_from_data_device_resource(device_resource);
	assert(seat_client != NULL);
	assert(source != NULL);

	struct nx_data_offer *offer = calloc(1, sizeof(*offer));
	if (offer == NULL) {
		return NULL;
	}
	offer->source = source;
	offer->type = type;

	struct wl_client *client = wl_resource_get_client(device_resource);
	uint32_t version = wl_resource_get_version(device_resource);
	offer->resource =
		wl_resource_create(client, &wl_data_offer_interface, version, 0);
	if (offer->resource == NULL) {
		free(offer);
		return NULL;
	}
	wl_resource_set_implementation(offer->resource, &data_offer_impl, offer,
		data_offer_handle_resource_destroy);

	switch (type) {
	case NX_DATA_OFFER_SELECTION:
		wl_list_insert(&seat_client->seat->selection_offers, &offer->link);
		break;
	case NX_DATA_OFFER_DRAG:
		wl_list_insert(&seat_client->seat->drag_offers, &offer->link);
		break;
	}

	offer->source_destroy.notify = data_offer_handle_source_destroy;
	wl_signal_add(&source->events.destroy, &offer->source_destroy);

	wl_data_device_send_data_offer(device_resource, offer->resource);

	char **p;
	wl_array_for_each(p, &source->mime_types) {
		wl_data_offer_send_offer(offer->resource, *p);
	}

	return offer;
}

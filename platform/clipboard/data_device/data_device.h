#ifndef PLATFORM_CLIPBOARD_DATA_DEVICE_DATA_DEVICE_H
#define PLATFORM_CLIPBOARD_DATA_DEVICE_DATA_DEVICE_H

#include <wayland-server-core.h>
#include "platform/input/seat/seat.h"

struct nx_data_device_manager {
	struct wl_global *global;

	struct {
		struct wl_listener display_destroy;
	};
};

// Only send is mandatory.
struct nx_data_source_impl {
	void (*send)(struct nx_data_source *source, const char *mime_type,
		int32_t fd);
	void (*accept)(struct nx_data_source *source, uint32_t serial,
		const char *mime_type);
	void (*destroy)(struct nx_data_source *source);

	void (*dnd_drop)(struct nx_data_source *source);
	void (*dnd_finish)(struct nx_data_source *source);
	void (*dnd_action)(struct nx_data_source *source,
		enum wl_data_device_manager_dnd_action action);
};

struct nx_data_source {
	const struct nx_data_source_impl *impl;

	struct wl_array mime_types;
	int32_t actions;

	bool accepted;

	enum wl_data_device_manager_dnd_action current_dnd_action;

	struct {
		struct wl_signal destroy;
	} events;
};

struct nx_drag;

struct nx_drag_icon {
	struct nx_drag *drag;
	struct nx_surface *surface;

	struct {
		struct wl_signal destroy;
	} events;

	void *data;

	struct {
		struct wl_listener surface_destroy;
	};
};

struct nx_drag {
	struct nx_seat_keyboard_grab keyboard_grab;
	struct nx_seat_pointer_grab pointer_grab;

	struct nx_seat *seat;
	struct nx_seat_client *seat_client;
	struct nx_seat_client *focus_client;

	struct nx_drag_icon *icon; // can be NULL
	struct nx_surface *focus; // can be NULL
	struct nx_data_source *source; // can be NULL

	bool started, dropped, cancelling;

	struct {
		struct wl_signal focus;
		struct wl_signal motion; // struct nx_drag_motion_event
		struct wl_signal drop; // struct nx_drag_drop_event
		struct wl_signal destroy;
	} events;

	struct {
		struct wl_listener source_destroy;
		struct wl_listener seat_client_destroy;
		struct wl_listener focus_destroy;
		struct wl_listener icon_destroy;
	};
};

struct nx_drag_motion_event {
	struct nx_drag *drag;
	uint32_t time;
	double sx, sy;
};

struct nx_drag_drop_event {
	struct nx_drag *drag;
	uint32_t time;
};

struct nx_data_device_manager *nx_data_device_manager_create(
	struct wl_display *display);

// Verifies serials of client requests.
void nx_seat_request_set_selection(struct nx_seat *seat,
	struct nx_seat_client *client, struct nx_data_source *source,
	uint32_t serial);
// NULL clears the selection.
void nx_seat_set_selection(struct nx_seat *seat,
	struct nx_data_source *source, uint32_t serial);

void nx_seat_request_start_drag(struct nx_seat *seat, struct nx_drag *drag,
	struct nx_surface *origin, uint32_t serial);
// Grabs keyboard and pointer.
void nx_seat_start_pointer_drag(struct nx_seat *seat, struct nx_drag *drag,
	uint32_t serial);

void nx_data_source_init(struct nx_data_source *source,
	const struct nx_data_source_impl *impl);
// Sends data, then closes fd.
void nx_data_source_send(struct nx_data_source *source, const char *mime_type,
	int32_t fd);
// NULL means no type accepted.
void nx_data_source_accept(struct nx_data_source *source, uint32_t serial,
	const char *mime_type);
void nx_data_source_destroy(struct nx_data_source *source);
// Dropped, not necessarily accepted.
void nx_data_source_dnd_drop(struct nx_data_source *source);
// May destroy the source.
void nx_data_source_dnd_finish(struct nx_data_source *source);
void nx_data_source_dnd_action(struct nx_data_source *source,
	enum wl_data_device_manager_dnd_action action);

#define DATA_DEVICE_ALL_ACTIONS (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | \
	WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE | \
	WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)

enum nx_data_offer_type {
	NX_DATA_OFFER_SELECTION,
	NX_DATA_OFFER_DRAG,
};

struct nx_data_offer {
	struct wl_resource *resource;
	struct nx_data_source *source;
	enum nx_data_offer_type type;
	struct wl_list link; // nx_seat.{selection_offers,drag_offers}

	uint32_t actions;
	enum wl_data_device_manager_dnd_action preferred_action;

	struct wl_listener source_destroy;
};

struct nx_client_data_source {
	struct nx_data_source source;
	struct nx_data_source_impl impl;
	struct wl_resource *resource;
	bool finalized;
};

struct nx_data_offer *data_offer_create(struct wl_resource *device_resource,
	struct nx_data_source *source, enum nx_data_offer_type type);
void data_offer_update_action(struct nx_data_offer *offer);
void data_offer_destroy(struct nx_data_offer *offer);

void client_data_source_create(struct wl_client *client, uint32_t version,
	uint32_t id);
struct nx_client_data_source *client_data_source_from_resource(
	struct wl_resource *resource);

void data_device_create(struct wl_resource *manager_resource, uint32_t id,
	struct wl_resource *seat_resource);
struct nx_seat_client *seat_client_from_data_device_resource(
	struct wl_resource *resource);
void data_device_send_selection(struct wl_resource *device_resource);
// Replaces the client's selection offers.
void seat_client_send_selection(struct nx_seat_client *seat_client);

#endif

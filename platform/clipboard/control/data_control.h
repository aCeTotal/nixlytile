#ifndef PLATFORM_CLIPBOARD_CONTROL_DATA_CONTROL_H
#define PLATFORM_CLIPBOARD_CONTROL_DATA_CONTROL_H

#include <wayland-server-core.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/clipboard/primary/primary_selection.h"

// One wire layout, two protocols.
struct control_protocol {
	const struct wl_interface *manager;
	const struct wl_interface *device;
	const struct wl_interface *source;
	const struct wl_interface *offer;
	int primary_selection_since;
};

struct control_source {
	struct wl_resource *resource;
	struct wl_array mime_types;
	bool finalized;

	// At most one is set.
	struct nx_data_source *active_source;
	struct nx_primary_selection_source *active_primary_source;
};

struct control_device {
	const struct control_protocol *protocol;
	struct wl_resource *resource;
	struct nx_seat *seat;
	struct wl_resource *selection_offer_resource;
	struct wl_resource *primary_selection_offer_resource;

	struct wl_listener seat_destroy;
	struct wl_listener seat_set_selection;
	struct wl_listener seat_set_primary_selection;
};

void control_source_create(const struct control_protocol *protocol,
	struct wl_resource *manager_resource, uint32_t id);
struct control_source *control_source_from_resource(
	struct wl_resource *resource);
struct nx_data_source *control_source_set_selection(
	struct control_source *source);
struct nx_primary_selection_source *control_source_set_primary_selection(
	struct control_source *source);

struct wl_resource *control_offer_create(struct control_device *device,
	struct wl_array *mime_types, bool is_primary);
// Accepts NULL.
void control_offer_destroy(struct wl_resource *resource);

void control_device_create(const struct control_protocol *protocol,
	struct wl_resource *manager_resource, uint32_t id,
	struct wl_resource *seat_resource);

#endif

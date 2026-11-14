#ifndef PLATFORM_CLIPBOARD_PRIMARY_DEVICE_H
#define PLATFORM_CLIPBOARD_PRIMARY_DEVICE_H

#include <wayland-server-core.h>
#include "platform/clipboard/primary/primary_selection.h"
#include "platform/clipboard/primary/primary_selection_v1.h"

// One per seat.
struct primary_device {
	struct nx_seat *seat;
	struct wl_list link; // nx_primary_selection_v1_device_manager.devices
	struct wl_list resources; // wl_resource_get_link()

	struct wl_list offers; // wl_resource_get_link()

	struct wl_listener seat_destroy;
	struct wl_listener seat_focus_change;
	struct wl_listener seat_set_primary_selection;
};

struct primary_device *primary_device_from_resource(
	struct wl_resource *resource);
void primary_device_bind(struct nx_primary_selection_v1_device_manager *manager,
	struct wl_resource *manager_resource, uint32_t id,
	struct wl_resource *seat_resource);
void primary_device_destroy(struct primary_device *device);

void primary_offer_create(struct wl_resource *device_resource,
	struct nx_primary_selection_source *source);
void primary_offer_destroy(struct wl_resource *resource);

void primary_source_create(struct wl_client *client,
	struct wl_resource *manager_resource, uint32_t id);
// Marks the client source set.
struct nx_primary_selection_source *primary_source_finalize(
	struct wl_resource *source_resource);

#endif

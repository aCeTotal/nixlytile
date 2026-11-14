#ifndef PLATFORM_CLIPBOARD_PRIMARY_PRIMARY_SELECTION_H
#define PLATFORM_CLIPBOARD_PRIMARY_PRIMARY_SELECTION_H

#include <wayland-server-core.h>
#include "platform/input/seat/seat.h"

struct nx_primary_selection_source;

// Only send is mandatory.
struct nx_primary_selection_source_impl {
	void (*send)(struct nx_primary_selection_source *source,
		const char *mime_type, int fd);
	void (*destroy)(struct nx_primary_selection_source *source);
};

struct nx_primary_selection_source {
	const struct nx_primary_selection_source_impl *impl;

	struct wl_array mime_types;

	struct {
		struct wl_signal destroy;
	} events;
};

void nx_primary_selection_source_init(
	struct nx_primary_selection_source *source,
	const struct nx_primary_selection_source_impl *impl);
void nx_primary_selection_source_destroy(
	struct nx_primary_selection_source *source);
void nx_primary_selection_source_send(
	struct nx_primary_selection_source *source, const char *mime_type,
	int fd);

// Verifies serials of client requests.
void nx_seat_request_set_primary_selection(struct nx_seat *seat,
	struct nx_seat_client *client,
	struct nx_primary_selection_source *source, uint32_t serial);
// NULL clears the selection.
void nx_seat_set_primary_selection(struct nx_seat *seat,
	struct nx_primary_selection_source *source, uint32_t serial);

#endif

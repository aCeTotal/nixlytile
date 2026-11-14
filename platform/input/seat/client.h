#ifndef PLATFORM_INPUT_SEAT_CLIENT_H
#define PLATFORM_INPUT_SEAT_CLIENT_H

#include <wayland-server-core.h>
#include "platform/input/seat/seat.h"

void seat_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id);
void seat_client_destroy(struct nx_seat_client *client);
void seat_resource_make_inert(struct wl_resource *resource);

void seat_client_create_pointer(struct nx_seat_client *seat_client,
		uint32_t version, uint32_t id);
void seat_client_create_inert_pointer(struct wl_client *client,
		uint32_t version, uint32_t id);
void seat_client_send_pointer_leave_raw(struct nx_seat_client *seat_client,
		struct nx_surface *surface);
void seat_pointer_resource_send_frame(struct wl_resource *resource);

void seat_client_create_keyboard(struct nx_seat_client *seat_client,
		uint32_t version, uint32_t id);
void seat_client_create_inert_keyboard(struct wl_client *client,
		uint32_t version, uint32_t id);
void seat_client_send_keyboard_leave_raw(struct nx_seat_client *seat_client,
		struct nx_surface *surface);
void seat_client_send_keymap(struct nx_seat_client *client,
		struct nx_keyboard *keyboard);
void seat_client_send_repeat_info(struct nx_seat_client *client,
		struct nx_keyboard *keyboard);
// NULL for inert resources.
struct nx_seat_client *seat_client_from_keyboard_resource(
		struct wl_resource *resource);

#endif

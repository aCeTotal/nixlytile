#ifndef PLATFORM_CLIPBOARD_DATA_DEVICE_DRAG_H
#define PLATFORM_CLIPBOARD_DATA_DEVICE_DRAG_H

#include "platform/clipboard/data_device/data_device.h"
#include "platform/surface/compositor.h"

extern const struct nx_pointer_grab_interface drag_pointer_grab_impl;
extern const struct nx_keyboard_grab_interface drag_keyboard_grab_impl;
extern const struct nx_surface_role drag_icon_surface_role;

struct nx_drag *drag_create(struct nx_seat_client *seat_client,
	struct nx_data_source *source, struct nx_surface *icon_surface);
void drag_set_focus(struct nx_drag *drag, struct nx_surface *surface,
	double sx, double sy);
void drag_destroy(struct nx_drag *drag);

struct nx_drag_icon *drag_icon_create(struct nx_drag *drag,
	struct nx_surface *surface);
void drag_icon_destroy(struct nx_drag_icon *icon);

#endif

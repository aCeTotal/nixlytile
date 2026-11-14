#ifndef PLATFORM_INPUT_LIBINPUT_LIBINPUT_DEVICE_H
#define PLATFORM_INPUT_LIBINPUT_LIBINPUT_DEVICE_H

#include <libinput.h>
#include <wayland-server-core.h>
#include "platform/backend/backend_interface.h"
#include "platform/input/device/switch.h"
#include "platform/input/keyboard/keyboard.h"
#include "platform/input/libinput/libinput_backend.h"
#include "platform/input/pointer/pointer.h"
#include "platform/input/tablet/pad_device.h"
#include "platform/input/tablet/tool_device.h"

struct nx_libinput_backend {
	struct nx_backend backend;

	struct nx_session *session;

	struct libinput *libinput_context;
	struct wl_event_source *input_event;

	struct wl_listener session_destroy;
	struct wl_listener session_signal;

	struct wl_list devices; // nx_libinput_input_device.link
};

// One sub-device per libinput capability.
struct nx_libinput_input_device {
	struct libinput_device *handle;

	struct nx_keyboard keyboard;
	struct nx_pointer pointer;
	struct nx_switch switch_device;
	struct nx_tablet tablet;
	struct wl_list tablet_tools; // nx_libinput_tablet_tool.link
	struct nx_tablet_pad tablet_pad;

	struct wl_list link; // nx_libinput_backend.devices
};

struct nx_libinput_tablet_tool {
	struct nx_tablet_tool nx_tool;
	struct libinput_tablet_tool *handle;
	struct wl_list link; // nx_libinput_input_device.tablet_tools
};

static inline uint32_t usec_to_msec(uint64_t usec) {
	return (uint32_t)(usec / 1000);
}

void handle_libinput_event(struct nx_libinput_backend *backend,
	struct libinput_event *event);
void destroy_libinput_input_device(struct nx_libinput_input_device *dev);
// NULL for empty libinput names.
const char *get_libinput_device_name(struct libinput_device *device);
bool button_state_from_libinput(enum libinput_button_state state,
	enum nx_button_state *out);

extern const struct nx_keyboard_impl libinput_keyboard_impl;
extern const struct nx_pointer_impl libinput_pointer_impl;
extern const struct nx_switch_impl libinput_switch_impl;
extern const struct nx_tablet_impl libinput_tablet_impl;
extern const struct nx_tablet_pad_impl libinput_tablet_pad_impl;

struct nx_input_device *init_device_keyboard(
	struct nx_libinput_input_device *dev);
struct nx_libinput_input_device *device_from_keyboard(struct nx_keyboard *kb);
void handle_keyboard_key(struct libinput_event *event, struct nx_keyboard *kb);

struct nx_input_device *init_device_pointer(
	struct nx_libinput_input_device *dev);
struct nx_libinput_input_device *device_from_pointer(
	struct nx_pointer *pointer);
void handle_pointer_motion(struct libinput_event *event,
	struct nx_pointer *pointer);
void handle_pointer_motion_abs(struct libinput_event *event,
	struct nx_pointer *pointer);
void handle_pointer_button(struct libinput_event *event,
	struct nx_pointer *pointer);
void handle_pointer_axis_value120(struct libinput_event *event,
	struct nx_pointer *pointer, enum wl_pointer_axis_source source);
void handle_pointer_swipe_begin(struct libinput_event *event,
	struct nx_pointer *pointer);
void handle_pointer_swipe_update(struct libinput_event *event,
	struct nx_pointer *pointer);
void handle_pointer_swipe_end(struct libinput_event *event,
	struct nx_pointer *pointer);
void handle_pointer_pinch_begin(struct libinput_event *event,
	struct nx_pointer *pointer);
void handle_pointer_pinch_update(struct libinput_event *event,
	struct nx_pointer *pointer);
void handle_pointer_pinch_end(struct libinput_event *event,
	struct nx_pointer *pointer);
void handle_pointer_hold_begin(struct libinput_event *event,
	struct nx_pointer *pointer);
void handle_pointer_hold_end(struct libinput_event *event,
	struct nx_pointer *pointer);

struct nx_input_device *init_device_switch(
	struct nx_libinput_input_device *dev);
struct nx_libinput_input_device *device_from_switch(
	struct nx_switch *switch_device);
void handle_switch_toggle(struct libinput_event *event,
	struct nx_switch *switch_device);

struct nx_input_device *init_device_tablet(
	struct nx_libinput_input_device *dev);
void finish_device_tablet(struct nx_libinput_input_device *dev);
struct nx_libinput_input_device *device_from_tablet(struct nx_tablet *tablet);
// Creates the tool on first sight.
struct nx_libinput_tablet_tool *input_libinput_tool_get(
	struct nx_libinput_input_device *dev,
	struct libinput_tablet_tool *libinput_tool);
void input_libinput_tool_destroy(struct nx_libinput_tablet_tool *tool);
void handle_tablet_tool_axis(struct libinput_event *event,
	struct nx_tablet *tablet);
void handle_tablet_tool_proximity(struct libinput_event *event,
	struct nx_tablet *tablet);
void handle_tablet_tool_tip(struct libinput_event *event,
	struct nx_tablet *tablet);
void handle_tablet_tool_button(struct libinput_event *event,
	struct nx_tablet *tablet);

struct nx_input_device *init_device_tablet_pad(
	struct nx_libinput_input_device *dev);
void finish_device_tablet_pad(struct nx_libinput_input_device *dev);
struct nx_libinput_input_device *device_from_tablet_pad(
	struct nx_tablet_pad *tablet_pad);
void handle_tablet_pad_button(struct libinput_event *event,
	struct nx_tablet_pad *tablet_pad);
void handle_tablet_pad_ring(struct libinput_event *event,
	struct nx_tablet_pad *tablet_pad);
void handle_tablet_pad_strip(struct libinput_event *event,
	struct nx_tablet_pad *tablet_pad);

#endif

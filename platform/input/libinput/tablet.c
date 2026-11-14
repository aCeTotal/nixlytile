#include <assert.h>
#include <libinput.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include "platform/input/libinput/libinput_device.h"
#include "platform/input/tablet/tool_interface.h"
#include "platform/log/log.h"

const struct nx_tablet_impl libinput_tablet_impl = {
	.name = "libinput-tablet-tool",
};

struct nx_input_device *init_device_tablet(
		struct nx_libinput_input_device *dev) {
	const char *name = get_libinput_device_name(dev->handle);
	struct nx_tablet *nx_tablet = &dev->tablet;
	nx_tablet_init(nx_tablet, &libinput_tablet_impl, name);

	if (libinput_device_get_id_bustype(dev->handle) == BUS_USB) {
		nx_tablet->usb_vendor_id = libinput_device_get_id_vendor(dev->handle);
		nx_tablet->usb_product_id = libinput_device_get_id_product(dev->handle);
	}

	struct udev_device *udev = libinput_device_get_udev_device(dev->handle);
	char **dst = wl_array_add(&nx_tablet->paths, sizeof(char *));
	*dst = strdup(udev_device_get_syspath(udev));
	udev_device_unref(udev);

	wl_list_init(&dev->tablet_tools);
	return &nx_tablet->base;
}

void finish_device_tablet(struct nx_libinput_input_device *dev) {
	struct nx_libinput_tablet_tool *tool, *next;
	wl_list_for_each_safe(tool, next, &dev->tablet_tools, link) {
		input_libinput_tool_destroy(tool);
	}

	nx_tablet_finish(&dev->tablet);
}

struct nx_libinput_input_device *device_from_tablet(
		struct nx_tablet *nx_tablet) {
	assert(nx_tablet->impl == &libinput_tablet_impl);

	struct nx_libinput_input_device *dev =
		wl_container_of(nx_tablet, dev, tablet);
	return dev;
}

static bool type_from_libinput(enum libinput_tablet_tool_type type,
		enum nx_tablet_tool_type *out) {
	switch (type) {
	case LIBINPUT_TABLET_TOOL_TYPE_PEN:
		*out = NX_TABLET_TOOL_TYPE_PEN;
		return true;
	case LIBINPUT_TABLET_TOOL_TYPE_ERASER:
		*out = NX_TABLET_TOOL_TYPE_ERASER;
		return true;
	case LIBINPUT_TABLET_TOOL_TYPE_BRUSH:
		*out = NX_TABLET_TOOL_TYPE_BRUSH;
		return true;
	case LIBINPUT_TABLET_TOOL_TYPE_PENCIL:
		*out = NX_TABLET_TOOL_TYPE_PENCIL;
		return true;
	case LIBINPUT_TABLET_TOOL_TYPE_AIRBRUSH:
		*out = NX_TABLET_TOOL_TYPE_AIRBRUSH;
		return true;
	case LIBINPUT_TABLET_TOOL_TYPE_MOUSE:
		*out = NX_TABLET_TOOL_TYPE_MOUSE;
		return true;
	case LIBINPUT_TABLET_TOOL_TYPE_LENS:
		*out = NX_TABLET_TOOL_TYPE_LENS;
		return true;
	case LIBINPUT_TABLET_TOOL_TYPE_TOTEM:
		*out = NX_TABLET_TOOL_TYPE_TOTEM;
		return true;
	}
	return false;
}

struct nx_libinput_tablet_tool *input_libinput_tool_get(
		struct nx_libinput_input_device *dev,
		struct libinput_tablet_tool *libinput_tool) {
	struct nx_libinput_tablet_tool *tool =
		libinput_tablet_tool_get_user_data(libinput_tool);
	if (tool) {
		return tool;
	}

	enum nx_tablet_tool_type type;
	if (!type_from_libinput(libinput_tablet_tool_get_type(libinput_tool), &type)) {
		nx_log(NX_DEBUG, "Unhandled libinput tablet tool type");
		return NULL;
	}

	tool = calloc(1, sizeof(*tool));
	if (tool == NULL) {
		nx_log_errno(NX_ERROR, "failed to allocate nx_libinput_tablet_tool");
		return NULL;
	}

	tool->nx_tool.type = type;
	tool->nx_tool.hardware_serial =
		libinput_tablet_tool_get_serial(libinput_tool);
	tool->nx_tool.hardware_wacom =
		libinput_tablet_tool_get_tool_id(libinput_tool);

	tool->nx_tool.pressure = libinput_tablet_tool_has_pressure(libinput_tool);
	tool->nx_tool.distance = libinput_tablet_tool_has_distance(libinput_tool);
	tool->nx_tool.tilt = libinput_tablet_tool_has_tilt(libinput_tool);
	tool->nx_tool.rotation = libinput_tablet_tool_has_rotation(libinput_tool);
	tool->nx_tool.slider = libinput_tablet_tool_has_slider(libinput_tool);
	tool->nx_tool.wheel = libinput_tablet_tool_has_wheel(libinput_tool);

	wl_signal_init(&tool->nx_tool.events.destroy);

	tool->handle = libinput_tablet_tool_ref(libinput_tool);
	libinput_tablet_tool_set_user_data(libinput_tool, tool);

	wl_list_insert(&dev->tablet_tools, &tool->link);
	return tool;
}

void input_libinput_tool_destroy(struct nx_libinput_tablet_tool *tool) {
	wl_signal_emit_mutable(&tool->nx_tool.events.destroy, &tool->nx_tool);
	libinput_tablet_tool_unref(tool->handle);
	libinput_tablet_tool_set_user_data(tool->handle, NULL);
	wl_list_remove(&tool->link);
	free(tool);
}

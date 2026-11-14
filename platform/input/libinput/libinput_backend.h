#ifndef PLATFORM_INPUT_LIBINPUT_LIBINPUT_BACKEND_H
#define PLATFORM_INPUT_LIBINPUT_LIBINPUT_BACKEND_H

#include <libinput.h>
#include <wayland-server-core.h>
#include "platform/backend/backend.h"
#include "platform/session/session.h"

struct nx_input_device;

struct nx_backend *nx_libinput_backend_create(struct nx_session *session);
struct libinput_device *nx_libinput_get_device_handle(
	struct nx_input_device *dev);
bool nx_input_device_is_libinput(struct nx_input_device *device);

#endif

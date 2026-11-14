#include <assert.h>
#include <string.h>
#include <wayland-server-protocol.h>
#include "platform/buffer/buffer_interface.h"
#include "platform/log/log.h"
#include "platform/buffer/buffer.h"

// Registered resource interfaces
static struct wl_array buffer_resource_interfaces = {0};

void nx_buffer_register_resource_interface(
		const struct nx_buffer_resource_interface *iface) {
	assert(iface);
	assert(iface->is_instance);
	assert(iface->from_resource);

	const struct nx_buffer_resource_interface **iface_ptr;
	wl_array_for_each(iface_ptr, &buffer_resource_interfaces) {
		if (*iface_ptr == iface) {
			nx_log(NX_DEBUG, "nx_resource_buffer_interface %s has already"
					"been registered", iface->name);
			return;
		}
	}

	iface_ptr = wl_array_add(&buffer_resource_interfaces, sizeof(iface));
	*iface_ptr = iface;
}

static const struct nx_buffer_resource_interface *get_buffer_resource_iface(
		struct wl_resource *resource) {
	struct nx_buffer_resource_interface **iface_ptr;
	wl_array_for_each(iface_ptr, &buffer_resource_interfaces) {
		if ((*iface_ptr)->is_instance(resource)) {
			return *iface_ptr;
		}
	}

	return NULL;
}

struct nx_buffer *nx_buffer_try_from_resource(struct wl_resource *resource) {
	if (strcmp(wl_resource_get_class(resource), wl_buffer_interface.name) != 0) {
		return NULL;
	}

	const struct nx_buffer_resource_interface *iface =
			get_buffer_resource_iface(resource);
	if (!iface) {
		nx_log(NX_ERROR, "Unknown buffer type");
		return NULL;
	}

	struct nx_buffer *buffer = iface->from_resource(resource);
	if (!buffer) {
		nx_log(NX_ERROR, "Failed to create %s buffer", iface->name);
		return NULL;
	}

	return nx_buffer_lock(buffer);
}

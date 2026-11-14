#ifndef PLATFORM_BUFFER_LINUX_DMABUF_REQUESTS_H
#define PLATFORM_BUFFER_LINUX_DMABUF_REQUESTS_H

#include <wayland-server-core.h>
#include "platform/buffer/linux_dmabuf.h"

struct nx_linux_dmabuf_v1 *nx_linux_dmabuf_from_resource(struct wl_resource *resource);

void nx_linux_dmabuf_create_params(struct wl_client *client,
	struct wl_resource *linux_dmabuf_resource, uint32_t params_id);

void nx_linux_dmabuf_get_surface_feedback(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource);

void nx_linux_dmabuf_surfaces_destroy(struct nx_linux_dmabuf_v1 *linux_dmabuf);

bool nx_linux_dmabuf_check_planes(struct wl_resource *params_resource,
	const struct nx_dmabuf_attributes *attribs);
bool nx_linux_dmabuf_check_plane_bounds(struct wl_resource *params_resource,
	const struct nx_dmabuf_attributes *attribs);

extern const struct nx_buffer_resource_interface nx_dmabuf_v1_buffer_resource_interface;

/* Owns attribs FDs on success */
bool nx_dmabuf_v1_buffer_create(struct wl_resource *params_resource,
	uint32_t buffer_id, const struct nx_dmabuf_attributes *attribs);

#endif

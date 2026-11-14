#ifndef PLATFORM_SURFACE_COMPOSITOR_REGION_H
#define PLATFORM_SURFACE_COMPOSITOR_REGION_H

#include <stdint.h>

struct wl_client;

struct wl_resource *region_create(struct wl_client *client,
	uint32_t version, uint32_t id);

#endif

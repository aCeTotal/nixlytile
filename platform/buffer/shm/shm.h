#ifndef PLATFORM_BUFFER_SHM_SHM_H
#define PLATFORM_BUFFER_SHM_SHM_H

#include <wayland-server-core.h>

struct nx_renderer;

struct nx_shm *nx_shm_create_with_renderer(struct wl_display *display,
	uint32_t version, struct nx_renderer *renderer);

#endif

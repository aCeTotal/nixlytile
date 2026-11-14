#ifndef PLATFORM_BUFFER_SHM_POOL_H
#define PLATFORM_BUFFER_SHM_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

/* Buffers are not thread-safe */
struct nx_shm {
	struct wl_global *global;
	uint32_t *formats; // enum wl_shm_format
	size_t formats_len;
	struct wl_listener display_destroy;
};

extern const struct nx_buffer_resource_interface nx_shm_buffer_resource_interface;

/* wl_shm.create_pool handler */
void nx_shm_create_pool(struct wl_client *client,
	struct wl_resource *shm_resource, uint32_t id, int fd, int32_t size);

#endif

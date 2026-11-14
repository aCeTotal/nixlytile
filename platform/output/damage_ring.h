#ifndef PLATFORM_OUTPUT_DAMAGE_RING_H
#define PLATFORM_OUTPUT_DAMAGE_RING_H

#include <pixman.h>
#include <wayland-server-core.h>

struct nx_box;
struct nx_buffer;

// Damage history per swapchain buffer.
struct nx_damage_ring {
	pixman_region32_t current; // since the previous buffer

	struct {
		struct wl_list buffers; // nx_damage_ring_buffer.link
	};
};

void nx_damage_ring_init(struct nx_damage_ring *ring);
void nx_damage_ring_finish(struct nx_damage_ring *ring);
void nx_damage_ring_add(struct nx_damage_ring *ring,
	const pixman_region32_t *damage);
void nx_damage_ring_add_box(struct nx_damage_ring *ring,
	const struct nx_box *box);
void nx_damage_ring_add_whole(struct nx_damage_ring *ring);
// Damage to repaint into buffer.
void nx_damage_ring_rotate_buffer(struct nx_damage_ring *ring,
	struct nx_buffer *buffer, pixman_region32_t *damage);

#endif

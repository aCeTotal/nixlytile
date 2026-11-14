#ifndef PLATFORM_CAPTURE_SCREENCOPY_FRAME_H
#define PLATFORM_CAPTURE_SCREENCOPY_FRAME_H

#include <pixman.h>
#include <stdbool.h>
#include <wayland-server-core.h>
#include "platform/buffer/buffer.h"
#include "platform/geometry/box.h"

// Damage tracked per client.
struct screencopy_client {
	int ref;
	struct wl_list damages; // screencopy_damage.link
};

struct screencopy_frame {
	struct wl_resource *resource;
	struct screencopy_client *client;

	uint32_t shm_format, dmabuf_format; // DRM format codes
	struct nx_box box;
	int shm_stride;

	bool overlay_cursor, cursor_locked;

	bool with_damage;

	enum nx_buffer_cap buffer_cap;
	struct nx_buffer *buffer;

	struct nx_output *output;

	struct wl_listener output_commit;
	struct wl_listener output_destroy;
};

// NULL when out of memory.
pixman_region32_t *screencopy_client_damage(struct screencopy_client *client,
	struct nx_output *output);
void screencopy_client_unref(struct screencopy_client *client);

bool screencopy_copy_shm(struct screencopy_frame *frame,
	struct nx_buffer *src_buffer);
bool screencopy_copy_dmabuf(struct screencopy_frame *frame,
	struct nx_buffer *src_buffer);

// NULL after posting OOM.
struct screencopy_frame *screencopy_frame_create(
	struct wl_resource *manager_resource, uint32_t id);
void screencopy_frame_start(struct screencopy_frame *frame,
	struct screencopy_client *client);
void screencopy_frame_fail(struct screencopy_frame *frame);

#endif

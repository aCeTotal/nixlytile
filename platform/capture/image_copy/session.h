#ifndef PLATFORM_CAPTURE_IMAGE_COPY_SESSION_H
#define PLATFORM_CAPTURE_IMAGE_COPY_SESSION_H

#include <pixman.h>
#include <time.h>
#include <wayland-server-protocol.h>
#include <wayland-protocols/ext-image-copy-capture-v1-enum.h>
#include "platform/capture/source/capture_source.h"

struct nx_buffer;
struct nx_renderer;

struct image_copy_session {
	struct wl_resource *resource;
	struct capture_source *source;
	struct image_copy_frame *frame;

	pixman_region32_t damage;

	struct wl_listener source_destroy;
	struct wl_listener source_constraints_update;
	struct wl_listener source_frame;
};

struct image_copy_frame {
	struct wl_resource *resource;
	struct image_copy_session *session;
	bool capturing;
	struct nx_buffer *buffer;
	pixman_region32_t buffer_damage;
};

void image_copy_session_create(struct wl_resource *parent_resource,
	uint32_t new_id, struct capture_source *source, uint32_t options);
void image_copy_cursor_session_create(struct wl_resource *manager_resource,
	uint32_t new_id, struct wl_resource *source_resource,
	struct wl_resource *pointer_resource);

// NULL session fails the frame.
void image_copy_frame_create(struct wl_resource *session_resource,
	struct image_copy_session *session, uint32_t new_id);
// Sends ready; destroys the frame.
void image_copy_frame_ready(struct image_copy_frame *frame,
	enum wl_output_transform transform,
	const struct timespec *presentation_time);
// Sends failed; destroys the frame.
void image_copy_frame_fail(struct image_copy_frame *frame,
	enum ext_image_copy_capture_frame_v1_failure_reason reason);
// Fails the frame on error.
bool image_copy_frame_copy_buffer(struct image_copy_frame *frame,
	struct nx_buffer *src, struct nx_renderer *renderer);

#endif

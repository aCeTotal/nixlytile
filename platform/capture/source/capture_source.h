#ifndef PLATFORM_CAPTURE_SOURCE_CAPTURE_SOURCE_H
#define PLATFORM_CAPTURE_SOURCE_CAPTURE_SOURCE_H

#include <pixman.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include "platform/render/drm_format_set.h"

struct image_copy_frame;
struct nx_renderer;
struct nx_seat;
struct nx_swapchain;

// Constraint changes emit constraints_update.
struct capture_source {
	const struct capture_source_impl *impl;
	struct wl_list resources; // wl_resource_get_link()

	uint32_t width, height;

	uint32_t *shm_formats;
	size_t shm_formats_len;

	dev_t dmabuf_device;
	struct nx_drm_format_set dmabuf_formats;

	struct {
		struct wl_signal constraints_update;
		struct wl_signal frame; // struct capture_source_frame_event
		struct wl_signal destroy;
	} events;
};

struct capture_source_frame_event {
	const pixman_region32_t *damage;
};

struct capture_cursor_source {
	struct capture_source base;

	bool entered;
	int32_t x, y;
	struct {
		int32_t x, y;
	} hotspot;

	struct {
		struct wl_signal update;
	} events;
};

struct capture_source_impl {
	void (*start)(struct capture_source *source, bool with_cursors);
	void (*stop)(struct capture_source *source);
	void (*request_frame)(struct capture_source *source, bool schedule_frame);
	void (*copy_frame)(struct capture_source *source,
		struct image_copy_frame *dst_frame,
		struct capture_source_frame_event *frame_event);
	struct capture_cursor_source *(*get_pointer_cursor)(
		struct capture_source *source, struct nx_seat *seat);
};

// NULL for an inert resource.
struct capture_source *capture_source_from_resource(
	struct wl_resource *resource);
void capture_source_init(struct capture_source *source,
	const struct capture_source_impl *impl);
void capture_source_finish(struct capture_source *source);
// NULL source: inert resource.
void capture_source_create_resource(struct capture_source *source,
	struct wl_client *client, uint32_t new_id);
void capture_source_set_constraints_from_swapchain(
	struct capture_source *source, struct nx_swapchain *swapchain,
	struct nx_renderer *renderer);

void capture_cursor_source_init(struct capture_cursor_source *source,
	const struct capture_source_impl *impl);
void capture_cursor_source_finish(struct capture_cursor_source *source);

#endif

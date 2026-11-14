#ifndef PLATFORM_BUFFER_LINUX_DMABUF_FEEDBACK_H
#define PLATFORM_BUFFER_LINUX_DMABUF_FEEDBACK_H

#include <wayland-server-core.h>
#include "platform/buffer/linux_dmabuf.h"

/* Format table plus per-tranche indices */
struct nx_linux_dmabuf_feedback_v1_compiled;

struct nx_linux_dmabuf_feedback_v1_compiled *nx_linux_dmabuf_feedback_compile(
	const struct nx_linux_dmabuf_feedback_v1 *feedback);
void nx_linux_dmabuf_feedback_destroy(
	struct nx_linux_dmabuf_feedback_v1_compiled *feedback);
void nx_linux_dmabuf_feedback_send(
	const struct nx_linux_dmabuf_feedback_v1_compiled *feedback,
	struct wl_resource *resource);

/* Feedback object for parent's client */
struct wl_resource *nx_linux_dmabuf_feedback_resource_create(
	struct wl_resource *parent, uint32_t id, wl_resource_destroy_func_t destroy);

#endif

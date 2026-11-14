#ifndef PLATFORM_BUFFER_LINUX_DMABUF_H
#define PLATFORM_BUFFER_LINUX_DMABUF_H

#include <stdint.h>
#include <sys/stat.h>
#include <wayland-server-core.h>
#include "platform/buffer/buffer.h"
#include "platform/render/dmabuf.h"
#include "platform/render/drm_format_set.h"

struct nx_surface;

struct nx_linux_dmabuf_feedback_v1 {
	dev_t main_device;
	struct wl_array tranches; // nx_linux_dmabuf_feedback_v1_tranche
};

struct nx_linux_dmabuf_feedback_v1_tranche {
	dev_t target_device;
	uint32_t flags; // enum zwp_linux_dmabuf_feedback_v1_tranche_flags bits
	struct nx_drm_format_set formats;
};

struct nx_linux_dmabuf_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct nx_linux_dmabuf_feedback_v1_compiled *default_feedback;
		struct nx_drm_format_set default_formats; // for pre-feedback clients
		struct wl_list surfaces; // nx_linux_dmabuf_v1_surface.link

		int main_device_fd; // validates client FDs, or -1

		struct wl_listener display_destroy;
	};
};

/* Feedback derived from renderer */
struct nx_linux_dmabuf_v1 *nx_linux_dmabuf_v1_create_with_renderer(struct wl_display *display,
	uint32_t version, struct nx_renderer *renderer);

/* NULL feedback restores the default */
bool nx_linux_dmabuf_v1_set_surface_feedback(
	struct nx_linux_dmabuf_v1 *linux_dmabuf, struct nx_surface *surface,
	const struct nx_linux_dmabuf_feedback_v1 *feedback);

void nx_linux_dmabuf_feedback_v1_finish(struct nx_linux_dmabuf_feedback_v1 *feedback);

struct nx_linux_dmabuf_feedback_v1_init_options {
	struct nx_renderer *main_renderer;
	struct nx_output *scanout_primary_output; // primary plane scanout, or NULL
};

/* Caller finishes the feedback */
bool nx_linux_dmabuf_feedback_v1_init_with_options(struct nx_linux_dmabuf_feedback_v1 *feedback,
	const struct nx_linux_dmabuf_feedback_v1_init_options *options);

#endif

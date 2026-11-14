#include <assert.h>
#include <stdbool.h>
#include "platform/buffer/shm/shm.h"
#include "platform/render/renderer.h"
#include "platform/render/renderer_interface.h"

void nx_renderer_init(struct nx_renderer *renderer,
		const struct nx_renderer_impl *impl, uint32_t render_buffer_caps) {
	assert(impl->begin_buffer_pass);
	assert(impl->get_texture_formats);
	assert(impl->get_render_formats);
	assert(impl->texture_from_buffer);
	assert(impl->destroy);
	assert(render_buffer_caps != 0);

	*renderer = (struct nx_renderer){
		.impl = impl,
		.render_buffer_caps = render_buffer_caps,
	};

	wl_signal_init(&renderer->events.destroy);
	wl_signal_init(&renderer->events.lost);
}

void nx_renderer_destroy(struct nx_renderer *r) {
	if (!r) {
		return;
	}

	wl_signal_emit_mutable(&r->events.destroy, r);

	assert(wl_list_empty(&r->events.destroy.listener_list));
	assert(wl_list_empty(&r->events.lost.listener_list));

	r->impl->destroy(r);
}

const struct nx_drm_format_set *nx_renderer_get_texture_formats(
		struct nx_renderer *r, uint32_t buffer_caps) {
	return r->impl->get_texture_formats(r, buffer_caps);
}

const struct nx_drm_format_set *nx_renderer_get_render_formats(
		struct nx_renderer *r) {
	return r->impl->get_render_formats(r);
}

bool nx_renderer_init_wl_shm(struct nx_renderer *r,
		struct wl_display *wl_display) {
	return nx_shm_create_with_renderer(wl_display, 2, r) != NULL;
}

int nx_renderer_get_drm_fd(struct nx_renderer *r) {
	if (!r->impl->get_drm_fd) {
		return -1;
	}
	return r->impl->get_drm_fd(r);
}

struct nx_render_pass *nx_renderer_begin_buffer_pass(struct nx_renderer *renderer,
		struct nx_buffer *buffer, const struct nx_buffer_pass_options *options) {
	struct nx_buffer_pass_options default_options = {0};
	if (!options) {
		options = &default_options;
	}

	return renderer->impl->begin_buffer_pass(renderer, buffer, options);
}

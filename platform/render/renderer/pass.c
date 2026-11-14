#include <assert.h>
#include "platform/render/renderer_interface.h"

void nx_render_pass_init(struct nx_render_pass *render_pass,
		const struct nx_render_pass_impl *impl) {
	assert(impl->submit && impl->add_texture && impl->add_rect);
	*render_pass = (struct nx_render_pass){
		.impl = impl,
	};
}

bool nx_render_pass_submit(struct nx_render_pass *render_pass) {
	return render_pass->impl->submit(render_pass);
}

void nx_render_pass_add_texture(struct nx_render_pass *render_pass,
		const struct nx_render_texture_options *options) {
	// Never sample outside the texture
	if (!nx_fbox_empty(&options->src_box)) {
		const struct nx_fbox *box = &options->src_box;
		assert(box->x >= 0 && box->y >= 0 &&
			(uint32_t)(box->x + box->width) <= options->texture->width &&
			(uint32_t)(box->y + box->height) <= options->texture->height);
	}

	render_pass->impl->add_texture(render_pass, options);
}

void nx_render_pass_add_rect(struct nx_render_pass *render_pass,
		const struct nx_render_rect_options *options) {
	assert(options->box.width >= 0 && options->box.height >= 0);
	render_pass->impl->add_rect(render_pass, options);
}

void nx_render_texture_options_get_src_box(const struct nx_render_texture_options *options,
		struct nx_fbox *box) {
	*box = options->src_box;
	if (nx_fbox_empty(box)) {
		*box = (struct nx_fbox){
			.width = options->texture->width,
			.height = options->texture->height,
		};
	}
}

void nx_render_texture_options_get_dst_box(const struct nx_render_texture_options *options,
		struct nx_box *box) {
	*box = options->dst_box;
	if (nx_box_empty(box)) {
		box->width = options->texture->width;
		box->height = options->texture->height;
	}
}

float nx_render_texture_options_get_alpha(const struct nx_render_texture_options *options) {
	if (options->alpha == NULL) {
		return 1;
	}
	return *options->alpha;
}

void nx_render_rect_options_get_box(const struct nx_render_rect_options *options,
		const struct nx_buffer *buffer, struct nx_box *box) {
	if (nx_box_empty(&options->box)) {
		*box = (struct nx_box){
			.width = buffer->width,
			.height = buffer->height,
		};

		return;
	}

	*box = options->box;
}

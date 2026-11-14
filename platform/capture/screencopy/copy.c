#include <assert.h>
#include "platform/capture/screencopy/frame.h"
#include "platform/log/log.h"
#include "platform/output/output.h"
#include "platform/render/renderer.h"

bool screencopy_copy_shm(struct screencopy_frame *frame,
		struct nx_buffer *src_buffer) {
	struct nx_renderer *renderer = frame->output->renderer;
	assert(renderer);

	void *data;
	uint32_t format;
	size_t stride;
	if (!nx_buffer_begin_data_ptr_access(frame->buffer,
			NX_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		return false;
	}

	bool ok = false;

	struct nx_texture *texture = nx_texture_from_buffer(renderer, src_buffer);
	if (!texture) {
		nx_log(NX_DEBUG, "Failed to grab a texture from a buffer during shm screencopy");
		goto out;
	}

	ok = nx_texture_read_pixels(texture, &(struct nx_texture_read_pixels_options) {
		.data = data,
		.format = format,
		.stride = stride,
		.src_box = frame->box,
	});

	nx_texture_destroy(texture);

out:
	nx_buffer_end_data_ptr_access(frame->buffer);

	if (!ok) {
		nx_log(NX_DEBUG, "Failed to copy to destination during shm screencopy");
	}

	return ok;
}

bool screencopy_copy_dmabuf(struct screencopy_frame *frame,
		struct nx_buffer *src_buffer) {
	struct nx_buffer *dst_buffer = frame->buffer;
	struct nx_renderer *renderer = frame->output->renderer;
	assert(renderer);

	struct nx_texture *src_tex =
		nx_texture_from_buffer(renderer, src_buffer);
	if (src_tex == NULL) {
		nx_log(NX_DEBUG, "Failed to grab a texture from a buffer during dma screencopy");
		return false;
	}

	bool ok = false;

	struct nx_render_pass *pass =
		nx_renderer_begin_buffer_pass(renderer, dst_buffer, NULL);
	if (!pass) {
		goto out;
	}

	nx_render_pass_add_texture(pass, &(struct nx_render_texture_options) {
		.texture = src_tex,
		.blend_mode = NX_RENDER_BLEND_MODE_NONE,
		.dst_box = (struct nx_box){
			.width = dst_buffer->width,
			.height = dst_buffer->height,
		},
		.src_box = (struct nx_fbox){
			.x = frame->box.x,
			.y = frame->box.y,
			.width = frame->box.width,
			.height = frame->box.height,
		},
	});

	ok = nx_render_pass_submit(pass);

out:
	nx_texture_destroy(src_tex);

	if (!ok) {
		nx_log(NX_DEBUG, "Failed to render to destination during dma screencopy");
	}

	return ok;
}

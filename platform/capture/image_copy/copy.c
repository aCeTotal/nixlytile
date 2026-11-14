#include "platform/buffer/buffer.h"
#include "platform/capture/image_copy/session.h"
#include "platform/render/renderer.h"

static bool copy_dmabuf(struct nx_buffer *dst,
		struct nx_buffer *src, struct nx_renderer *renderer,
		const pixman_region32_t *clip) {
	struct nx_texture *texture = nx_texture_from_buffer(renderer, src);
	if (texture == NULL) {
		return false;
	}

	bool ok = false;
	struct nx_render_pass *pass =
		nx_renderer_begin_buffer_pass(renderer, dst, NULL);
	if (!pass) {
		goto out;
	}

	nx_render_pass_add_texture(pass, &(struct nx_render_texture_options) {
		.texture = texture,
		.clip = clip,
		.blend_mode = NX_RENDER_BLEND_MODE_NONE,
	});

	ok = nx_render_pass_submit(pass);

out:
	nx_texture_destroy(texture);
	return ok;
}

static bool copy_shm(void *data, uint32_t format, size_t stride,
		struct nx_buffer *src, struct nx_renderer *renderer) {
	struct nx_texture *texture = nx_texture_from_buffer(renderer, src);
	if (!texture) {
		return false;
	}

	bool ok = nx_texture_read_pixels(texture, &(struct nx_texture_read_pixels_options){
		.data = data,
		.format = format,
		.stride = stride,
	});

	nx_texture_destroy(texture);

	return ok;
}

static bool frame_copy_to_dmabuf(struct image_copy_frame *frame,
		struct nx_buffer *src, struct nx_renderer *renderer) {
	if (frame->session->source->dmabuf_formats.len == 0) {
		image_copy_frame_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
		return false;
	}
	if (!copy_dmabuf(frame->buffer, src, renderer, &frame->buffer_damage)) {
		image_copy_frame_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
		return false;
	}
	return true;
}

static bool frame_copy_to_shm(struct image_copy_frame *frame,
		struct nx_buffer *src, struct nx_renderer *renderer) {
	void *data;
	uint32_t format;
	size_t stride;
	if (!nx_buffer_begin_data_ptr_access(frame->buffer,
			NX_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
		image_copy_frame_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
		return false;
	}

	bool constrained = frame->session->source->shm_formats_len == 0;
	bool ok = !constrained && copy_shm(data, format, stride, src, renderer);
	// Failing unlocks the buffer.
	nx_buffer_end_data_ptr_access(frame->buffer);
	if (ok) {
		return true;
	}

	image_copy_frame_fail(frame, constrained ?
		EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS :
		EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
	return false;
}

bool image_copy_frame_copy_buffer(struct image_copy_frame *frame,
		struct nx_buffer *src, struct nx_renderer *renderer) {
	struct nx_buffer *dst = frame->buffer;
	if (src->width != dst->width || src->height != dst->height) {
		image_copy_frame_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
		return false;
	}

	struct nx_dmabuf_attributes dmabuf;
	if (nx_buffer_get_dmabuf(dst, &dmabuf)) {
		return frame_copy_to_dmabuf(frame, src, renderer);
	}
	return frame_copy_to_shm(frame, src, renderer);
}

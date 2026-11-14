#include <drm_fourcc.h>
#include <stdlib.h>
#include <xcb/render.h>
#include "platform/buffer/buffer.h"
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

#define CURSOR_DEPTH 32

void xwm_find_cursor_format(struct nx_xwm *xwm) {
	xcb_render_query_pict_formats_cookie_t cookie =
		xcb_render_query_pict_formats(xwm->xcb_conn);
	xcb_render_query_pict_formats_reply_t *reply =
		xcb_render_query_pict_formats_reply(xwm->xcb_conn, cookie, NULL);
	if (!reply) {
		nx_log(NX_ERROR, "Did not get any reply from xcb_render_query_pict_formats");
		return;
	}

	xcb_render_pictforminfo_iterator_t iter =
		xcb_render_query_pict_formats_formats_iterator(reply);
	for (; iter.rem > 0; xcb_render_pictforminfo_next(&iter)) {
		if (iter.data->depth == CURSOR_DEPTH) {
			xwm->render_format_id = iter.data->id;
			free(reply);
			return;
		}
	}

	nx_log(NX_DEBUG, "No 32 bit render format");
	free(reply);
}

void xwm_set_cursor(struct nx_xwm *xwm, struct nx_buffer *buffer,
		int32_t hotspot_x, int32_t hotspot_y) {
	if (!xwm->render_format_id) {
		nx_log(NX_ERROR, "Cannot set xwm cursor: no render format available");
		return;
	}
	if (xwm->cursor) {
		xcb_free_cursor(xwm->xcb_conn, xwm->cursor);
	}

	void *pixels = NULL;
	uint32_t format = DRM_FORMAT_INVALID;
	size_t stride = 0;
	if (!nx_buffer_begin_data_ptr_access(buffer, NX_BUFFER_DATA_PTR_ACCESS_READ,
			&pixels, &format, &stride)) {
		return;
	}
	if (format != DRM_FORMAT_ARGB8888) {
		nx_buffer_end_data_ptr_access(buffer);
		nx_log(NX_ERROR, "Only ARGB8888 is supported for Xwayland cursors");
		return;
	}

	xcb_connection_t *conn = xwm->xcb_conn;
	xcb_pixmap_t pix = xcb_generate_id(conn);
	xcb_create_pixmap(conn, CURSOR_DEPTH, pix, xwm->screen->root,
		buffer->width, buffer->height);

	xcb_render_picture_t pic = xcb_generate_id(conn);
	xcb_render_create_picture(conn, pic, pix, xwm->render_format_id, 0, 0);

	xcb_gcontext_t gc = xcb_generate_id(conn);
	xcb_create_gc(conn, gc, pix, 0, NULL);
	xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pix, gc,
		buffer->width, buffer->height, 0, 0, 0, CURSOR_DEPTH,
		stride * buffer->height, pixels);
	xcb_free_gc(conn, gc);

	nx_buffer_end_data_ptr_access(buffer);

	xwm->cursor = xcb_generate_id(conn);
	xcb_render_create_cursor(conn, xwm->cursor, pic, hotspot_x, hotspot_y);
	xcb_free_pixmap(conn, pix);
	xcb_render_free_picture(conn, pic);

	xcb_change_window_attributes(conn, xwm->screen->root, XCB_CW_CURSOR,
		&xwm->cursor);
	xwm_schedule_flush(xwm);
}

/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include "platform/buffer/buffer.h"
#include "platform/input/xcursor/cursor.h"

static struct nx_xcursor_image *image_create(const struct xcursor_image *src) {
	struct nx_xcursor_image *image = calloc(1, sizeof(*image));
	if (image == NULL) {
		return NULL;
	}
	image->width = src->width;
	image->height = src->height;
	image->hotspot_x = src->xhot;
	image->hotspot_y = src->yhot;
	image->delay = src->delay;

	size_t stride = src->width * sizeof(uint32_t);
	size_t size = stride * src->height;
	image->buffer = malloc(size);
	if (image->buffer == NULL) {
		goto err_image;
	}
	memcpy(image->buffer, src->pixels, size);

	image->readonly_buffer = readonly_data_buffer_create(DRM_FORMAT_ARGB8888,
		stride, src->width, src->height, image->buffer);
	if (image->readonly_buffer == NULL) {
		goto err_buffer;
	}
	return image;

err_buffer:
	free(image->buffer);
err_image:
	free(image);
	return NULL;
}

struct nx_xcursor *xcursor_create_from_images(
		const struct xcursor_images *images) {
	struct nx_xcursor *cursor = calloc(1, sizeof(*cursor));
	if (cursor == NULL) {
		return NULL;
	}
	cursor->images = calloc(images->nimage, sizeof(cursor->images[0]));
	if (cursor->images == NULL) {
		free(cursor);
		return NULL;
	}
	cursor->name = strdup(images->name);

	for (int i = 0; i < images->nimage; i++) {
		struct nx_xcursor_image *image = image_create(images->images[i]);
		if (image == NULL) {
			break;
		}
		cursor->images[i] = image;
		cursor->image_count++;
	}

	if (cursor->image_count == 0) {
		free(cursor->name);
		free(cursor->images);
		free(cursor);
		return NULL;
	}
	return cursor;
}

void xcursor_destroy(struct nx_xcursor *cursor) {
	for (size_t i = 0; i < cursor->image_count; i++) {
		readonly_data_buffer_drop(cursor->images[i]->readonly_buffer);
		free(cursor->images[i]->buffer);
		free(cursor->images[i]);
	}
	free(cursor->images);
	free(cursor->name);
	free(cursor);
}

struct nx_buffer *nx_xcursor_image_get_buffer(struct nx_xcursor_image *image) {
	return &image->readonly_buffer->base;
}

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

#ifndef PLATFORM_INPUT_XCURSOR_XCURSOR_H
#define PLATFORM_INPUT_XCURSOR_XCURSOR_H

#include <stdint.h>

// Packed ARGB8888 pixels.
struct nx_xcursor_image {
	uint32_t width;
	uint32_t height;
	uint32_t hotspot_x;
	uint32_t hotspot_y;
	uint32_t delay; // ms until the next frame
	uint8_t *buffer;
	struct nx_readonly_data_buffer *readonly_buffer;
};

// Several images when animated.
struct nx_xcursor {
	unsigned int image_count;
	struct nx_xcursor_image **images;
	char *name;
};

struct nx_xcursor_theme {
	unsigned int cursor_count;
	struct nx_xcursor **cursors;
	char *name;
};

// Falls back to embedded theme.
struct nx_xcursor_theme *nx_xcursor_theme_load(const char *name, int size);
void nx_xcursor_theme_destroy(struct nx_xcursor_theme *theme);
struct nx_xcursor *nx_xcursor_theme_get_cursor(
		struct nx_xcursor_theme *theme, const char *name);

struct nx_buffer *nx_xcursor_image_get_buffer(struct nx_xcursor_image *image);

#endif

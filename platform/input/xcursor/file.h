/*
 * Copyright © 2002 Keith Packard
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

#ifndef PLATFORM_INPUT_XCURSOR_FILE_H
#define PLATFORM_INPUT_XCURSOR_FILE_H

#include <stdint.h>
#include <stdio.h>

struct xcursor_image {
	uint32_t width;
	uint32_t height;
	uint32_t xhot;
	uint32_t yhot;
	uint32_t delay; // ms until the next frame
	uint32_t *pixels;
};

// All frames of one cursor.
struct xcursor_images {
	int nimage;
	struct xcursor_image **images;
	char *name;
};

// Images nearest `size`.
struct xcursor_images *xcursor_file_load_images(FILE *file, int size);
void xcursor_images_destroy(struct xcursor_images *images);

#endif

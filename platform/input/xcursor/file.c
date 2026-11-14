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

#include <stdbool.h>
#include <stdlib.h>
#include "platform/input/xcursor/file.h"

#define XCURSOR_MAGIC 0x72756358 // "Xcur" little-endian
#define XCURSOR_FILE_HEADER_LEN (4 * 4)
#define XCURSOR_IMAGE_TYPE 0xfffd0002
#define XCURSOR_IMAGE_MAX_SIZE 0x7fff
#define XCURSOR_MAX_TOCS 0x10000
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

struct xcursor_file_toc {
	uint32_t type;
	uint32_t subtype; // nominal size for images
	uint32_t position;
};

struct xcursor_file_header {
	uint32_t ntoc;
	struct xcursor_file_toc *tocs;
};

struct xcursor_chunk_header {
	uint32_t header;
	uint32_t type;
	uint32_t subtype;
	uint32_t version;
};

static bool read_uint(FILE *file, uint32_t *u) {
	unsigned char bytes[4];
	if (fread(bytes, 1, 4, file) != 4) {
		return false;
	}
	*u = ((uint32_t)(bytes[0]) << 0) |
		((uint32_t)(bytes[1]) << 8) |
		((uint32_t)(bytes[2]) << 16) |
		((uint32_t)(bytes[3]) << 24);
	return true;
}

static bool read_uints(FILE *file, uint32_t *values[], size_t count) {
	for (size_t i = 0; i < count; i++) {
		if (!read_uint(file, values[i])) {
			return false;
		}
	}
	return true;
}

static bool read_tocs(FILE *file, struct xcursor_file_header *file_header) {
	for (uint32_t n = 0; n < file_header->ntoc; n++) {
		struct xcursor_file_toc *toc = &file_header->tocs[n];
		uint32_t *fields[] = { &toc->type, &toc->subtype, &toc->position };
		if (!read_uints(file, fields, ARRAY_LEN(fields))) {
			return false;
		}
	}
	return true;
}

// Magic, header length, version, toc count.
static struct xcursor_file_header *read_file_header(FILE *file) {
	uint32_t magic, header, version, ntoc;
	if (!read_uint(file, &magic) || magic != XCURSOR_MAGIC) {
		return NULL;
	}
	uint32_t *fields[] = { &header, &version, &ntoc };
	if (!read_uints(file, fields, ARRAY_LEN(fields))) {
		return NULL;
	}
	uint32_t skip = header - XCURSOR_FILE_HEADER_LEN;
	if (skip && fseek(file, skip, SEEK_CUR) == EOF) {
		return NULL;
	}
	if (ntoc > XCURSOR_MAX_TOCS) {
		return NULL;
	}

	struct xcursor_file_header *file_header = malloc(sizeof(*file_header) +
		ntoc * sizeof(struct xcursor_file_toc));
	if (file_header == NULL) {
		return NULL;
	}
	file_header->ntoc = ntoc;
	file_header->tocs = (struct xcursor_file_toc *)(file_header + 1);
	if (!read_tocs(file, file_header)) {
		free(file_header);
		return NULL;
	}
	return file_header;
}

// Must match its toc entry.
static bool read_chunk_header(FILE *file,
		const struct xcursor_file_toc *toc, struct xcursor_chunk_header *chunk) {
	if (fseek(file, toc->position, SEEK_SET) == EOF) {
		return false;
	}
	uint32_t *fields[] = {
		&chunk->header, &chunk->type, &chunk->subtype, &chunk->version,
	};
	if (!read_uints(file, fields, ARRAY_LEN(fields))) {
		return false;
	}
	return chunk->type == toc->type && chunk->subtype == toc->subtype;
}

static uint32_t dist(uint32_t a, uint32_t b) {
	return a > b ? a - b : b - a;
}

struct size_match {
	uint32_t size;
	int count;
};

// Nominal size closest to `size`.
static struct size_match best_size(
		const struct xcursor_file_header *file_header, uint32_t size) {
	struct size_match best = {0};
	for (uint32_t n = 0; n < file_header->ntoc; n++) {
		const struct xcursor_file_toc *toc = &file_header->tocs[n];
		if (toc->type != XCURSOR_IMAGE_TYPE) {
			continue;
		}
		if (!best.size || dist(toc->subtype, size) < dist(best.size, size)) {
			best.size = toc->subtype;
			best.count = 1;
		} else if (toc->subtype == best.size) {
			best.count++;
		}
	}
	return best;
}

// The `count`th match, or -1.
static int find_image_toc(const struct xcursor_file_header *file_header,
		uint32_t size, int count) {
	for (uint32_t toc = 0; toc < file_header->ntoc; toc++) {
		if (file_header->tocs[toc].type != XCURSOR_IMAGE_TYPE ||
				file_header->tocs[toc].subtype != size) {
			continue;
		}
		if (!count) {
			return (int)toc;
		}
		count--;
	}
	return -1;
}

static bool image_header_valid(const struct xcursor_image *head) {
	return head->width <= XCURSOR_IMAGE_MAX_SIZE &&
		head->height <= XCURSOR_IMAGE_MAX_SIZE &&
		head->width != 0 && head->height != 0 &&
		head->xhot <= head->width && head->yhot <= head->height;
}

static struct xcursor_image *read_image(FILE *file,
		const struct xcursor_file_toc *toc) {
	struct xcursor_chunk_header chunk;
	struct xcursor_image head = {0};
	uint32_t *fields[] = {
		&head.width, &head.height, &head.xhot, &head.yhot, &head.delay,
	};
	if (!read_chunk_header(file, toc, &chunk) ||
			!read_uints(file, fields, ARRAY_LEN(fields)) ||
			!image_header_valid(&head)) {
		return NULL;
	}

	size_t npixels = (size_t)head.width * head.height;
	struct xcursor_image *image =
		malloc(sizeof(*image) + npixels * sizeof(uint32_t));
	if (image == NULL) {
		return NULL;
	}
	*image = head;
	image->pixels = (uint32_t *)(image + 1);
	for (size_t i = 0; i < npixels; i++) {
		if (!read_uint(file, &image->pixels[i])) {
			free(image);
			return NULL;
		}
	}
	return image;
}

static struct xcursor_images *images_create(int size) {
	struct xcursor_images *images =
		malloc(sizeof(*images) + size * sizeof(struct xcursor_image *));
	if (images == NULL) {
		return NULL;
	}
	images->nimage = 0;
	images->images = (struct xcursor_image **)(images + 1);
	images->name = NULL;
	return images;
}

void xcursor_images_destroy(struct xcursor_images *images) {
	for (int n = 0; n < images->nimage; n++) {
		free(images->images[n]);
	}
	free(images->name);
	free(images);
}

// Frames of the nearest size.
static struct xcursor_images *read_images(FILE *file,
		const struct xcursor_file_header *file_header, uint32_t size) {
	struct size_match best = best_size(file_header, size);
	if (!best.size) {
		return NULL;
	}
	struct xcursor_images *images = images_create(best.count);
	if (images == NULL) {
		return NULL;
	}

	for (int n = 0; n < best.count; n++) {
		int toc = find_image_toc(file_header, best.size, n);
		struct xcursor_image *image =
			toc < 0 ? NULL : read_image(file, &file_header->tocs[toc]);
		if (image == NULL) {
			xcursor_images_destroy(images);
			return NULL;
		}
		images->images[images->nimage++] = image;
	}
	return images;
}

struct xcursor_images *xcursor_file_load_images(FILE *file, int size) {
	if (size < 0) {
		return NULL;
	}
	struct xcursor_file_header *file_header = read_file_header(file);
	if (file_header == NULL) {
		return NULL;
	}
	struct xcursor_images *images = read_images(file, file_header, (uint32_t)size);
	free(file_header);
	return images;
}

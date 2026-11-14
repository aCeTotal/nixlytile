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

#include <stdlib.h>
#include <string.h>
#include "platform/input/xcursor/cursor.h"
#include "platform/input/xcursor/default_cursor_data.h"
#include "platform/input/xcursor/theme_search.h"
#include "platform/log/log.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

// Older X11 names as fallback.
static const struct {
	const char *name;
	const char *legacy;
} legacy_names[] = {
	{ "default", "left_ptr" },
	{ "text", "xterm" },
	{ "pointer", "hand1" },
	{ "wait", "watch" },
	{ "all-scroll", "grabbing" },
	{ "sw-resize", "bottom_left_corner" },
	{ "se-resize", "bottom_right_corner" },
	{ "s-resize", "bottom_side" },
	{ "w-resize", "left_side" },
	{ "e-resize", "right_side" },
	{ "nw-resize", "top_left_corner" },
	{ "ne-resize", "top_right_corner" },
	{ "n-resize", "top_side" },
};

static struct nx_xcursor *find_cursor(struct nx_xcursor_theme *theme,
		const char *name) {
	for (unsigned int i = 0; i < theme->cursor_count; i++) {
		if (strcmp(name, theme->cursors[i]->name) == 0) {
			return theme->cursors[i];
		}
	}
	return NULL;
}

static const char *legacy_name(const char *name) {
	for (size_t i = 0; i < ARRAY_LEN(legacy_names); i++) {
		if (strcmp(name, legacy_names[i].name) == 0) {
			return legacy_names[i].legacy;
		}
	}
	return NULL;
}

struct nx_xcursor *nx_xcursor_theme_get_cursor(struct nx_xcursor_theme *theme,
		const char *name) {
	struct nx_xcursor *xcursor = find_cursor(theme, name);
	if (xcursor != NULL) {
		return xcursor;
	}
	const char *fallback = legacy_name(name);
	if (fallback == NULL) {
		return NULL;
	}
	return find_cursor(theme, fallback);
}

static void theme_append(struct nx_xcursor_theme *theme,
		struct nx_xcursor *cursor) {
	struct nx_xcursor **cursors = realloc(theme->cursors,
		(theme->cursor_count + 1) * sizeof(theme->cursors[0]));
	if (cursors == NULL) {
		xcursor_destroy(cursor);
		return;
	}
	theme->cursors = cursors;
	theme->cursors[theme->cursor_count++] = cursor;
}

// First theme providing it wins.
static void add_cursor(struct nx_xcursor_theme *theme,
		const struct xcursor_images *images) {
	if (find_cursor(theme, images->name) != NULL) {
		return;
	}
	struct nx_xcursor *cursor = xcursor_create_from_images(images);
	if (cursor != NULL) {
		theme_append(theme, cursor);
	}
}

static void load_callback(struct xcursor_images *images, void *data) {
	add_cursor(data, images);
	xcursor_images_destroy(images);
}

static struct nx_xcursor *cursor_from_metadata(
		const struct cursor_metadata *metadata) {
	struct xcursor_image image = {
		.width = metadata->width,
		.height = metadata->height,
		.xhot = metadata->hotspot_x,
		.yhot = metadata->hotspot_y,
		.pixels = (uint32_t *)&cursor_data[metadata->offset], // only read
	};
	struct xcursor_image *frames[] = { &image };
	struct xcursor_images images = {
		.nimage = 1,
		.images = frames,
		.name = metadata->name,
	};
	return xcursor_create_from_images(&images);
}

static void load_default_theme(struct nx_xcursor_theme *theme) {
	free(theme->name);
	theme->name = strdup("default");

	theme->cursor_count = 0;
	theme->cursors = malloc(ARRAY_LEN(cursor_metadata) * sizeof(*theme->cursors));
	if (theme->cursors == NULL) {
		return;
	}
	for (size_t i = 0; i < ARRAY_LEN(cursor_metadata); i++) {
		theme->cursors[i] = cursor_from_metadata(&cursor_metadata[i]);
		if (theme->cursors[i] == NULL) {
			break;
		}
		theme->cursor_count++;
	}
}

struct nx_xcursor_theme *nx_xcursor_theme_load(const char *name, int size) {
	struct nx_xcursor_theme *theme = calloc(1, sizeof(*theme));
	if (theme == NULL) {
		return NULL;
	}
	if (name == NULL) {
		name = "default";
	}
	theme->name = strdup(name);
	if (theme->name == NULL) {
		free(theme);
		return NULL;
	}

	xcursor_load_theme(name, size, load_callback, theme);
	if (theme->cursor_count == 0) {
		load_default_theme(theme);
	}

	nx_log(NX_DEBUG, "Loaded cursor theme '%s' at size %d (%d available cursors)",
		theme->name, size, theme->cursor_count);
	return theme;
}

void nx_xcursor_theme_destroy(struct nx_xcursor_theme *theme) {
	for (unsigned int i = 0; i < theme->cursor_count; i++) {
		xcursor_destroy(theme->cursors[i]);
	}
	free(theme->name);
	free(theme->cursors);
	free(theme);
}

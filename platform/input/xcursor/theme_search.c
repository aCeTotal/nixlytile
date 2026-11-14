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

#undef _POSIX_C_SOURCE
#define _DEFAULT_SOURCE // d_type in struct dirent
#include <dirent.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "platform/input/xcursor/file.h"
#include "platform/input/xcursor/theme_inherits.h"
#include "platform/input/xcursor/theme_search.h"

#ifndef ICONDIR
#define ICONDIR "/usr/X11R6/lib/X11/icons"
#endif

#define XCURSOR_SEARCH_PATH "~/.icons:/usr/share/icons:/usr/share/pixmaps:" \
	"~/.cursors:/usr/share/cursors/xorg-x11:" ICONDIR
#define XDG_DATA_HOME_FALLBACK "~/.local/share"
#define CURSORDIR "/icons"

struct theme_loader {
	int size;
	xcursor_load_callback callback;
	void *user_data;
};

// Visited themes, for cycle detection.
struct xcursor_nodelist {
	size_t nodelen;
	const char *node;
	struct xcursor_nodelist *next;
};

// Colon-separated directories; caller frees.
static char *library_path(void) {
	const char *env_var = getenv("XCURSOR_PATH");
	if (env_var) {
		return strdup(env_var);
	}

	env_var = getenv("XDG_DATA_HOME");
	if (!env_var || env_var[0] != '/') {
		env_var = XDG_DATA_HOME_FALLBACK;
	}
	const char *suffix = CURSORDIR ":" XCURSOR_SEARCH_PATH;
	size_t path_size = strlen(env_var) + strlen(suffix) + 1;
	char *path = malloc(path_size);
	if (!path) {
		return NULL;
	}
	snprintf(path, path_size, "%s%s", env_var, suffix);
	return path;
}

static size_t segment_len(const char *s) {
	const char *colon = strchr(s, ':');
	return colon ? (size_t)(colon - s) : strlen(s);
}

// Joins the first path segments.
static char *build_theme_dir(const char *dir, const char *theme) {
	size_t dirlen = segment_len(dir);
	size_t themelen = segment_len(theme);
	const char *home = "";
	const char *homesep = "";
	if (*dir == '~') {
		home = getenv("HOME");
		homesep = "/";
		dir++;
		dirlen--;
	}
	if (!home) {
		return NULL;
	}

	size_t full_size = 1 + strlen(home) + 1 + dirlen + 1 + themelen + 1;
	char *full = malloc(full_size);
	if (!full) {
		return NULL;
	}
	snprintf(full, full_size, "%s%s%.*s/%.*s", home, homesep,
		(int)dirlen, dir, (int)themelen, theme);
	return full;
}

static char *build_fullname(const char *dir, const char *subdir,
		const char *file) {
	size_t full_size = strlen(dir) + 1 + strlen(subdir) + 1 + strlen(file) + 1;
	char *full = malloc(full_size);
	if (!full) {
		return NULL;
	}
	snprintf(full, full_size, "%s/%s/%s", dir, subdir, file);
	return full;
}

static const char *next_path(const char *path) {
	const char *colon = strchr(path, ':');
	return colon ? colon + 1 : NULL;
}

static bool is_cursor_file(const struct dirent *ent) {
	return ent->d_type == DT_UNKNOWN || ent->d_type == DT_REG ||
		ent->d_type == DT_LNK;
}

static void load_cursor_file(const char *dir, const char *name,
		const struct theme_loader *loader) {
	char *full = build_fullname(dir, "", name);
	if (!full) {
		return;
	}
	FILE *f = fopen(full, "r");
	free(full);
	if (!f) {
		return;
	}

	struct xcursor_images *images = xcursor_file_load_images(f, loader->size);
	if (images) {
		images->name = strdup(name);
		loader->callback(images, loader->user_data);
	}
	fclose(f);
}

static void load_all_cursors_from_dir(const char *path,
		const struct theme_loader *loader) {
	DIR *dir = opendir(path);
	if (!dir) {
		return;
	}
	for (struct dirent *ent = readdir(dir); ent; ent = readdir(dir)) {
		if (is_cursor_file(ent)) {
			load_cursor_file(path, ent->d_name, loader);
		}
	}
	closedir(dir);
}

static void load_theme_cursors(const char *dir,
		const struct theme_loader *loader) {
	char *full = build_fullname(dir, "cursors", "");
	if (!full) {
		return;
	}
	load_all_cursors_from_dir(full, loader);
	free(full);
}

static char *theme_dir_inherits(const char *dir) {
	char *full = build_fullname(dir, "", "index.theme");
	char *inherits = xcursor_theme_inherits(full);
	free(full);
	return inherits;
}

static bool nodelist_contains(const struct xcursor_nodelist *nodelist,
		const char *s, size_t ss) {
	for (const struct xcursor_nodelist *vi = nodelist; vi && vi->node;
			vi = vi->next) {
		if (vi->nodelen == ss && !strncmp(s, vi->node, vi->nodelen)) {
			return true;
		}
	}
	return false;
}

static void load_theme_protected(const char *theme,
		const struct theme_loader *loader, struct xcursor_nodelist *visited);

// Matches the whole remaining list.
static void load_inherited_themes(const char *inherits,
		const struct theme_loader *loader, struct xcursor_nodelist *visited) {
	for (const char *i = inherits; i; i = next_path(i)) {
		if (!nodelist_contains(visited, i, strlen(i))) {
			load_theme_protected(i, loader, visited);
		}
	}
}

static void load_theme_protected(const char *theme,
		const struct theme_loader *loader, struct xcursor_nodelist *visited) {
	struct xcursor_nodelist current_node = {
		.next = visited,
		.node = theme,
		.nodelen = strlen(theme),
	};

	char *inherits = NULL;
	char *xcursor_path = library_path();
	for (const char *path = xcursor_path; path; path = next_path(path)) {
		char *dir = build_theme_dir(path, theme);
		if (!dir) {
			continue;
		}
		load_theme_cursors(dir, loader);
		if (!inherits) {
			inherits = theme_dir_inherits(dir);
		}
		free(dir);
	}

	load_inherited_themes(inherits, loader, &current_node);
	free(inherits);
	free(xcursor_path);
}

void xcursor_load_theme(const char *theme, int size,
		xcursor_load_callback load_callback, void *user_data) {
	struct theme_loader loader = {
		.size = size,
		.callback = load_callback,
		.user_data = user_data,
	};
	load_theme_protected(theme, &loader, NULL);
}

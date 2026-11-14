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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/input/xcursor/theme_inherits.h"

#define INHERITS_KEY "Inherits"

static bool is_list_delim(char c) {
	return c == ';' || c == ',' || c == ' ' || c == '\t' || c == '\n';
}

// Value after "Inherits =", or NULL.
static const char *inherits_value(const char *line) {
	if (strncmp(line, INHERITS_KEY, strlen(INHERITS_KEY))) {
		return NULL;
	}
	const char *l = line + strlen(INHERITS_KEY);
	while (*l == ' ') {
		l++;
	}
	if (*l != '=') {
		return NULL;
	}
	l++;
	while (*l == ' ') {
		l++;
	}
	return l;
}

// Theme list, colon-separated.
static char *parse_inherits(const char *l) {
	char *result = malloc(strlen(l) + 1);
	if (!result) {
		return NULL;
	}
	char *r = result;
	while (*l) {
		while (is_list_delim(*l)) {
			l++;
		}
		if (!*l) {
			break;
		}
		if (r != result) {
			*r++ = ':';
		}
		while (*l && !is_list_delim(*l)) {
			*r++ = *l++;
		}
	}
	*r = '\0';
	return result;
}

char *xcursor_theme_inherits(const char *index_path) {
	if (!index_path) {
		return NULL;
	}
	FILE *f = fopen(index_path, "r");
	if (!f) {
		return NULL;
	}

	char *line = NULL;
	size_t line_size = 0;
	char *result = NULL;
	while (getline(&line, &line_size, f) >= 0) {
		const char *value = inherits_value(line);
		if (value == NULL) {
			continue;
		}
		result = parse_inherits(value);
		break;
	}
	fclose(f);
	free(line);
	return result;
}

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "platform/log/log.h"
#include "platform/render/drm_format_set.h"

void nx_drm_format_finish(struct nx_drm_format *format) {
	if (!format) {
		return;
	}

	free(format->modifiers);
}

bool nx_drm_format_has(const struct nx_drm_format *fmt, uint64_t modifier) {
	for (size_t i = 0; i < fmt->len; ++i) {
		if (fmt->modifiers[i] == modifier) {
			return true;
		}
	}
	return false;
}

bool nx_drm_format_add(struct nx_drm_format *fmt, uint64_t modifier) {
	if (nx_drm_format_has(fmt, modifier)) {
		return true;
	}

	if (fmt->len == fmt->capacity) {
		size_t capacity = fmt->capacity ? fmt->capacity * 2 : 4;

		uint64_t *new_modifiers = realloc(fmt->modifiers, sizeof(*fmt->modifiers) * capacity);
		if (!new_modifiers) {
			nx_log_errno(NX_ERROR, "Allocation failed");
			return false;
		}

		fmt->capacity = capacity;
		fmt->modifiers = new_modifiers;
	}

	fmt->modifiers[fmt->len++] = modifier;
	return true;
}

bool nx_drm_format_copy(struct nx_drm_format *dst, const struct nx_drm_format *src) {
	assert(src->len <= src->capacity);

	uint64_t *modifiers = malloc(sizeof(*modifiers) * src->len);
	if (!modifiers) {
		return false;
	}

	memcpy(modifiers, src->modifiers, sizeof(*modifiers) * src->len);

	nx_drm_format_finish(dst);
	dst->capacity = src->len;
	dst->len = src->len;
	dst->format = src->format;
	dst->modifiers = modifiers;
	return true;
}

bool nx_drm_format_intersect(struct nx_drm_format *dst,
		const struct nx_drm_format *a, const struct nx_drm_format *b) {
	assert(a->format == b->format);

	size_t capacity = a->len < b->len ? a->len : b->len;
	uint64_t *modifiers = malloc(sizeof(*modifiers) * capacity);
	if (!modifiers) {
		return false;
	}

	struct nx_drm_format fmt = {
		.capacity = capacity,
		.len = 0,
		.modifiers = modifiers,
		.format = a->format,
	};

	for (size_t i = 0; i < a->len; i++) {
		if (nx_drm_format_has(b, a->modifiers[i])) {
			assert(fmt.len < fmt.capacity);
			fmt.modifiers[fmt.len++] = a->modifiers[i];
		}
	}

	nx_drm_format_finish(dst);
	*dst = fmt;
	return true;
}

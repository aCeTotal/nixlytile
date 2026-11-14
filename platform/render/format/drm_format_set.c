#include <assert.h>
#include <drm_fourcc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "platform/log/log.h"
#include "platform/render/drm_format_set.h"

void nx_drm_format_set_finish(struct nx_drm_format_set *set) {
	for (size_t i = 0; i < set->len; ++i) {
		nx_drm_format_finish(&set->formats[i]);
	}
	free(set->formats);

	set->len = 0;
	set->capacity = 0;
	set->formats = NULL;
}

static struct nx_drm_format *format_set_get(const struct nx_drm_format_set *set,
		uint32_t format) {
	for (size_t i = 0; i < set->len; ++i) {
		if (set->formats[i].format == format) {
			return &set->formats[i];
		}
	}

	return NULL;
}

const struct nx_drm_format *nx_drm_format_set_get(
		const struct nx_drm_format_set *set, uint32_t format) {
	return format_set_get(set, format);
}

bool nx_drm_format_set_has(const struct nx_drm_format_set *set,
		uint32_t format, uint64_t modifier) {
	const struct nx_drm_format *fmt = nx_drm_format_set_get(set, format);
	if (!fmt) {
		return false;
	}
	return nx_drm_format_has(fmt, modifier);
}

static void format_init(struct nx_drm_format *fmt, uint32_t format) {
	*fmt = (struct nx_drm_format){
		.format = format,
	};
}

bool nx_drm_format_set_add(struct nx_drm_format_set *set, uint32_t format,
		uint64_t modifier) {
	assert(format != DRM_FORMAT_INVALID);

	struct nx_drm_format *existing = format_set_get(set, format);
	if (existing) {
		return nx_drm_format_add(existing, modifier);
	}

	struct nx_drm_format fmt;
	format_init(&fmt, format);
	if (!nx_drm_format_add(&fmt, modifier)) {
		nx_drm_format_finish(&fmt);
		return false;
	}

	if (set->len == set->capacity) {
		size_t capacity = set->capacity ? set->capacity * 2 : 4;

		struct nx_drm_format *fmts = realloc(set->formats, sizeof(*fmts) * capacity);
		if (!fmts) {
			nx_log_errno(NX_ERROR, "Allocation failed");
			nx_drm_format_finish(&fmt);
			return false;
		}

		set->capacity = capacity;
		set->formats = fmts;
	}

	set->formats[set->len++] = fmt;
	return true;
}

bool nx_drm_format_set_remove(struct nx_drm_format_set *set, uint32_t format,
		uint64_t modifier) {
	struct nx_drm_format *fmt = format_set_get(set, format);
	if (fmt == NULL) {
		return false;
	}

	for (size_t idx = 0; idx < fmt->len; idx++) {
		if (fmt->modifiers[idx] == modifier) {
			memmove(&fmt->modifiers[idx], &fmt->modifiers[idx+1], (fmt->len - idx - 1) * sizeof(fmt->modifiers[0]));
			fmt->len--;
			return true;
		}
	}
	return false;
}

bool nx_drm_format_set_copy(struct nx_drm_format_set *dst, const struct nx_drm_format_set *src) {
	struct nx_drm_format *formats = malloc(src->len * sizeof(formats[0]));
	if (formats == NULL) {
		return false;
	}

	struct nx_drm_format_set out = {
		.len = 0,
		.capacity = src->len,
		.formats = formats,
	};

	for (size_t i = 0; i < src->len; i++) {
		out.formats[out.len] = (struct nx_drm_format){0};
		if (!nx_drm_format_copy(&out.formats[out.len], &src->formats[i])) {
			nx_drm_format_set_finish(&out);
			return false;
		}

		out.len++;
	}

	*dst = out;

	return true;
}

bool nx_drm_format_set_intersect(struct nx_drm_format_set *dst,
		const struct nx_drm_format_set *a, const struct nx_drm_format_set *b) {
	struct nx_drm_format_set out = {0};
	out.capacity = a->len < b->len ? a->len : b->len;
	out.formats = malloc(sizeof(*out.formats) * out.capacity);
	if (out.formats == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return false;
	}

	for (size_t i = 0; i < a->len; i++) {
		const struct nx_drm_format *match = format_set_get(b, a->formats[i].format);
		if (match == NULL) {
			continue;
		}

		struct nx_drm_format *fmt = &out.formats[out.len];
		*fmt = (struct nx_drm_format){0};
		if (!nx_drm_format_intersect(fmt, &a->formats[i], match)) {
			nx_drm_format_set_finish(&out);
			return false;
		}

		if (fmt->len == 0) {
			nx_drm_format_finish(fmt);
			continue;
		}
		out.len++;
	}

	if (out.len == 0) {
		nx_drm_format_set_finish(&out);
		return false;
	}

	nx_drm_format_set_finish(dst);
	*dst = out;
	return true;
}

static bool format_set_add_format(struct nx_drm_format_set *dst,
		const struct nx_drm_format *format) {
	for (size_t i = 0; i < format->len; i++) {
		if (!nx_drm_format_set_add(dst, format->format, format->modifiers[i])) {
			nx_log_errno(NX_ERROR, "Adding format/modifier to set failed");
			return false;
		}
	}
	return true;
}

static bool format_set_extend(struct nx_drm_format_set *dst,
		const struct nx_drm_format_set *src) {
	for (size_t i = 0; i < src->len; i++) {
		if (!format_set_add_format(dst, &src->formats[i])) {
			return false;
		}
	}
	return true;
}

bool nx_drm_format_set_union(struct nx_drm_format_set *dst,
		const struct nx_drm_format_set *a, const struct nx_drm_format_set *b) {
	struct nx_drm_format_set out = {0};
	out.capacity = a->len + b->len;
	out.formats = malloc(sizeof(*out.formats) * out.capacity);
	if (out.formats == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return false;
	}

	if (!format_set_extend(&out, a) || !format_set_extend(&out, b)) {
		nx_drm_format_set_finish(&out);
		return false;
	}

	nx_drm_format_set_finish(dst);
	*dst = out;

	return true;
}

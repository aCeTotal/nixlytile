#ifndef PLATFORM_RENDER_DRM_FORMAT_SET_H
#define PLATFORM_RENDER_DRM_FORMAT_SET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A fourcc and its modifiers */
struct nx_drm_format {
	uint32_t format;
	size_t len;
	size_t capacity;
	uint64_t *modifiers;
};

void nx_drm_format_finish(struct nx_drm_format *format);
bool nx_drm_format_has(const struct nx_drm_format *fmt, uint64_t modifier);
bool nx_drm_format_add(struct nx_drm_format *fmt, uint64_t modifier);
bool nx_drm_format_copy(struct nx_drm_format *dst, const struct nx_drm_format *src);

/* Same format only; replaces dst */
bool nx_drm_format_intersect(struct nx_drm_format *dst,
	const struct nx_drm_format *a, const struct nx_drm_format *b);

/* INVALID modifier means implicit support */
struct nx_drm_format_set {
	size_t len;
	size_t capacity;
	struct nx_drm_format *formats;
};

/* Leaves the set empty */
void nx_drm_format_set_finish(struct nx_drm_format_set *set);

bool nx_drm_format_set_copy(struct nx_drm_format_set *dst, const struct nx_drm_format_set *src);

const struct nx_drm_format *nx_drm_format_set_get(
	const struct nx_drm_format_set *set, uint32_t format);

bool nx_drm_format_set_remove(struct nx_drm_format_set *set, uint32_t format,
	uint64_t modifier);

bool nx_drm_format_set_has(const struct nx_drm_format_set *set,
	uint32_t format, uint64_t modifier);

bool nx_drm_format_set_add(struct nx_drm_format_set *set, uint32_t format,
	uint64_t modifier);

/* False if empty or failed */
bool nx_drm_format_set_intersect(struct nx_drm_format_set *dst,
	const struct nx_drm_format_set *a, const struct nx_drm_format_set *b);

bool nx_drm_format_set_union(struct nx_drm_format_set *dst,
	const struct nx_drm_format_set *a, const struct nx_drm_format_set *b);

#endif

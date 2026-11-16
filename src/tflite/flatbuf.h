#ifndef TFLITE_FLATBUF_H
#define TFLITE_FLATBUF_H

#include <stdint.h>
#include <string.h>

/* Bounds-checked read-only flatbuffer access. */
typedef struct {
	const uint8_t *base;
	size_t len;
} Flatbuf;

static inline uint32_t
fb_u32(const Flatbuf *fb, size_t at)
{
	uint32_t v = 0;

	if (at + 4 <= fb->len)
		memcpy(&v, fb->base + at, 4);
	return v;
}

static inline uint16_t
fb_u16(const Flatbuf *fb, size_t at)
{
	uint16_t v = 0;

	if (at + 2 <= fb->len)
		memcpy(&v, fb->base + at, 2);
	return v;
}

/* Field position, 0 when absent. */
static inline size_t
fb_field(const Flatbuf *fb, size_t t, unsigned id)
{
	size_t vt = t - (size_t)(int32_t)fb_u32(fb, t);
	unsigned slot = 4 + 2 * id;
	uint16_t off;

	if (vt >= fb->len || slot + 2 > fb_u16(fb, vt))
		return 0;
	off = fb_u16(fb, vt + slot);
	return off ? t + off : 0;
}

static inline int32_t
fb_int(const Flatbuf *fb, size_t t, unsigned id, int32_t dflt)
{
	size_t at = fb_field(fb, t, id);

	return at ? (int32_t)fb_u32(fb, at) : dflt;
}

static inline uint8_t
fb_byte(const Flatbuf *fb, size_t t, unsigned id, uint8_t dflt)
{
	size_t at = fb_field(fb, t, id);

	return at && at < fb->len ? fb->base[at] : dflt;
}

/* Offset target, 0 when absent. */
static inline size_t
fb_ref(const Flatbuf *fb, size_t t, unsigned id)
{
	size_t at = fb_field(fb, t, id);

	return at ? at + fb_u32(fb, at) : 0;
}

static inline uint32_t
fb_veclen(const Flatbuf *fb, size_t vec)
{
	return vec ? fb_u32(fb, vec) : 0;
}

static inline size_t
fb_vecdata(size_t vec)
{
	return vec + 4;
}

/* Element i of table vector. */
static inline size_t
fb_vectable(const Flatbuf *fb, size_t vec, uint32_t i)
{
	size_t at = fb_vecdata(vec) + 4 * (size_t)i;

	return at + fb_u32(fb, at);
}

#endif

#include <stdlib.h>
#include <turbojpeg.h>

#include "decode.h"

int
decoder_open(Decoder *d, int w, int h)
{
	d->tj = tj3Init(TJINIT_DECOMPRESS);
	d->buf = malloc((size_t)w * (size_t)h * 3);
	return d->tj && d->buf ? 0 : -1;
}

void
decoder_close(Decoder *d)
{
	tj3Destroy(d->tj);
	free(d->buf);
}

static Region
clamp_region(const Frame *f, const Region *r)
{
	int x = r->x < 0 ? 0 : r->x, y = r->y < 0 ? 0 : r->y;
	int x1 = r->x + r->w > f->w ? f->w : r->x + r->w;
	int y1 = r->y + r->h > f->h ? f->h : r->y + r->h;

	return (Region){ x / r->shrink, y / r->shrink, (x1 - x) / r->shrink, (y1 - y) / r->shrink,
		r->shrink };
}

/* Region only, iMCU-aligned left edge. */
static const Image *
decode_jpeg(Decoder *d, const Frame *f, Region s)
{
	tjscalingfactor factor = { 1, s.shrink };
	int mcu, x1 = s.x + s.w;

	if (tj3DecompressHeader(d->tj, f->data, f->len) < 0 ||
	    tj3SetScalingFactor(d->tj, factor) < 0)
		return NULL;
	mcu = TJSCALED(tjMCUWidth[tj3Get(d->tj, TJPARAM_SUBSAMP)], factor);
	s.x -= s.x % mcu;
	s.w = x1 - s.x;
	if (tj3SetCroppingRegion(d->tj, (tjregion){ s.x, s.y, s.w, s.h }) < 0 ||
	    tj3Decompress8(d->tj, f->data, f->len, d->buf, s.w * 3, TJPF_RGB) < 0)
		return NULL;
	d->img = (Image){ d->buf, s.w, s.h, (float)(s.x * s.shrink), (float)(s.y * s.shrink),
		(float)s.shrink };
	return &d->img;
}

static uint8_t
clip(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* BT.601 limited range, fixed point. */
static void
yuyv_pixel(const uint8_t *p, int odd, uint8_t *rgb)
{
	int y = (p[odd ? 2 : 0] - 16) * 298, u = p[1] - 128, v = p[3] - 128;

	rgb[0] = clip((y + 409 * v + 128) >> 8);
	rgb[1] = clip((y - 100 * u - 208 * v + 128) >> 8);
	rgb[2] = clip((y + 516 * u + 128) >> 8);
}

static const Image *
decode_yuyv(Decoder *d, const Frame *f, Region s)
{
	size_t stride = (size_t)f->w * 2;

	if (f->len < stride * (size_t)f->h)
		return NULL;
	for (int i = 0; i < s.w * s.h; i++) {
		int fx = (s.x + i % s.w) * s.shrink, fy = (s.y + i / s.w) * s.shrink;

		yuyv_pixel(f->data + (size_t)fy * stride + (size_t)(fx & ~1) * 2, fx & 1,
			d->buf + (size_t)i * 3);
	}
	d->img = (Image){ d->buf, s.w, s.h, (float)(s.x * s.shrink), (float)(s.y * s.shrink),
		(float)s.shrink };
	return &d->img;
}

const Image *
decode(Decoder *d, const Frame *f, const Region *r)
{
	Region s = clamp_region(f, r);

	if (s.w <= 0 || s.h <= 0)
		return NULL;
	return f->jpeg ? decode_jpeg(d, f, s) : decode_yuyv(d, f, s);
}

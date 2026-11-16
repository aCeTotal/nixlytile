#include <math.h>

#include "track.h"

/* Image-space origin and steps. */
typedef struct {
	float x, y, ux, uy, vx, vy;
} Affine;

static Affine
affine(const Image *img, const Rect *r, const Crop *c)
{
	float cs = cosf(r->angle) / img->scale, sn = sinf(r->angle) / img->scale;
	float su = r->w / (float)c->size * (c->flip ? -1.0f : 1.0f), sv = r->h / (float)c->size;
	float u0 = (0.5f - (float)c->size * 0.5f) * su, v0 = (0.5f - (float)c->size * 0.5f) * sv;

	return (Affine){
		(r->cx - img->ox) / img->scale - 0.5f + u0 * cs - v0 * sn,
		(r->cy - img->oy) / img->scale - 0.5f + u0 * sn + v0 * cs,
		su * cs, su * sn, -sv * sn, sv * cs,
	};
}

/* Black outside, like MediaPipe. */
static float
texel(const Image *img, int x, int y, int ch)
{
	if (x < 0 || y < 0 || x >= img->w || y >= img->h)
		return 0.0f;
	return img->rgb[((size_t)y * (size_t)img->w + (size_t)x) * 3 + (size_t)ch];
}

/* Top-left pixel, bilinear weights. */
typedef struct {
	int x, y;
	float w[4];
} Taps;

static void
bilinear_edge(const Image *img, const Taps *t, float *rgb)
{
	for (int ch = 0; ch < 3; ch++)
		rgb[ch] = texel(img, t->x, t->y, ch) * t->w[0] + texel(img, t->x + 1, t->y, ch) * t->w[1] +
			texel(img, t->x, t->y + 1, ch) * t->w[2] +
			texel(img, t->x + 1, t->y + 1, ch) * t->w[3];
}

/* floorf without a libm call. */
static float
floor_fast(float v)
{
	float t = (float)(int)v;

	return t > v ? t - 1.0f : t;
}

static void
bilinear(const Image *img, float x, float y, float *rgb)
{
	float fx = floor_fast(x), fy = floor_fast(y), ax = x - fx, ay = y - fy;
	Taps t = { (int)fx, (int)fy, { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay } };
	size_t row = (size_t)img->w * 3;
	const uint8_t *p;

	if (t.x < 0 || t.y < 0 || t.x + 1 >= img->w || t.y + 1 >= img->h) {
		bilinear_edge(img, &t, rgb);
		return;
	}
	p = img->rgb + (size_t)t.y * row + (size_t)t.x * 3;
	for (int ch = 0; ch < 3; ch++)
		rgb[ch] = p[ch] * t.w[0] + p[3 + ch] * t.w[1] + p[row + ch] * t.w[2] +
			p[row + 3 + ch] * t.w[3];
}

static void
sample_row(const Image *img, const Affine *a, const Crop *c, int v)
{
	float scale = (c->hi - c->lo) / 255.0f;
	float *dst = c->dst + (size_t)v * (size_t)c->size * 3;

	for (int u = 0; u < c->size; u++, dst += 3) {
		bilinear(img, a->x + (float)u * a->ux + (float)v * a->vx,
			a->y + (float)u * a->uy + (float)v * a->vy, dst);
		for (int ch = 0; ch < 3; ch++)
			dst[ch] = c->lo + dst[ch] * scale;
	}
}

void
sample_rect(const Image *img, const Rect *r, const Crop *c)
{
	Affine a = affine(img, r, c);

	for (int v = 0; v < c->size; v++)
		sample_row(img, &a, c, v);
}

/* Model coordinates to frame. */
Point
rect_point(const Rect *r, const Crop *c, Point model)
{
	float lx = (model.x / (float)c->size - 0.5f) * r->w * (c->flip ? -1.0f : 1.0f);
	float ly = (model.y / (float)c->size - 0.5f) * r->h;
	float cs = cosf(r->angle), sn = sinf(r->angle);

	return (Point){ r->cx + lx * cs - ly * sn, r->cy + lx * sn + ly * cs };
}

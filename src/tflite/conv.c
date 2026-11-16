#include "window.h"

#define TILE_PIXELS 6

typedef struct {
	Window win;
	const Op *op;
	float *dst;
	int ow, npix, cout;
} Conv;

/* Pixel by channel accumulator tile. */
typedef struct {
	size_t base[TILE_PIXELS];
	v8 acc[TILE_PIXELS][2];
	int p, y, x, nb;
} Tile;

/* Offsets; tail repeats last pixel. */
NET_HOT void
tile_bases(const Conv *c, Tile *t)
{
	for (int m = 0; m < TILE_PIXELS; m++) {
		t->base[m] = window_base(&c->win, t->y, t->x);
		if (t->p + m + 1 < c->npix && ++t->x == c->ow) {
			t->x = 0;
			t->y++;
		}
	}
}

NET_HOT void
tile_tap(const Conv *c, Tile *t, int tap)
{
	const float *w = c->op->weights + (size_t)tap * c->win.c * c->op->coutp + t->nb;
	const float *src = c->win.data + c->win.tap[tap];
	v8 w0, w1;

	for (int i = 0; i < c->win.c; i++, w += c->op->coutp) {
		load8(&w0, w);
		load8(&w1, w + 8);
		for (int m = 0; m < TILE_PIXELS; m++) {
			float x = src[t->base[m] + i];

			t->acc[m][0] += w0 * x;
			t->acc[m][1] += w1 * x;
		}
	}
}

NET_HOT void
tile_store(const Conv *c, Tile *t)
{
	int rows = c->npix - t->p < TILE_PIXELS ? c->npix - t->p : TILE_PIXELS;
	int n = c->cout - t->nb;

	for (int m = 0; m < rows && n >= NET_CONV_BLOCK; m++) {
		float *dst = c->dst + (size_t)(t->p + m) * c->cout + t->nb;

		finish8(c->op, &t->acc[m][0], t->nb);
		finish8(c->op, &t->acc[m][1], t->nb + 8);
		store8(dst, &t->acc[m][0]);
		store8(dst + 8, &t->acc[m][1]);
	}
	for (int m = 0; m < rows && n < NET_CONV_BLOCK; m++) {
		float *dst = c->dst + (size_t)(t->p + m) * c->cout + t->nb;
		float row[NET_CONV_BLOCK];

		store8(row, &t->acc[m][0]);
		store8(row + 8, &t->acc[m][1]);
		memcpy(dst, row, (size_t)n * sizeof(*dst));
		finish(c->op, dst, t->nb, n);
	}
}

NET_HOT void
tile_run(const Conv *c, Tile *t)
{
	v8 b0, b1;

	load8(&b0, c->op->bias + t->nb);
	load8(&b1, c->op->bias + t->nb + 8);
	for (int m = 0; m < TILE_PIXELS; m++) {
		t->acc[m][0] = b0;
		t->acc[m][1] = b1;
	}
	for (int tap = 0; tap < c->win.ntap; tap++)
		tile_tap(c, t, tap);
	tile_store(c, t);
}

NET_SIMD void
kernel_conv(const Net *net, const Op *op)
{
	const Tensor *out = &net->t[op->out];
	Conv c = { window_input(net, op), op, out->data, out->dim[2], out->dim[1] * out->dim[2],
		out->dim[3] };
	Tile t = {0};

	for (t.p = 0; t.p < c.npix; t.p += TILE_PIXELS) {
		tile_bases(&c, &t);
		for (t.nb = 0; t.nb < c.cout; t.nb += NET_CONV_BLOCK)
			tile_run(&c, &t);
	}
}

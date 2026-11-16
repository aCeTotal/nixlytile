#include "window.h"

#define RUN_PIXELS 4

typedef struct {
	Window win;
	const Op *op;
	float *dst;
	int ow;
} Depthwise;

/* Neighbours form independent chains. */
typedef struct {
	const float *src[RUN_PIXELS];
	float *dst[RUN_PIXELS];
	int n;
} Run;

NET_HOT void
block(const Depthwise *d, const Run *r, int cb)
{
	v8 acc[RUN_PIXELS], x, w;

	for (int m = 0; m < RUN_PIXELS; m++)
		load8(&acc[m], d->op->bias + cb);
	for (int t = 0; t < d->win.ntap; t++) {
		load8(&w, d->op->weights + (size_t)t * d->win.c + cb);
		for (int m = 0; m < RUN_PIXELS; m++) {
			load8(&x, r->src[m] + d->win.tap[t] + cb);
			acc[m] += x * w;
		}
	}
	for (int m = 0; m < r->n; m++) {
		finish8(d->op, &acc[m], cb);
		store8(r->dst[m] + cb, &acc[m]);
	}
}

NET_HOT float
single(const Depthwise *d, const float *src, int ch)
{
	float acc = d->op->bias[ch];

	for (int t = 0; t < d->win.ntap; t++)
		acc += src[d->win.tap[t] + ch] * d->op->weights[(size_t)t * d->win.c + ch];
	return acc;
}

NET_HOT void
tail(const Depthwise *d, const Run *r, int full)
{
	for (int m = 0; m < r->n; m++) {
		for (int ch = full; ch < d->win.c; ch++)
			r->dst[m][ch] = single(d, r->src[m], ch);
		finish(d->op, r->dst[m] + full, full, d->win.c - full);
	}
}

NET_HOT void
run(const Depthwise *d, int y, int x)
{
	Run r = { .n = d->ow - x < RUN_PIXELS ? d->ow - x : RUN_PIXELS };
	int full = d->win.c & ~7;

	for (int m = 0; m < RUN_PIXELS; m++) {
		int px = m < r.n ? x + m : x + r.n - 1;

		r.src[m] = d->win.data + window_base(&d->win, y, px);
		r.dst[m] = d->dst + ((size_t)y * d->ow + px) * d->win.c;
	}
	for (int cb = 0; cb < full; cb += 8)
		block(d, &r, cb);
	tail(d, &r, full);
}

NET_SIMD void
kernel_dwconv(const Net *net, const Op *op)
{
	const Tensor *out = &net->t[op->out];
	Depthwise d = { window_input(net, op), op, out->data, out->dim[2] };

	for (int y = 0; y < out->dim[1]; y++)
		for (int x = 0; x < d.ow; x += RUN_PIXELS)
			run(&d, y, x);
}

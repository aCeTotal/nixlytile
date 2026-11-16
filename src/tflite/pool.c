#include <float.h>

#include "net.h"

typedef struct {
	const Op *op;
	const float *src;
	float *dst;
	int ih, iw, c, ow;
} Pool;

NET_HOT void
max_tap(float *restrict acc, const float *restrict src, int c)
{
	for (int i = 0; i < c; i++)
		acc[i] = src[i] > acc[i] ? src[i] : acc[i];
}

/* Padding never wins the max. */
NET_HOT void
max_pixel(const Pool *p, int y, int x)
{
	int by = y * p->op->stride_h - p->op->pad_top, bx = x * p->op->stride_w - p->op->pad_left;
	int y0 = by < 0 ? 0 : by, y1 = by + p->op->kh < p->ih ? by + p->op->kh : p->ih;
	int x0 = bx < 0 ? 0 : bx, x1 = bx + p->op->kw < p->iw ? bx + p->op->kw : p->iw;
	float *acc = p->dst + ((size_t)y * p->ow + x) * p->c;

	for (int i = 0; i < p->c; i++)
		acc[i] = -FLT_MAX;
	for (int sy = y0; sy < y1; sy++)
		for (int sx = x0; sx < x1; sx++)
			max_tap(acc, p->src + ((size_t)sy * p->iw + sx) * p->c, p->c);
	activate(acc, (size_t)p->c, p->op->act);
}

NET_SIMD void
kernel_maxpool(const Net *net, const Op *op)
{
	const Tensor *in = &net->t[op->in[0]], *out = &net->t[op->out];
	Pool p = { op, in->data, out->data, in->dim[1], in->dim[2], in->dim[3], out->dim[2] };

	for (int y = 0; y < out->dim[1]; y++)
		for (int x = 0; x < p.ow; x++)
			max_pixel(&p, y, x);
}

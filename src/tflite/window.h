#ifndef TFLITE_WINDOW_H
#define TFLITE_WINDOW_H

#include <string.h>

#include "net.h"

typedef float v8 __attribute__((vector_size(32)));
typedef int m8 __attribute__((vector_size(32)));

/* Padded input; taps stay inside. */
typedef struct {
	const float *data;
	int w, c;
	int stride_h, stride_w;
	int ntap;
	size_t tap[NET_MAX_TAPS];
} Window;

Window window_input(const Net *net, const Op *op);

NET_HOT size_t
window_base(const Window *win, int y, int x)
{
	return ((size_t)y * win->stride_h * win->w + (size_t)x * win->stride_w) * win->c;
}

NET_HOT void
load8(v8 *v, const float *p)
{
	memcpy(v, p, sizeof(*v));
}

NET_HOT void
store8(float *p, const v8 *v)
{
	memcpy(p, v, sizeof(*v));
}

NET_HOT void
select8(v8 *x, const m8 *keep, const v8 *other)
{
	*x = (v8)(((m8)*x & *keep) | ((m8)*other & ~*keep));
}

/* Fused activation and PReLU. */
NET_HOT void
finish8(const Op *op, v8 *x, int c0)
{
	const v8 zero = {0}, six = zero + 6.0f;
	v8 slope;
	m8 keep;

	if (op->act != ACT_NONE)
		*x = (v8)((m8)*x & (*x > zero));
	if (op->act == ACT_RELU6) {
		keep = *x < six;
		select8(x, &keep, &six);
	}
	if (!op->alpha)
		return;
	load8(&slope, op->alpha + c0);
	slope *= *x;
	keep = *x > zero;
	select8(x, &keep, &slope);
}

/* Scalar finish8 for tails. */
NET_HOT void
finish(const Op *op, float *dst, int c0, int n)
{
	activate(dst, (size_t)n, op->act);
	if (!op->alpha)
		return;
	for (int i = 0; i < n; i++)
		dst[i] = dst[i] > 0.0f ? dst[i] : dst[i] * op->alpha[c0 + i];
}

#endif

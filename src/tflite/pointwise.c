#include "window.h"

/* PReLU slopes come via op->alpha. */
typedef struct {
	const Op *op;
	const float *a, *b;
	float *dst;
	int c, bc;
} Pointwise;

/* Eight floats, zero past n. */
NET_HOT void
load_upto(v8 *v, const float *p, int n)
{
	const v8 zero = {0};

	if (n >= 8) {
		load8(v, p);
		return;
	}
	*v = zero;
	if (n > 0)
		memcpy(v, p, (size_t)n * sizeof(float));
}

/* Second operand may be narrower. */
NET_HOT void
add_pixel(const Pointwise *e, size_t p)
{
	const float *a = e->a + p * (size_t)e->c, *b = e->b + p * (size_t)e->bc;
	float *dst = e->dst + p * (size_t)e->c;
	int cb = 0;
	v8 x, y;

	for (; cb + 8 <= e->c; cb += 8) {
		load8(&x, a + cb);
		load_upto(&y, b + cb, e->bc - cb);
		x += y;
		finish8(e->op, &x, cb);
		store8(dst + cb, &x);
	}
	for (int ch = cb; ch < e->c; ch++)
		dst[ch] = a[ch] + (ch < e->bc ? b[ch] : 0.0f);
	finish(e->op, dst + cb, cb, e->c - cb);
}

NET_HOT void
prelu_pixel(const Pointwise *e, size_t p)
{
	const float *a = e->a + p * (size_t)e->c;
	float *dst = e->dst + p * (size_t)e->c;
	int cb = 0;
	v8 x;

	for (; cb + 8 <= e->c; cb += 8) {
		load8(&x, a + cb);
		finish8(e->op, &x, cb);
		store8(dst + cb, &x);
	}
	memcpy(dst + cb, a + cb, (size_t)(e->c - cb) * sizeof(float));
	finish(e->op, dst + cb, cb, e->c - cb);
}

static Pointwise
pointwise(const Net *net, const Op *op)
{
	const Tensor *out = &net->t[op->out];
	const Tensor *b = op->nin > 1 ? &net->t[op->in[1]] : out;

	return (Pointwise){ op, net->t[op->in[0]].data, b->data, out->data, out->dim[3], b->dim[3] };
}

NET_SIMD void
kernel_add(const Net *net, const Op *op)
{
	Pointwise e = pointwise(net, op);
	size_t pixels = net->t[op->out].count / (size_t)e.c;

	for (size_t p = 0; p < pixels; p++)
		add_pixel(&e, p);
}

NET_SIMD void
kernel_prelu(const Net *net, const Op *op)
{
	Pointwise e = pointwise(net, op);
	size_t pixels = net->t[op->out].count / (size_t)e.c;

	for (size_t p = 0; p < pixels; p++)
		prelu_pixel(&e, p);
}

NET_SIMD void
kernel_relu(const Net *net, const Op *op)
{
	const float *restrict src = net->t[op->in[0]].data;
	float *restrict dst = net->t[op->out].data;

	for (size_t i = 0; i < net->t[op->out].count; i++)
		dst[i] = src[i] > 0.0f ? src[i] : 0.0f;
}

#include "window.h"

static void
pad_copy(const Net *net, const Op *op, const Tensor *in)
{
	size_t row = (size_t)in->dim[2] * in->dim[3];

	memset(net->scratch, 0, (size_t)op->pad_h * op->pad_w * in->dim[3] * sizeof(float));
	for (int y = 0; y < in->dim[1]; y++)
		memcpy(net->scratch + ((size_t)(y + op->pad_top) * op->pad_w + op->pad_left) * in->dim[3],
			in->data + (size_t)y * row, row * sizeof(float));
}

Window
window_input(const Net *net, const Op *op)
{
	const Tensor *in = &net->t[op->in[0]];
	Window win = { in->data, in->dim[2], in->dim[3], op->stride_h, op->stride_w,
		op->kh * op->kw, {0} };

	if (op->pad_h) {
		pad_copy(net, op, in);
		win.data = net->scratch;
		win.w = op->pad_w;
	}
	for (int t = 0; t < win.ntap; t++)
		win.tap[t] = ((size_t)(t / op->kw) * win.w + (size_t)(t % op->kw)) * win.c;
	return win;
}

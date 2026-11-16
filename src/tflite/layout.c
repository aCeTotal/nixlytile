#include <string.h>

#include "net.h"

void
kernel_reshape(const Net *net, const Op *op)
{
	memcpy(net->t[op->out].data, net->t[op->in[0]].data, net->t[op->out].count * sizeof(float));
}

/* Rows into zeroed padded output. */
void
kernel_pad(const Net *net, const Op *op)
{
	const Tensor *in = &net->t[op->in[0]], *out = &net->t[op->out];
	size_t rows = in->count / (size_t)in->dim[3];
	size_t row = (size_t)in->dim[3] * sizeof(float);

	memset(out->data, 0, out->count * sizeof(float));
	for (size_t r = 0; r < rows; r++) {
		int w = (int)(r % (size_t)in->dim[2]);
		int h = (int)(r / (size_t)in->dim[2] % (size_t)in->dim[1]);
		int n = (int)(r / ((size_t)in->dim[2] * (size_t)in->dim[1]));
		size_t at = (((size_t)(n + op->pads[0][0]) * out->dim[1] + (size_t)(h + op->pads[1][0])) *
			out->dim[2] + (size_t)(w + op->pads[2][0])) * out->dim[3] + (size_t)op->pads[3][0];

		memcpy(out->data + at, in->data + r * (size_t)in->dim[3], row);
	}
}

static size_t
inner(const Tensor *t, int axis)
{
	size_t n = 1;

	for (int d = axis; d < NET_RANK; d++)
		n *= (size_t)t->dim[d];
	return n;
}

void
kernel_concat(const Net *net, const Op *op)
{
	const Tensor *out = &net->t[op->out];
	size_t chunk = inner(out, op->axis);
	size_t outer = out->count / chunk;
	float *dst = out->data;

	for (size_t o = 0; o < outer; o++)
		for (int i = 0; i < op->nin; i++) {
			const Tensor *in = &net->t[op->in[i]];
			size_t n = inner(in, op->axis);

			memcpy(dst, in->data + o * n, n * sizeof(float));
			dst += n;
		}
}

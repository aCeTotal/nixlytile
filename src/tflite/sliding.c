#include "model.h"

enum { PADDING_SAME, PADDING_VALID };

/* Options table field ids. */
enum { CONV_PADDING, CONV_STRIDE_W, CONV_STRIDE_H, CONV_ACT, CONV_DIL_W, CONV_DIL_H };
enum { DW_PADDING, DW_STRIDE_W, DW_STRIDE_H, DW_MULT, DW_ACT, DW_DIL_W, DW_DIL_H };
enum { POOL_PADDING, POOL_STRIDE_W, POOL_STRIDE_H, POOL_FILTER_W, POOL_FILTER_H, POOL_ACT };

static int
geometry(Op *op, const Tensor *in, const Tensor *out, uint8_t padding)
{
	int th = (out->dim[1] - 1) * op->stride_h + op->kh - in->dim[1];
	int tw = (out->dim[2] - 1) * op->stride_w + op->kw - in->dim[2];

	if (op->stride_h < 1 || op->stride_w < 1 || op->kh < 1 || op->kw < 1 ||
	    op->kh * op->kw > NET_MAX_TAPS || in->dim[0] != 1 || out->dim[0] != 1)
		return -1;
	if (padding == PADDING_VALID)
		return out->dim[1] == (in->dim[1] - op->kh) / op->stride_h + 1 &&
			out->dim[2] == (in->dim[2] - op->kw) / op->stride_w + 1 ? 0 : -1;
	op->pad_top = th > 0 ? th / 2 : 0;
	op->pad_left = tw > 0 ? tw / 2 : 0;
	return out->dim[1] == (in->dim[1] + op->stride_h - 1) / op->stride_h &&
		out->dim[2] == (in->dim[2] + op->stride_w - 1) / op->stride_w ? 0 : -1;
}

/* Padding spares kernel bounds checks. */
static void
padded(Op *op, const Tensor *in, const Tensor *out)
{
	int h = (out->dim[1] - 1) * op->stride_h + op->kh;
	int w = (out->dim[2] - 1) * op->stride_w + op->kw;

	h = h > in->dim[1] + op->pad_top ? h : in->dim[1] + op->pad_top;
	w = w > in->dim[2] + op->pad_left ? w : in->dim[2] + op->pad_left;
	if (!op->pad_top && !op->pad_left && h == in->dim[1] && w == in->dim[2])
		return;
	op->pad_h = h;
	op->pad_w = w;
}

static int
repack(Net *net, Op *op, const Tensor *f, const Tensor *b)
{
	size_t col = (size_t)op->kh * op->kw * f->dim[3];
	int cout = f->dim[0];
	float *w, *bias;

	op->coutp = (cout + NET_CONV_BLOCK - 1) / NET_CONV_BLOCK * NET_CONV_BLOCK;
	w = net_own(net, col * (size_t)op->coutp);
	bias = net_own(net, (size_t)op->coutp);
	if (!w || !bias)
		return -1;
	for (size_t j = 0; j < f->count; j++)
		w[j % col * (size_t)op->coutp + j / col] = f->data[j];
	memcpy(bias, b->data, (size_t)cout * sizeof(*bias));
	op->weights = w;
	op->bias = bias;
	return 0;
}

int
sliding_conv(const Model *md, size_t opts, Net *net, Op *op)
{
	const Tensor *in = &net->t[op->in[0]], *f = &net->t[op->in[1]];
	const Tensor *b = &net->t[op->in[2]], *out = &net->t[op->out];

	op->stride_w = fb_int(&md->fb, opts, CONV_STRIDE_W, 1);
	op->stride_h = fb_int(&md->fb, opts, CONV_STRIDE_H, 1);
	op->kh = f->dim[1];
	op->kw = f->dim[2];
	if (op->nin != 3 || !f->data || !b->data || f->dim[0] != out->dim[3] ||
	    f->dim[3] != in->dim[3] || (int)b->count != out->dim[3] ||
	    fb_int(&md->fb, opts, CONV_DIL_W, 1) != 1 || fb_int(&md->fb, opts, CONV_DIL_H, 1) != 1 ||
	    model_activation(fb_byte(&md->fb, opts, CONV_ACT, 0), &op->act) < 0 ||
	    geometry(op, in, out, fb_byte(&md->fb, opts, CONV_PADDING, 0)) < 0)
		return -1;
	padded(op, in, out);
	return repack(net, op, f, b);
}

int
sliding_dwconv(const Model *md, size_t opts, Net *net, Op *op)
{
	const Tensor *in = &net->t[op->in[0]], *f = &net->t[op->in[1]];
	const Tensor *b = &net->t[op->in[2]], *out = &net->t[op->out];

	op->stride_w = fb_int(&md->fb, opts, DW_STRIDE_W, 1);
	op->stride_h = fb_int(&md->fb, opts, DW_STRIDE_H, 1);
	op->kh = f->dim[1];
	op->kw = f->dim[2];
	op->weights = f->data;
	op->bias = b->data;
	if (op->nin != 3 || !f->data || !b->data || f->dim[0] != 1 || f->dim[3] != in->dim[3] ||
	    out->dim[3] != in->dim[3] || (int)b->count != in->dim[3] ||
	    fb_int(&md->fb, opts, DW_MULT, 1) != 1 || fb_int(&md->fb, opts, DW_DIL_W, 1) != 1 ||
	    fb_int(&md->fb, opts, DW_DIL_H, 1) != 1 ||
	    model_activation(fb_byte(&md->fb, opts, DW_ACT, 0), &op->act) < 0 ||
	    geometry(op, in, out, fb_byte(&md->fb, opts, DW_PADDING, 0)) < 0)
		return -1;
	padded(op, in, out);
	return 0;
}

int
sliding_maxpool(const Model *md, size_t opts, Net *net, Op *op)
{
	const Tensor *in = &net->t[op->in[0]], *out = &net->t[op->out];

	op->stride_w = fb_int(&md->fb, opts, POOL_STRIDE_W, 1);
	op->stride_h = fb_int(&md->fb, opts, POOL_STRIDE_H, 1);
	op->kw = fb_int(&md->fb, opts, POOL_FILTER_W, 1);
	op->kh = fb_int(&md->fb, opts, POOL_FILTER_H, 1);
	return in->dim[3] == out->dim[3] &&
		model_activation(fb_byte(&md->fb, opts, POOL_ACT, 0), &op->act) == 0 &&
		geometry(op, in, out, fb_byte(&md->fb, opts, POOL_PADDING, 0)) == 0 ? 0 : -1;
}

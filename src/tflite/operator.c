#include "model.h"

enum {
	BUILTIN_ADD = 0,
	BUILTIN_CONCATENATION = 2,
	BUILTIN_CONV_2D = 3,
	BUILTIN_DEPTHWISE_CONV_2D = 4,
	BUILTIN_DEQUANTIZE = 6,
	BUILTIN_MAX_POOL_2D = 17,
	BUILTIN_RELU = 19,
	BUILTIN_RESHAPE = 22,
	BUILTIN_PAD = 34,
	BUILTIN_PRELU = 54,
};

/* Options table field ids. */
enum { ADD_ACT };
enum { CONCAT_AXIS, CONCAT_ACT };

static float
half_to_float(uint16_t h)
{
	uint32_t sign = (uint32_t)(h & 0x8000) << 16;
	uint32_t exp = (h >> 10) & 0x1f;
	uint32_t man = h & 0x3ff;
	uint32_t bits = sign;
	float f;

	if (exp == 0x1f)
		bits |= 0x7f800000 | man << 13;
	else if (exp)
		bits |= (exp + 112) << 23 | man << 13;
	else if (man)
		return sign ? -(float)man * 0x1p-24f : (float)man * 0x1p-24f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

static int
dequantize(Model *md, Net *net, int src, int dst)
{
	size_t bytes;
	const uint8_t *raw = model_raw(md, src, &bytes);
	Tensor *out = &net->t[dst];
	float *vals;

	if (!raw || model_type(md, src) != TYPE_FLOAT16 || bytes != out->count * 2)
		return -1;
	vals = net_own(net, out->count);
	if (!vals)
		return -1;
	for (size_t i = 0; i < out->count; i++) {
		uint16_t h;

		memcpy(&h, raw + 2 * i, sizeof(h));
		vals[i] = half_to_float(h);
	}
	out->data = vals;
	out->runtime = 0;
	return 0;
}

static int
pad(const Model *md, Net *net, Op *op)
{
	const Tensor *in = &net->t[op->in[0]], *out = &net->t[op->out];
	int rank = net->t[op->in[1]].dim[NET_RANK - 2];
	size_t bytes;
	const uint8_t *raw = model_raw(md, op->in[1], &bytes);

	if (op->nin != 2 || !raw || model_type(md, op->in[1]) != TYPE_INT32 || rank > NET_RANK ||
	    bytes != (size_t)rank * 2 * sizeof(int32_t))
		return -1;
	for (int d = 0; d < rank; d++) {
		int32_t p[2];
		int at = NET_RANK - rank + d;

		memcpy(p, raw + 8 * d, sizeof(p));
		if (p[0] < 0 || p[1] < 0 || in->dim[at] + p[0] + p[1] != out->dim[at])
			return -1;
		op->pads[at][0] = p[0];
		op->pads[at][1] = p[1];
	}
	return 0;
}

static int
concat(const Model *md, size_t opts, Net *net, Op *op)
{
	const Tensor *out = &net->t[op->out];
	int axis = fb_int(&md->fb, opts, CONCAT_AXIS, 0);
	int sum = 0;

	op->axis = axis < 0 ? NET_RANK + axis : NET_RANK - out->rank + axis;
	if (op->axis < 0 || op->axis >= NET_RANK ||
	    model_activation(fb_byte(&md->fb, opts, CONCAT_ACT, 0), &op->act) < 0 ||
	    op->act != ACT_NONE)
		return -1;
	for (int i = 0; i < op->nin; i++) {
		const Tensor *in = &net->t[op->in[i]];

		for (int d = 0; d < NET_RANK; d++)
			if (d != op->axis && in->dim[d] != out->dim[d])
				return -1;
		sum += in->dim[op->axis];
	}
	return sum == out->dim[op->axis] ? 0 : -1;
}

static int
same_count(const Net *net, const Op *op, int nin)
{
	for (int i = 0; i < nin; i++)
		if (net->t[op->in[i]].count != net->t[op->out].count)
			return -1;
	return 0;
}

static int
add(const Model *md, size_t opts, Net *net, Op *op)
{
	return op->nin == 2 && same_count(net, op, 2) == 0 ?
		model_activation(fb_byte(&md->fb, opts, ADD_ACT, 0), &op->act) : -1;
}

static int
prelu(const Net *net, Op *op)
{
	const Tensor *alpha = &net->t[op->in[1]];

	op->alpha = alpha->data;
	return op->nin == 2 && alpha->data && (int)alpha->count == net->t[op->in[0]].dim[3] &&
		same_count(net, op, 1) == 0 ? 0 : -1;
}

static int
builtin(const Model *md, size_t oper)
{
	uint32_t ci = (uint32_t)fb_int(&md->fb, oper, OPER_CODE, 0);
	size_t code;
	int a, b;

	if (ci >= fb_veclen(&md->fb, md->codes))
		return -1;
	code = fb_vectable(&md->fb, md->codes, ci);
	a = (int8_t)fb_byte(&md->fb, code, CODE_DEPRECATED, 0);
	b = fb_int(&md->fb, code, CODE_BUILTIN, 0);
	return a > b ? a : b;
}

static int
dispatch(Model *md, size_t opts, Net *net, Op *op, int code)
{
	switch (code) {
	case BUILTIN_ADD: op->kind = OP_ADD; return add(md, opts, net, op);
	case BUILTIN_CONCATENATION: op->kind = OP_CONCAT; return concat(md, opts, net, op);
	case BUILTIN_CONV_2D: op->kind = OP_CONV; return sliding_conv(md, opts, net, op);
	case BUILTIN_DEPTHWISE_CONV_2D: op->kind = OP_DWCONV; return sliding_dwconv(md, opts, net, op);
	case BUILTIN_MAX_POOL_2D: op->kind = OP_MAXPOOL; return sliding_maxpool(md, opts, net, op);
	case BUILTIN_RELU: op->kind = OP_RELU; return same_count(net, op, 1);
	case BUILTIN_RESHAPE: op->kind = OP_RESHAPE; return same_count(net, op, 1);
	case BUILTIN_PAD: op->kind = OP_PAD; return pad(md, net, op);
	case BUILTIN_PRELU: op->kind = OP_PRELU; return prelu(net, op);
	default: return -1;
	}
}

int
operator_parse(Model *md, Net *net, size_t oper)
{
	Op *op = &net->op[net->nop];
	int nout, code = builtin(md, oper);

	if (code < 0 || model_indices(md, fb_ref(&md->fb, oper, OPER_INPUTS), op->in, &op->nin) < 0 ||
	    model_indices(md, fb_ref(&md->fb, oper, OPER_OUTPUTS), &op->out, &nout) < 0 || nout != 1 ||
	    !net->t[op->out].runtime)
		return -1;
	if (code == BUILTIN_DEQUANTIZE)
		return op->nin == 1 ? dequantize(md, net, op->in[0], op->out) : -1;
	for (int i = 0; i < op->nin; i++)
		if (!net->t[op->in[i]].runtime && !net->t[op->in[i]].data && code != BUILTIN_PAD &&
		    code != BUILTIN_RESHAPE)
			return -1;
	if (dispatch(md, fb_ref(&md->fb, oper, OPER_OPTIONS), net, op, code) < 0)
		return -1;
	net->nop++;
	return 0;
}

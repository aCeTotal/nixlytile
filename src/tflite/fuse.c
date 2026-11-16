#include "net.h"

static int
readers(const Net *net, int tensor)
{
	int n = 0;

	for (int i = 0; i < net->nop; i++)
		for (int k = 0; k < net->op[i].nin && net->op[i].kind != OP_DEAD; k++)
			n += net->op[i].in[k] == tensor;
	for (int i = 0; i < net->nout; i++)
		n += net->out[i] == tensor;
	return n;
}

static Op *
producer(Net *net, int tensor)
{
	for (int i = 0; i < net->nop; i++)
		if (net->op[i].out == tensor && net->op[i].kind != OP_DEAD)
			return &net->op[i];
	return NULL;
}

static int
channel_pad(const Op *op)
{
	if (!op || op->kind != OP_PAD || op->pads[NET_RANK - 1][0])
		return 0;
	for (int d = 0; d < NET_RANK - 1; d++)
		if (op->pads[d][0] || op->pads[d][1])
			return 0;
	return 1;
}

/* ADD reads padded operands unpadded. */
static int
fold_pad(Net *net, Op *add, int k)
{
	Op *pad = producer(net, add->in[k]);

	if (!channel_pad(pad) || readers(net, add->in[k]) != 1)
		return 0;
	add->in[k] = add->in[1 - k];
	add->in[1] = pad->in[0];
	pad->kind = OP_DEAD;
	return 1;
}

/* Producer applies the PReLU. */
static void
fold_prelu(Net *net, Op *op)
{
	Op *prev = producer(net, op->in[0]);

	if (!prev || prev->alpha || readers(net, prev->out) != 1 ||
	    (prev->kind != OP_CONV && prev->kind != OP_DWCONV && prev->kind != OP_ADD))
		return;
	prev->alpha = op->alpha;
	prev->out = op->out;
	op->kind = OP_DEAD;
}

void
fuse_ops(Net *net)
{
	int kept = 0;

	for (int i = 0; i < net->nop; i++)
		if (net->op[i].kind == OP_ADD && !fold_pad(net, &net->op[i], 1))
			fold_pad(net, &net->op[i], 0);
	for (int i = 0; i < net->nop; i++)
		if (net->op[i].kind == OP_PRELU)
			fold_prelu(net, &net->op[i]);
	for (int i = 0; i < net->nop; i++)
		if (net->op[i].kind != OP_DEAD)
			net->op[kept++] = net->op[i];
	net->nop = kept;
}

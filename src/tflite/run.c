#include "net.h"

static const Kernel kernels[OP_COUNT] = {
	[OP_ADD] = kernel_add,
	[OP_CONCAT] = kernel_concat,
	[OP_CONV] = kernel_conv,
	[OP_DWCONV] = kernel_dwconv,
	[OP_MAXPOOL] = kernel_maxpool,
	[OP_PAD] = kernel_pad,
	[OP_PRELU] = kernel_prelu,
	[OP_RELU] = kernel_relu,
	[OP_RESHAPE] = kernel_reshape,
};

void
net_run(Net *net)
{
	for (int i = 0; i < net->nop; i++)
		kernels[net->op[i].kind](net, &net->op[i]);
}

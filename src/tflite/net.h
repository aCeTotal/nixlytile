#ifndef TFLITE_NET_H
#define TFLITE_NET_H

#include <float.h>
#include <stddef.h>

#include "tflite.h"

#define NET_RANK 4
#define NET_MAX_IN 4
#define NET_MAX_TAPS 25
#define NET_CONV_BLOCK 16

typedef struct {
	float *data;
	int dim[NET_RANK];      /* NHWC, left-padded with 1 */
	int rank;
	size_t count;
	int first, last;        /* defining and last reading op */
	int runtime;            /* lives in the arena */
} Tensor;

typedef enum { ACT_NONE, ACT_RELU, ACT_RELU6 } Act;

typedef enum {
	OP_ADD,
	OP_CONCAT,
	OP_CONV,
	OP_DWCONV,
	OP_MAXPOOL,
	OP_PAD,
	OP_PRELU,
	OP_RELU,
	OP_RESHAPE,
	OP_DEAD,
	OP_COUNT,
} OpKind;

typedef struct {
	OpKind kind;
	Act act;
	int nin;
	int in[NET_MAX_IN];
	int out;
	int stride_h, stride_w;
	int kh, kw;
	int pad_top, pad_left;
	int axis;
	int pads[NET_RANK][2];
	int pad_h, pad_w;       /* padded input, 0 if none */
	int coutp;              /* conv output channels rounded up */
	const float *weights;   /* conv: HWIO with coutp columns */
	const float *bias;
	const float *alpha;     /* fused PReLU slopes */
} Op;

struct Net {
	void *map;
	size_t maplen;
	Tensor *t;
	int nt;
	Op *op;
	int nop;
	int in[NET_MAX_IN], nin;
	int out[NET_MAX_IN], nout;
	float *arena;
	float *scratch;         /* padded window input */
	float **owned;
	int nowned;
};

#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
#define NET_SIMD __attribute__((target_clones("arch=x86-64-v3", "default")))
#else
#define NET_SIMD
#endif

#define NET_HOT static inline __attribute__((always_inline))

typedef void (*Kernel)(const Net *net, const Op *op);

NET_HOT void
activate(float *v, size_t n, Act act)
{
	float hi = act == ACT_RELU6 ? 6.0f : FLT_MAX;

	if (act == ACT_NONE)
		return;
	for (size_t i = 0; i < n; i++)
		v[i] = v[i] < 0.0f ? 0.0f : v[i] > hi ? hi : v[i];
}

void kernel_conv(const Net *net, const Op *op);
void kernel_dwconv(const Net *net, const Op *op);
void kernel_add(const Net *net, const Op *op);
void kernel_prelu(const Net *net, const Op *op);
void kernel_relu(const Net *net, const Op *op);
void kernel_maxpool(const Net *net, const Op *op);
void kernel_pad(const Net *net, const Op *op);
void kernel_reshape(const Net *net, const Op *op);
void kernel_concat(const Net *net, const Op *op);

int arena_plan(Net *net);
void fuse_ops(Net *net);
float *net_own(Net *net, size_t count);

#endif

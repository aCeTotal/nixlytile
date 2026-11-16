#ifndef TFLITE_MODEL_H
#define TFLITE_MODEL_H

#include "flatbuf.h"
#include "net.h"

/* Ids from TFLite schema.fbs. */
enum { MODEL_CODES = 1, MODEL_SUBGRAPHS = 2, MODEL_BUFFERS = 4 };
enum { CODE_DEPRECATED = 0, CODE_BUILTIN = 3 };
enum { GRAPH_TENSORS = 0, GRAPH_INPUTS = 1, GRAPH_OUTPUTS = 2, GRAPH_OPS = 3 };
enum { TENSOR_SHAPE = 0, TENSOR_TYPE = 1, TENSOR_BUFFER = 2 };
enum { BUFFER_DATA = 0 };
enum { OPER_CODE = 0, OPER_INPUTS = 1, OPER_OUTPUTS = 2, OPER_OPTIONS = 4 };
enum { TYPE_FLOAT32 = 0, TYPE_FLOAT16 = 1, TYPE_INT32 = 2 };
enum { TFL_ACT_NONE = 0, TFL_ACT_RELU = 1, TFL_ACT_RELU6 = 3 };

typedef struct {
	Flatbuf fb;
	size_t root, codes, buffers, tensors, ops;
	int ntensors;
} Model;

/* Constant payload, NULL at runtime. */
static inline const void *
model_raw(const Model *md, int i, size_t *bytes)
{
	size_t t = fb_vectable(&md->fb, md->tensors, (uint32_t)i);
	uint32_t b = (uint32_t)fb_int(&md->fb, t, TENSOR_BUFFER, 0);
	size_t data;

	if (b >= fb_veclen(&md->fb, md->buffers))
		return NULL;
	data = fb_ref(&md->fb, fb_vectable(&md->fb, md->buffers, b), BUFFER_DATA);
	*bytes = fb_veclen(&md->fb, data);
	if (!*bytes || fb_vecdata(data) + *bytes > md->fb.len)
		return NULL;
	return md->fb.base + fb_vecdata(data);
}

static inline int
model_type(const Model *md, int i)
{
	return fb_byte(&md->fb, fb_vectable(&md->fb, md->tensors, (uint32_t)i), TENSOR_TYPE, 0);
}

static inline int
model_indices(const Model *md, size_t vec, int *dst, int *n)
{
	uint32_t len = fb_veclen(&md->fb, vec);

	if (len == 0 || len > NET_MAX_IN)
		return -1;
	for (uint32_t i = 0; i < len; i++) {
		dst[i] = (int32_t)fb_u32(&md->fb, fb_vecdata(vec) + 4 * i);
		if (dst[i] < 0 || dst[i] >= md->ntensors)
			return -1;
	}
	*n = (int)len;
	return 0;
}

static inline int
model_activation(uint8_t code, Act *act)
{
	switch (code) {
	case TFL_ACT_NONE: *act = ACT_NONE; return 0;
	case TFL_ACT_RELU: *act = ACT_RELU; return 0;
	case TFL_ACT_RELU6: *act = ACT_RELU6; return 0;
	default: return -1;
	}
}

int operator_parse(Model *md, Net *net, size_t oper);
int sliding_conv(const Model *md, size_t opts, Net *net, Op *op);
int sliding_dwconv(const Model *md, size_t opts, Net *net, Op *op);
int sliding_maxpool(const Model *md, size_t opts, Net *net, Op *op);

#endif

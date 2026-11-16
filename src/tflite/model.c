#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "model.h"

static int
parse_tensor(const Model *md, Net *net, int i)
{
	size_t t = fb_vectable(&md->fb, md->tensors, (uint32_t)i);
	size_t shape = fb_ref(&md->fb, t, TENSOR_SHAPE);
	uint32_t rank = fb_veclen(&md->fb, shape);
	Tensor *ts = &net->t[i];
	size_t bytes;
	const void *raw = model_raw(md, i, &bytes);

	if (rank > NET_RANK)
		return -1;
	ts->rank = (int)rank;
	ts->count = 1;
	for (int d = 0; d < NET_RANK; d++)
		ts->dim[d] = 1;
	for (uint32_t d = 0; d < rank; d++) {
		int32_t n = (int32_t)fb_u32(&md->fb, fb_vecdata(shape) + 4 * d);

		if (n <= 0)
			return -1;
		ts->dim[NET_RANK - rank + d] = n;
		ts->count *= (size_t)n;
	}
	ts->first = ts->last = -1;
	ts->runtime = !raw;
	if (raw && fb_byte(&md->fb, t, TENSOR_TYPE, 0) == TYPE_FLOAT32) {
		if (bytes != ts->count * sizeof(float))
			return -1;
		ts->data = (float *)raw;
	}
	return 0;
}

static int
parse_graph(Model *md, Net *net)
{
	size_t graph;

	if (fb_veclen(&md->fb, fb_ref(&md->fb, md->root, MODEL_SUBGRAPHS)) != 1)
		return -1;
	graph = fb_vectable(&md->fb, fb_ref(&md->fb, md->root, MODEL_SUBGRAPHS), 0);
	md->tensors = fb_ref(&md->fb, graph, GRAPH_TENSORS);
	md->ops = fb_ref(&md->fb, graph, GRAPH_OPS);
	md->ntensors = (int)fb_veclen(&md->fb, md->tensors);
	if (model_indices(md, fb_ref(&md->fb, graph, GRAPH_INPUTS), net->in, &net->nin) < 0 ||
	    model_indices(md, fb_ref(&md->fb, graph, GRAPH_OUTPUTS), net->out, &net->nout) < 0)
		return -1;
	net->nt = md->ntensors;
	net->t = calloc((size_t)net->nt, sizeof(*net->t));
	net->op = calloc(fb_veclen(&md->fb, md->ops), sizeof(*net->op));
	net->owned = calloc((size_t)net->nt + fb_veclen(&md->fb, md->ops) * 2, sizeof(*net->owned));
	if (!net->t || !net->op || !net->owned)
		return -1;
	for (int i = 0; i < net->nt; i++)
		if (parse_tensor(md, net, i) < 0)
			return -1;
	for (uint32_t i = 0; i < fb_veclen(&md->fb, md->ops); i++)
		if (operator_parse(md, net, fb_vectable(&md->fb, md->ops, i)) < 0)
			return -1;
	for (int i = 0; i < net->nout; i++)
		if (!net->t[net->out[i]].runtime)
			return -1;
	fuse_ops(net);
	return arena_plan(net);
}

Net *
net_load(const char *path)
{
	Net *net = calloc(1, sizeof(*net));
	Model md = {0};
	struct stat st;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (!net || fd < 0 || fstat(fd, &st) < 0 || st.st_size < 8)
		goto fail;
	net->maplen = (size_t)st.st_size;
	net->map = mmap(NULL, net->maplen, PROT_READ, MAP_PRIVATE, fd, 0);
	if (net->map == MAP_FAILED) {
		net->map = NULL;
		goto fail;
	}
	md.fb = (Flatbuf){ net->map, net->maplen };
	md.root = fb_u32(&md.fb, 0);
	md.codes = fb_ref(&md.fb, md.root, MODEL_CODES);
	md.buffers = fb_ref(&md.fb, md.root, MODEL_BUFFERS);
	if (memcmp((const char *)net->map + 4, "TFL3", 4) || parse_graph(&md, net) < 0)
		goto fail;
	close(fd);
	return net;
fail:
	if (fd >= 0)
		close(fd);
	net_free(net);
	return NULL;
}

void
net_free(Net *net)
{
	if (!net)
		return;
	for (int i = 0; i < net->nowned; i++)
		free(net->owned[i]);
	free(net->owned);
	free(net->op);
	free(net->t);
	free(net->arena);
	free(net->scratch);
	if (net->map)
		munmap(net->map, net->maplen);
	free(net);
}

float *
net_own(Net *net, size_t count)
{
	float *p = calloc(count, sizeof(*p));

	if (p)
		net->owned[net->nowned++] = p;
	return p;
}

float *
net_input(Net *net, int i)
{
	return net->t[net->in[i]].data;
}

const float *
net_output(const Net *net, int i)
{
	return net->t[net->out[i]].data;
}

size_t
net_output_count(const Net *net, int i)
{
	return net->t[net->out[i]].count;
}

#include <stdlib.h>

#include "net.h"

#define SLOT_FLOATS 16

typedef struct {
	size_t size, at;
	int first, last, tensor;
} Slot;

/* Op i runs at i+1. */
static int
lifetimes(Net *net)
{
	for (int i = 0; i < net->nin; i++) {
		Tensor *t = &net->t[net->in[i]];

		if (!t->runtime)
			return -1;
		t->first = t->last = 0;
	}
	for (int o = 0; o < net->nop; o++) {
		const Op *op = &net->op[o];
		Tensor *out = &net->t[op->out];

		for (int i = 0; i < op->nin; i++) {
			Tensor *t = &net->t[op->in[i]];

			if (!t->runtime)
				continue;
			if (t->first < 0)
				return -1;
			t->last = o + 1;
		}
		if (out->first >= 0)
			return -1;
		out->first = out->last = o + 1;
	}
	for (int i = 0; i < net->nout; i++) {
		if (net->t[net->out[i]].first < 0)
			return -1;
		net->t[net->out[i]].last = net->nop + 1;
	}
	for (int i = 0; i < net->nin; i++)
		net->t[net->in[i]].last = net->nop + 1;
	return 0;
}

static int
larger(const void *a, const void *b)
{
	const Slot *x = a, *y = b;

	return (x->size < y->size) - (x->size > y->size);
}

static size_t
place(const Slot *s, int n, const Slot *new)
{
	size_t at = 0;
	int moved = 1;

	while (moved) {
		moved = 0;
		for (int j = 0; j < n; j++) {
			if (s[j].last < new->first || new->last < s[j].first)
				continue;
			if (at < s[j].at + s[j].size && s[j].at < at + new->size) {
				at = s[j].at + s[j].size;
				moved = 1;
			}
		}
	}
	return at;
}

static int
scratch_plan(Net *net)
{
	size_t most = 0;

	for (int i = 0; i < net->nop; i++) {
		const Op *op = &net->op[i];
		size_t n = (size_t)op->pad_h * op->pad_w * net->t[op->in[0]].dim[3];

		most = n > most ? n : most;
	}
	if (!most)
		return 0;
	net->scratch = malloc(most * sizeof(*net->scratch));
	return net->scratch ? 0 : -1;
}

/* Disjoint lifetimes share memory. */
int
arena_plan(Net *net)
{
	Slot *s = malloc((size_t)net->nt * sizeof(*s));
	size_t total = 0;
	int n = 0;

	if (!s || lifetimes(net) < 0) {
		free(s);
		return -1;
	}
	for (int i = 0; i < net->nt; i++) {
		const Tensor *t = &net->t[i];

		if (t->runtime && t->first >= 0)
			s[n++] = (Slot){ (t->count + SLOT_FLOATS - 1) & ~(size_t)(SLOT_FLOATS - 1),
				0, t->first, t->last, i };
	}
	qsort(s, (size_t)n, sizeof(*s), larger);
	for (int i = 0; i < n; i++) {
		s[i].at = place(s, i, &s[i]);
		if (s[i].at + s[i].size > total)
			total = s[i].at + s[i].size;
	}
	net->arena = total ? aligned_alloc(SLOT_FLOATS * sizeof(float), total * sizeof(float)) : NULL;
	if (net->arena)
		for (int i = 0; i < n; i++)
			net->t[s[i].tensor].data = net->arena + s[i].at;
	free(s);
	return net->arena ? scratch_plan(net) : -1;
}

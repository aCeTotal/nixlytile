#include "spirv.h"

#include <string.h>

#define FOLD_DEPTH   16
#define SCALAR_WORDS 4          /* 32-bit constants only */
#define OPERANDS     3          /* after type and result */

/* Fold recursion state. */
struct probe {
	const struct flow *flow;
	int depth;
};

/* Phi operands folded so far. */
struct agreement {
	struct constant agreed;
	int split;                  /* reached values differ */
};

static const struct constant unknown = { 0, 0 };

static struct constant
operand(struct probe p, uint32_t id)
{
	return spirv_known(p.flow, id, p.depth + 1);
}

static const VkSpecializationMapEntry *
map_entry(const VkSpecializationInfo *s, uint32_t constant)
{
	uint32_t i = 0;

	while (i < s->mapEntryCount && s->pMapEntries[i].constantID != constant)
		i++;
	return i < s->mapEntryCount ? &s->pMapEntries[i] : NULL;
}

/* Specialized value, else the default. */
static struct constant
spec_value(const struct flow *f, uint32_t id, uint32_t fallback)
{
	const VkSpecializationInfo *s = f->specialization;
	const VkSpecializationMapEntry *e = f->spec[id] && s
		? map_entry(s, f->spec[id] - 1) : NULL;
	uint32_t v = 0;

	if (!e)
		return (struct constant){ 1, fallback };
	if (e->size > sizeof(v) || e->offset + e->size > s->dataSize)
		return unknown;
	memcpy(&v, (const char *)s->pData + e->offset, e->size);
	return (struct constant){ 1, v };
}

static struct agreement
agree(struct probe p, struct agreement a, const uint32_t *pair)
{
	struct constant in;

	if (pair[1] >= p.flow->bound || !p.flow->seen[pair[1]])
		return a;
	in = operand(p, pair[0]);
	a.split = !in.known || (a.agreed.known && in.bits != a.agreed.bits);
	a.agreed = in;
	return a;
}

/* Agreed by reached predecessors. */
static struct constant
phi(struct probe p, const uint32_t *w, uint32_t count)
{
	struct agreement a = { { 0, 0 }, 0 };
	uint32_t i;

	for (i = OPERANDS; !a.split && i + 1 < count; i += 2)
		a = agree(p, a, w + i);
	return a.split ? unknown : a.agreed;
}

/* Settles AND or OR alone. */
static int
decides(uint32_t op, struct constant x)
{
	return x.known && ((op == OP_LOGICAL_AND && !x.bits)
		|| (op == OP_LOGICAL_OR && x.bits));
}

/* Logic on link-time values; short-circuits. */
static struct constant
fold(struct probe p, uint32_t op, const uint32_t *args)
{
	struct constant a = operand(p, args[0]), b;

	if (op == OP_LOGICAL_NOT)
		return (struct constant){ a.known, !a.bits };
	if (op == OP_SELECT)
		return a.known ? operand(p, a.bits ? args[1] : args[2]) : a;
	if (decides(op, a))
		return a;
	b = operand(p, args[1]);
	if (decides(op, b))
		return b;
	if (!a.known || !b.known)
		return unknown;
	switch (op) {
	case OP_LOGICAL_AND:
		return (struct constant){ 1, a.bits && b.bits };
	case OP_LOGICAL_OR:
		return (struct constant){ 1, a.bits || b.bits };
	case OP_LOGICAL_EQUAL:
	case OP_IEQUAL:
		return (struct constant){ 1, a.bits == b.bits };
	case OP_LOGICAL_NOT_EQUAL:
	case OP_INOT_EQUAL:
	default:
		return (struct constant){ 1, a.bits != b.bits };
	}
}

/* Operands a folded op needs. */
static uint32_t
arity(uint32_t op)
{
	if (op == OP_LOGICAL_NOT)
		return 1;
	if (op == OP_SELECT)
		return 3;
	return op >= OP_LOGICAL_EQUAL && op <= OP_INOT_EQUAL ? 2 : 0;
}

/* Plain or OpSpecConstantOp-wrapped logic. */
static struct constant
folded(struct probe p, const uint32_t *w, uint32_t count)
{
	int wrapped = op_of(w) == OP_SPEC_CONSTANT_OP && count > OPERANDS;
	uint32_t op = wrapped ? w[OPERANDS] : op_of(w);
	uint32_t at = wrapped ? OPERANDS + 1 : OPERANDS;

	if (!arity(op) || count < at + arity(op))
		return unknown;
	return fold(p, op, w + at);
}

struct constant
spirv_known(const struct flow *f, uint32_t id, int depth)
{
	struct probe p = { f, depth };
	const uint32_t *w;
	uint32_t op, count;

	if (id >= f->bound || !f->def[id] || depth > FOLD_DEPTH)
		return unknown;
	w = f->code + f->def[id];
	op = op_of(w);
	count = count_of(w);
	switch (op) {
	case OP_CONSTANT_TRUE:
	case OP_CONSTANT_FALSE:
		return (struct constant){ 1, op == OP_CONSTANT_TRUE };
	case OP_CONSTANT:
		return count == SCALAR_WORDS ? (struct constant){ 1, w[OPERANDS] } : unknown;
	case OP_SPEC_CONSTANT_TRUE:
	case OP_SPEC_CONSTANT_FALSE:
		return spec_value(f, id, op == OP_SPEC_CONSTANT_TRUE);
	case OP_SPEC_CONSTANT:
		return count == SCALAR_WORDS ? spec_value(f, id, w[OPERANDS]) : unknown;
	case OP_PHI:
		return phi(p, w, count);
	default:
		return folded(p, w, count);
	}
}

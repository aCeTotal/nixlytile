#include "spirv.h"

#include <stdlib.h>
#include <string.h>

#define BOUND_MAX    (1u << 22)
#define ID_WORDS     3          /* def, spec and blocks */
#define SWITCH_CASES 3          /* after selector and default */
#define ENTRY_NAME   3          /* after model and function */

/* Block walk outcome per instruction. */
enum step { STEP_NEXT, STEP_END, STEP_DISCARD };

/* Result ids of folded ops. */
static uint32_t
result_id(const uint32_t *w, uint32_t count)
{
	uint32_t op = op_of(w);

	if (op == OP_LABEL && count > WORD_LABEL)
		return w[WORD_LABEL];
	if (count <= WORD_RESULT)
		return 0;
	if ((op >= OP_CONSTANT_TRUE && op <= OP_CONSTANT)
			|| (op >= OP_SPEC_CONSTANT_TRUE && op <= OP_SPEC_CONSTANT_OP)
			|| (op >= OP_LOGICAL_EQUAL && op <= OP_INOT_EQUAL)
			|| op == OP_PHI || op == OP_FUNCTION)
		return w[WORD_RESULT];
	return 0;
}

static void
index_op(struct flow *f, size_t at)
{
	const uint32_t *w = f->code + at;
	uint32_t count = count_of(w), id = result_id(w, count);

	if (id && id < f->bound)
		f->def[id] = (uint32_t)at;
	if (op_of(w) == OP_DECORATE && count > WORD_SPEC_ID
			&& w[WORD_DECORATION] == DECORATION_SPEC_ID
			&& w[WORD_TARGET] < f->bound)
		f->spec[w[WORD_TARGET]] = w[WORD_SPEC_ID] + 1;
}

static void
index_module(struct flow *f)
{
	size_t i;

	for (i = SPIRV_HEADER; i < f->n; i += count_of(f->code + i))
		index_op(f, i);
}

static void
push(struct flow *f, uint32_t label)
{
	if (label >= f->bound || f->seen[label])
		return;
	f->seen[label] = 1;
	f->blocks[f->top++] = label;
}

/* 0 when no label. */
static uint32_t
entry_label(const struct flow *f, uint32_t function)
{
	size_t i = function < f->bound ? f->def[function] : 0;

	while (i && i < f->n && op_of(f->code + i) != OP_LABEL)
		i += count_of(f->code + i);
	if (!i || i >= f->n)
		return 0;
	return result_id(f->code + i, count_of(f->code + i));
}

static enum step
branch(struct flow *f, const uint32_t *w, uint32_t count)
{
	struct constant c;

	if (count <= WORD_FALSE_LABEL)
		return STEP_DISCARD;
	c = spirv_known(f, w[WORD_CONDITION], 0);
	if (!c.known || c.bits)
		push(f, w[WORD_TRUE_LABEL]);
	if (!c.known || !c.bits)
		push(f, w[WORD_FALSE_LABEL]);
	return STEP_END;
}

/* Push a reachable case. */
static int
take_case(struct flow *f, struct constant selector, const uint32_t *pair)
{
	if (selector.known && selector.bits != pair[0])
		return 0;
	push(f, pair[1]);
	return selector.known;
}

/* Single-word literals only; else all. */
static enum step
cases(struct flow *f, const uint32_t *w, uint32_t count)
{
	struct constant s;
	int hit = 0;
	uint32_t i;

	if (count < SWITCH_CASES || ((count - SWITCH_CASES) & 1))
		return STEP_DISCARD;
	s = spirv_known(f, w[WORD_SELECTOR], 0);
	for (i = SWITCH_CASES; i + 1 < count; i += 2)
		hit |= take_case(f, s, w + i);
	if (!hit)
		push(f, w[WORD_DEFAULT]);
	return STEP_END;
}

static enum step
follow(struct flow *f, const uint32_t *w)
{
	uint32_t count = count_of(w);

	switch (op_of(w)) {
	case OP_KILL:
	case OP_TERMINATE_INVOCATION:
	case OP_DEMOTE_TO_HELPER:
		return STEP_DISCARD;
	case OP_FUNCTION_CALL:
		push(f, entry_label(f, count > WORD_CALLEE ? w[WORD_CALLEE] : 0));
		return STEP_NEXT;
	case OP_BRANCH:
		push(f, count > WORD_TARGET ? w[WORD_TARGET] : 0);
		return STEP_END;
	case OP_BRANCH_CONDITIONAL:
		return branch(f, w, count);
	case OP_SWITCH:
		return cases(f, w, count);
	case OP_RETURN:
	case OP_RETURN_VALUE:
	case OP_UNREACHABLE:
		return STEP_END;
	default:
		return STEP_NEXT;
	}
}

/* 1: discard reachable or unreadable. */
static int
walk_block(struct flow *f, uint32_t label)
{
	size_t i = f->def[label];
	enum step s = STEP_NEXT;

	if (!i || op_of(f->code + i) != OP_LABEL)
		return 1;
	for (i += count_of(f->code + i); s == STEP_NEXT && i < f->n;
			i += count_of(f->code + i))
		s = follow(f, f->code + i);
	return s != STEP_END;
}

/* Every reached block, once. */
static int
walk_reached(struct flow *f)
{
	size_t i = 0;

	while (i < f->top && !walk_block(f, f->blocks[i]))
		i++;
	return i < f->top;
}

static int
entry_named(const uint32_t *w, const char *name, size_t len)
{
	uint32_t count = count_of(w);

	return op_of(w) == OP_ENTRY_POINT && count > ENTRY_NAME
		&& w[WORD_MODEL] == MODEL_FRAGMENT
		&& (count - ENTRY_NAME) * sizeof(*w) > len
		&& !memcmp(w + ENTRY_NAME, name, len + 1);
}

static uint32_t
fragment_entry(const struct flow *f, const char *name)
{
	size_t i = SPIRV_HEADER, len = strlen(name);

	while (i < f->n && !entry_named(f->code + i, name, len))
		i += count_of(f->code + i);
	return i < f->n ? f->code[i + WORD_ENTRY] : 0;
}

/* Grow reached set until stable. */
static int
walk(struct flow *f, const char *name)
{
	uint32_t entry;
	size_t grown;
	int discards;

	index_module(f);
	entry = entry_label(f, fragment_entry(f, name));
	if (!entry)
		return 1;
	push(f, entry);
	do {
		grown = f->top;
		discards = walk_reached(f);
	} while (!discards && f->top != grown);
	return discards;
}

/* Discard reachable under this specialization. */
int
spirv_discards(const struct entry *e)
{
	struct flow f = {
		.code = e->code, .n = e->words, .specialization = e->specialization,
	};
	uint32_t *mem;
	int discards;

	f.bound = e->code[SPIRV_BOUND];
	if (!f.bound || f.bound > BOUND_MAX || !e->name)
		return 1;
	mem = calloc(f.bound, ID_WORDS * sizeof(*mem) + 1);
	if (!mem)
		return 1;
	f.def = mem;
	f.spec = f.def + f.bound;
	f.blocks = f.spec + f.bound;
	f.seen = (unsigned char *)(f.blocks + f.bound);
	discards = walk(&f, e->name);
	free(mem);
	return discards;
}

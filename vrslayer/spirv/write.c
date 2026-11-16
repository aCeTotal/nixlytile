#include "spirv.h"

#include <stdlib.h>

/* Ids pointing at shared memory. */
struct writes {
	unsigned char *block;       /* BufferBlock struct types */
	unsigned char *shared;      /* pointer types and values */
	uint32_t bound;
	int found;
};

static int
shared_at(const struct writes *w, uint32_t id)
{
	return id < w->bound && w->shared[id];
}

static void
set_shared(struct writes *w, uint32_t id, int shared)
{
	if (id < w->bound)
		w->shared[id] = (unsigned char)shared;
}

static void
mark_block(struct writes *w, uint32_t id)
{
	if (id < w->bound)
		w->block[id] = 1;
}

/* Old SSBOs: Uniform plus BufferBlock. */
static int
shared_class(const struct writes *w, uint32_t storage, uint32_t pointee)
{
	if (storage == STORAGE_UNIFORM)
		return pointee < w->bound && w->block[pointee];
	return storage == STORAGE_BUFFER || storage == STORAGE_PHYSICAL_BUFFER
		|| storage == STORAGE_IMAGE;
}

static int
atomic_write(uint32_t op)
{
	return (op >= OP_ATOMIC_STORE && op <= OP_ATOMIC_XOR) || op == OP_ATOMIC_FLAG_SET
		|| op == OP_ATOMIC_FLAG_CLEAR || op == OP_ATOMIC_FMIN
		|| op == OP_ATOMIC_FMAX || op == OP_ATOMIC_FADD;
}

static int
stores(uint32_t op)
{
	return op == OP_STORE || op == OP_COPY_MEMORY || op == OP_COPY_MEMORY_SIZED;
}

/* Result takes its type's class. */
static int
typed_pointer(uint32_t op)
{
	return op == OP_VARIABLE || op == OP_FUNCTION_PARAMETER || op == OP_FUNCTION_CALL
		|| op == OP_LOAD || op == OP_COMPOSITE_EXTRACT || op == OP_PHI
		|| op == OP_SELECT || op == OP_CONVERT_U_TO_PTR || op == OP_BITCAST;
}

/* Result takes its base's class. */
static int
chained(uint32_t op)
{
	return op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_CHAIN
		|| op == OP_PTR_ACCESS_CHAIN || op == OP_IN_BOUNDS_PTR_CHAIN
		|| op == OP_COPY_OBJECT;
}

static void
visit(struct writes *w, const uint32_t *ins)
{
	uint32_t op = op_of(ins), count = count_of(ins);

	w->found |= atomic_write(op) || op == OP_IMAGE_WRITE
		|| (stores(op) && count > WORD_TARGET && shared_at(w, ins[WORD_TARGET]));
	if (op == OP_DECORATE && count > WORD_DECORATION
			&& ins[WORD_DECORATION] == DECORATION_BUFFER_BLOCK)
		mark_block(w, ins[WORD_TARGET]);
	if (op == OP_TYPE_POINTER && count > WORD_POINTEE)
		set_shared(w, ins[WORD_POINTER_TYPE],
			shared_class(w, ins[WORD_POINTER_STORAGE], ins[WORD_POINTEE]));
	if (typed_pointer(op) && count > WORD_RESULT)
		set_shared(w, ins[WORD_RESULT], shared_at(w, ins[WORD_RESULT_TYPE]));
	if (chained(op) && count > WORD_BASE)
		set_shared(w, ins[WORD_RESULT], shared_at(w, ins[WORD_BASE]));
}

/* Coarse shading would drop writes. */
int
spirv_writes(const uint32_t *code, size_t words)
{
	struct writes w = { .bound = code[SPIRV_BOUND] };
	size_t i;

	w.block = calloc(w.bound, 1);
	w.shared = calloc(w.bound, 1);
	w.found = !w.block || !w.shared;
	for (i = SPIRV_HEADER; i < words && !w.found; i += count_of(code + i))
		visit(&w, code + i);
	free(w.block);
	free(w.shared);
	return w.found;
}

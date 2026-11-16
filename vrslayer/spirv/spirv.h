#ifndef NIXLY_VRS_SPIRV_H
#define NIXLY_VRS_SPIRV_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* SPIR-V numbers the scans read. */
enum spirv {
	SPIRV_MAGIC             = 0x07230203,
	SPIRV_HEADER            = 5,
	SPIRV_BOUND             = 3,
	SPIRV_COUNT_SHIFT       = 16,
	SPIRV_OPCODE            = 0xffff,
	OP_ENTRY_POINT          = 15,
	OP_EXECUTION_MODE       = 16,
	OP_CAPABILITY           = 17,
	OP_CONSTANT_TRUE        = 41,
	OP_CONSTANT_FALSE       = 42,
	OP_CONSTANT             = 43,
	OP_SPEC_CONSTANT_TRUE   = 48,
	OP_SPEC_CONSTANT_FALSE  = 49,
	OP_SPEC_CONSTANT        = 50,
	OP_SPEC_CONSTANT_OP     = 52,
	OP_FUNCTION             = 54,
	OP_FUNCTION_PARAMETER   = 55,
	OP_FUNCTION_CALL        = 57,
	OP_TYPE_POINTER         = 32,
	OP_LOAD                 = 61,
	OP_STORE                = 62,
	OP_COPY_MEMORY          = 63,
	OP_COPY_MEMORY_SIZED    = 64,
	OP_ACCESS_CHAIN         = 65,
	OP_IN_BOUNDS_CHAIN      = 66,
	OP_PTR_ACCESS_CHAIN     = 67,
	OP_IN_BOUNDS_PTR_CHAIN  = 70,
	OP_COMPOSITE_EXTRACT    = 81,
	OP_COPY_OBJECT          = 83,
	OP_IMAGE_WRITE          = 99,
	OP_CONVERT_U_TO_PTR     = 120,
	OP_BITCAST              = 124,
	OP_ATOMIC_STORE         = 228,
	OP_ATOMIC_XOR           = 242,
	OP_ATOMIC_FLAG_SET      = 318,
	OP_ATOMIC_FLAG_CLEAR    = 319,
	OP_ATOMIC_FMIN          = 5614,
	OP_ATOMIC_FMAX          = 5615,
	OP_ATOMIC_FADD          = 6035,
	OP_VARIABLE             = 59,
	OP_DECORATE             = 71,
	OP_LOGICAL_EQUAL        = 164,
	OP_LOGICAL_NOT_EQUAL    = 165,
	OP_LOGICAL_OR           = 166,
	OP_LOGICAL_AND          = 167,
	OP_LOGICAL_NOT          = 168,
	OP_SELECT               = 169,
	OP_IEQUAL               = 170,
	OP_INOT_EQUAL           = 171,
	OP_PHI                  = 245,
	OP_LABEL                = 248,
	OP_BRANCH               = 249,
	OP_BRANCH_CONDITIONAL   = 250,
	OP_SWITCH               = 251,
	OP_KILL                 = 252,
	OP_RETURN               = 253,
	OP_RETURN_VALUE         = 254,
	OP_UNREACHABLE          = 255,
	OP_TERMINATE_INVOCATION = 4416,
	OP_DEMOTE_TO_HELPER     = 5380,
	DECORATION_SPEC_ID      = 1,
	DECORATION_BUFFER_BLOCK = 3,
	DECORATION_BUILTIN      = 11,
	MODEL_FRAGMENT          = 4,
	BUILTIN_SAMPLE_ID       = 18,
	BUILTIN_SAMPLE_POSITION = 19,
	BUILTIN_SAMPLE_MASK     = 20,
	BUILTIN_FRAG_DEPTH      = 22,
	BUILTIN_FRAG_STENCIL    = 5014,
	STORAGE_INPUT           = 1,
	STORAGE_UNIFORM         = 2,
	STORAGE_OUTPUT          = 3,
	STORAGE_IMAGE           = 11,
	STORAGE_BUFFER          = 12,
	STORAGE_PHYSICAL_BUFFER = 5349,
	CAP_SAMPLE_RATE_SHADING = 35,
	CAP_STENCIL_EXPORT      = 5013,
	CAP_SAMPLE_INTERLOCK    = 5363,
	CAP_PIXEL_INTERLOCK     = 5378,
	MODE_DEPTH_REPLACING    = 12,
	MODE_STENCIL_REPLACING  = 5027,
};

/* Operand words the scans read. */
enum word {
	WORD_RESULT_TYPE     = 1,
	WORD_RESULT          = 2,
	WORD_CAPABILITY      = 1,
	WORD_MODEL           = 1,
	WORD_ENTRY           = 2,
	WORD_MODE            = 2,
	WORD_TARGET          = 1,
	WORD_DECORATION      = 2,
	WORD_BUILTIN         = 3,
	WORD_SPEC_ID         = 3,
	WORD_POINTER_TYPE    = 1,
	WORD_POINTER_STORAGE = 2,
	WORD_POINTEE         = 3,
	WORD_STORAGE         = 3,
	WORD_BASE            = 3,
	WORD_CALLEE          = 3,
	WORD_LABEL           = 1,
	WORD_CONDITION       = 1,
	WORD_TRUE_LABEL      = 2,
	WORD_FALSE_LABEL     = 3,
	WORD_SELECTOR        = 1,
	WORD_DEFAULT         = 2,
};

/* Module walk under specialization. */
struct flow {
	const uint32_t *code;
	size_t n;
	uint32_t bound;
	uint32_t *def;              /* id: defining word offset */
	uint32_t *spec;             /* id: SpecId plus one */
	uint32_t *blocks;           /* reached labels */
	unsigned char *seen;
	size_t top;
	const VkSpecializationInfo *specialization;
};

struct constant {
	int known;
	uint32_t bits;
};

/* Entry point under its specialization. */
struct entry {
	const uint32_t *code;
	size_t words;
	const char *name;
	const VkSpecializationInfo *specialization;
};

static inline uint32_t
op_of(const uint32_t *w)
{
	return w[0] & SPIRV_OPCODE;
}

static inline uint32_t
count_of(const uint32_t *w)
{
	return w[0] >> SPIRV_COUNT_SHIFT;
}

struct constant spirv_known(const struct flow *f, uint32_t id, int depth);
int spirv_writes(const uint32_t *code, size_t words);
int spirv_discards(const struct entry *e);

#endif

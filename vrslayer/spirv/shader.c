#include "passes.h"
#include "spirv.h"

#include <stdlib.h>
#include <string.h>

#define MASK_IDS 4

/* Discarding or unsafe module. */
struct module {
	int unsafe;
	size_t words;
	uint32_t code[];
};

/* Ids decorated SampleMask so far. */
struct scan {
	uint32_t mask_ids[MASK_IDS];
	int nmask;
	int read_native;            /* device: gl_SampleMaskIn per pixel */
	int unsafe;
	int discard;
};

static int
unsafe_capability(uint32_t cap)
{
	return cap == CAP_SAMPLE_RATE_SHADING || cap == CAP_STENCIL_EXPORT
		|| cap == CAP_SAMPLE_INTERLOCK || cap == CAP_PIXEL_INTERLOCK;
}

static void
note_mask(struct scan *s, uint32_t id)
{
	if (s->nmask == MASK_IDS) {
		s->unsafe = 1;
		return;
	}
	s->mask_ids[s->nmask++] = id;
}

static void
note_builtin(struct scan *s, const uint32_t *w, uint32_t count)
{
	if (count <= WORD_BUILTIN || w[WORD_DECORATION] != DECORATION_BUILTIN)
		return;
	switch (w[WORD_BUILTIN]) {
	case BUILTIN_SAMPLE_MASK:
		note_mask(s, w[WORD_TARGET]);
		return;
	case BUILTIN_SAMPLE_ID:
	case BUILTIN_SAMPLE_POSITION:
	case BUILTIN_FRAG_DEPTH:
	case BUILTIN_FRAG_STENCIL:
		s->unsafe = 1;
		return;
	default:
		return;
	}
}

/* Written mask, or limited read. */
static int
mask_native(const struct scan *s, uint32_t id, uint32_t storage)
{
	int i = 0;

	if (storage != STORAGE_OUTPUT && (storage != STORAGE_INPUT || !s->read_native))
		return 0;
	while (i < s->nmask && s->mask_ids[i] != id)
		i++;
	return i < s->nmask;
}

static void
scan_op(struct scan *s, const uint32_t *w, uint32_t count)
{
	switch (op_of(w)) {
	case OP_KILL:
	case OP_TERMINATE_INVOCATION:
	case OP_DEMOTE_TO_HELPER:
		s->discard = 1;
		return;
	case OP_CAPABILITY:
		s->unsafe |= count > WORD_CAPABILITY
			&& unsafe_capability(w[WORD_CAPABILITY]);
		return;
	case OP_EXECUTION_MODE:
		s->unsafe |= count > WORD_MODE && (w[WORD_MODE] == MODE_DEPTH_REPLACING
			|| w[WORD_MODE] == MODE_STENCIL_REPLACING);
		return;
	case OP_DECORATE:
		note_builtin(s, w, count);
		return;
	case OP_VARIABLE:
		s->unsafe |= count > WORD_STORAGE
			&& mask_native(s, w[WORD_RESULT], w[WORD_STORAGE]);
		return;
	default:
		return;
	}
}

/* Word count; 0 when truncated. */
static uint32_t
length_at(const uint32_t *code, size_t n, size_t i)
{
	uint32_t count = count_of(code + i);

	return count <= n - i ? count : 0;
}

/* Unsafe exports; discard needs flow. */
static enum verdict
verdict(const struct device *dev, const uint32_t *code, size_t n)
{
	size_t i = SPIRV_HEADER;
	struct scan s = { .read_native = (dev->limits.native & NATIVE_SHADER_MASK) != 0 };
	uint32_t count;

	if (!code || n < SPIRV_HEADER || code[0] != SPIRV_MAGIC)
		return SPIRV_UNSAFE;
	for (; i < n && !s.unsafe && (count = length_at(code, n, i)); i += count)
		scan_op(&s, code + i, count);
	if (i < n || s.unsafe || spirv_writes(code, n))
		return SPIRV_UNSAFE;
	return s.discard ? SPIRV_DISCARD : SPIRV_SAFE;
}

static struct module *
keep(const VkShaderModuleCreateInfo *create, enum verdict v)
{
	size_t bytes = v == SPIRV_DISCARD ? create->codeSize : 0;
	struct module *m = malloc(sizeof(*m) + bytes);

	if (!m)
		return NULL;
	m->unsafe = v == SPIRV_UNSAFE;
	m->words = bytes / sizeof(*m->code);
	if (bytes)
		memcpy(m->code, create->pCode, bytes);
	return m;
}

/* Reached discards: capped, else native. */
static enum verdict
discard_verdict(const struct device *dev, const struct entry *e)
{
	if (!spirv_discards(e))
		return SPIRV_SAFE;
	return dev->limits.native & NATIVE_DISCARD ? SPIRV_UNSAFE : SPIRV_DISCARD;
}

/* Verdict, then flow for discards. */
static enum verdict
entry_verdict(const struct device *dev, const struct entry *e)
{
	enum verdict v = verdict(dev, e->code, e->words);

	return v == SPIRV_DISCARD ? discard_verdict(dev, e) : v;
}

/* Specialization decides kept modules. */
static enum verdict
module_verdict(struct device *dev, const VkPipelineShaderStageCreateInfo *st)
{
	const struct module *m = (const struct module *)(uintptr_t)
		handlemap_get(&dev->passes->shaders, (uint64_t)st->module);
	struct entry e = { .name = st->pName, .specialization = st->pSpecializationInfo };

	if (atomic_load(&dev->passes->blind))
		return SPIRV_UNSAFE;
	if (!m)
		return SPIRV_SAFE;
	e.code = m->code;
	e.words = m->words;
	return m->unsafe ? SPIRV_UNSAFE : discard_verdict(dev, &e);
}

static enum verdict
stage_verdict(struct device *dev, const VkPipelineShaderStageCreateInfo *st)
{
	const VkShaderModuleCreateInfo *code = (const void *)
		chain_find(st->pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
	struct entry e = { .name = st->pName, .specialization = st->pSpecializationInfo };

	if (st->module)
		return module_verdict(dev, st);
	if (!code)
		return SPIRV_UNSAFE;
	e.code = code->pCode;
	e.words = code->codeSize / sizeof(*code->pCode);
	return entry_verdict(dev, &e);
}

enum verdict
shader_fragment_verdict(struct device *dev, const VkGraphicsPipelineCreateInfo *in)
{
	uint32_t i = 0;

	while (i < in->stageCount && in->pStages[i].stage != VK_SHADER_STAGE_FRAGMENT_BIT)
		i++;
	return i < in->stageCount ? stage_verdict(dev, &in->pStages[i]) : SPIRV_SAFE;
}

/* Binary code is opaque: unsafe. */
enum verdict
shader_object_verdict(const struct device *dev, const VkShaderCreateInfoEXT *in)
{
	struct entry e = { in->pCode, in->codeSize / sizeof(uint32_t), in->pName,
		in->pSpecializationInfo };

	if (in->codeType != VK_SHADER_CODE_TYPE_SPIRV_EXT)
		return SPIRV_UNSAFE;
	return entry_verdict(dev, &e);
}

void
shader_drop(uint64_t value, void *dev)
{
	(void)dev;
	free((void *)(uintptr_t)value);
}

VKAPI_ATTR VkResult VKAPI_CALL
shader_create(VkDevice device, const VkShaderModuleCreateInfo *create,
	const VkAllocationCallbacks *alloc, VkShaderModule *out)
{
	struct device *dev = device_of(device);
	VkResult created = dev->vk.CreateShaderModule(device, create, alloc, out);
	enum verdict v = created == VK_SUCCESS
		? verdict(dev, create->pCode, create->codeSize / sizeof(*create->pCode))
		: SPIRV_SAFE;
	struct module *m = v == SPIRV_SAFE ? NULL : keep(create, v);

	if (v == SPIRV_SAFE)
		return created;
	if (!m || !handlemap_put(&dev->passes->shaders, (uint64_t)*out,
			(uint64_t)(uintptr_t)m)) {
		free(m);
		atomic_store(&dev->passes->blind, 1);
	}
	return created;
}

VKAPI_ATTR void VKAPI_CALL
shader_destroy(VkDevice device, VkShaderModule module,
	const VkAllocationCallbacks *alloc)
{
	struct device *dev = device_of(device);

	if (module)
		free((void *)(uintptr_t)handlemap_take(&dev->passes->shaders,
			(uint64_t)module));
	dev->vk.DestroyShaderModule(device, module, alloc);
}

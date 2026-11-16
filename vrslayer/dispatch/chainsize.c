#include "layer.h"

/* Copyable size, else 0. */
size_t
chain_size(const struct chain_kind *kinds, size_t count, VkStructureType type)
{
	size_t i = 0;

	while (i < count && kinds[i].type != type)
		i++;
	return i < count && kinds[i].size <= NODE_BYTES ? kinds[i].size : 0;
}

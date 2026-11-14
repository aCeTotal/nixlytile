#ifndef PLATFORM_CONTAINER_MEM_H
#define PLATFORM_CONTAINER_MEM_H

#include <stdbool.h>
#include <stddef.h>

// Stores the copy in *(void **)out.
bool memdup(void *out, const void *src, size_t size);

#endif

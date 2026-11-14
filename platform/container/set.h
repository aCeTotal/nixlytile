#ifndef PLATFORM_CONTAINER_SET_H
#define PLATFORM_CONTAINER_SET_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

// Index of target, -1 when full.
ssize_t set_add(uint32_t values[], size_t *len, size_t cap, uint32_t target);
// Moves the last value into the gap.
ssize_t set_remove(uint32_t values[], size_t *len, size_t cap, uint32_t target);

#endif

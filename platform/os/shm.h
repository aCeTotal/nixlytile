#ifndef PLATFORM_OS_SHM_H
#define PLATFORM_OS_SHM_H

#include <stdbool.h>
#include <stddef.h>

// Anonymous shared memory, -1 on failure.
int allocate_shm_file(size_t size);
// The read-only fd cannot be reopened writable.
bool allocate_shm_file_pair(size_t size, int *rw_fd, int *ro_fd);

#endif

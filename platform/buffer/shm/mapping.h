#ifndef PLATFORM_BUFFER_SHM_MAPPING_H
#define PLATFORM_BUFFER_SHM_MAPPING_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

/* Outlives pool resizes during access */
struct nx_shm_mapping {
	void *data;
	size_t size;
	bool dropped; // no pool references it
};

/* One in-flight data access */
struct nx_shm_sigbus_data {
	struct nx_shm_mapping *mapping;
	struct sigaction prev_action;
	struct nx_shm_sigbus_data *_Atomic next;
};

struct nx_shm_mapping *nx_shm_mapping_create(int fd, size_t size);

/* Pool lets go; may destroy */
void nx_shm_mapping_drop(struct nx_shm_mapping *mapping);

/* Survives clients shrinking the file */
bool nx_shm_mapping_begin_access(struct nx_shm_mapping *mapping,
	struct nx_shm_sigbus_data *access);
void nx_shm_mapping_end_access(struct nx_shm_sigbus_data *access);

#endif

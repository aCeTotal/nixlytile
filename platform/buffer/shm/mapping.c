#undef _POSIX_C_SOURCE
#define _DEFAULT_SOURCE // for MAP_ANONYMOUS
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "platform/buffer/shm/mapping.h"
#include "platform/log/log.h"

#if ATOMIC_POINTER_LOCK_FREE != 2
#error "Lock-free C11 atomic pointers are required"
#endif

// Read from the SIGBUS handler
static struct nx_shm_sigbus_data *_Atomic sigbus_data = NULL;

struct nx_shm_mapping *nx_shm_mapping_create(int fd, size_t size) {
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		nx_log_errno(NX_DEBUG, "mmap failed");
		return NULL;
	}

	struct nx_shm_mapping *mapping = calloc(1, sizeof(*mapping));
	if (mapping == NULL) {
		munmap(data, size);
		return NULL;
	}

	mapping->data = data;
	mapping->size = size;
	return mapping;
}

static void mapping_consider_destroy(struct nx_shm_mapping *mapping) {
	if (!mapping->dropped) {
		return;
	}

	for (struct nx_shm_sigbus_data *cur = sigbus_data; cur != NULL; cur = cur->next) {
		if (cur->mapping == mapping) {
			return;
		}
	}

	munmap(mapping->data, mapping->size);
	free(mapping);
}

void nx_shm_mapping_drop(struct nx_shm_mapping *mapping) {
	if (mapping == NULL) {
		return;
	}

	mapping->dropped = true;
	mapping_consider_destroy(mapping);
}

static struct nx_shm_mapping *mapping_at(uintptr_t addr) {
	for (struct nx_shm_sigbus_data *data = sigbus_data; data != NULL; data = data->next) {
		uintptr_t mapping_start = (uintptr_t)data->mapping->data;
		size_t mapping_size = data->mapping->size;
		if (addr >= mapping_start && addr < mapping_start + mapping_size) {
			return data->mapping;
		}
	}
	return NULL;
}

static void handle_sigbus(int sig, siginfo_t *info, void *context) {
	assert(sigbus_data != NULL);
	struct sigaction prev_action = sigbus_data->prev_action;

	// Swap in zero pages, not signal-safe
	struct nx_shm_mapping *mapping = mapping_at((uintptr_t)info->si_addr);
	if (mapping != NULL && mmap(mapping->data, mapping->size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) != MAP_FAILED) {
		return;
	}

	if (prev_action.sa_flags & SA_SIGINFO) {
		prev_action.sa_sigaction(sig, info, context);
		return;
	}
	prev_action.sa_handler(sig);
}

bool nx_shm_mapping_begin_access(struct nx_shm_mapping *mapping,
		struct nx_shm_sigbus_data *access) {
	struct sigaction prev_action;
	if (sigbus_data == NULL) {
		struct sigaction new_action = {
			.sa_sigaction = handle_sigbus,
			.sa_flags = SA_SIGINFO | SA_NODEFER,
		};
		if (sigaction(SIGBUS, &new_action, &prev_action) != 0) {
			nx_log_errno(NX_ERROR, "sigaction failed");
			return false;
		}
	} else {
		prev_action = sigbus_data->prev_action;
	}

	*access = (struct nx_shm_sigbus_data){
		.mapping = mapping,
		.prev_action = prev_action,
		.next = sigbus_data,
	};
	sigbus_data = access;
	return true;
}

static void sigbus_data_remove(struct nx_shm_sigbus_data *access) {
	if (sigbus_data == access) {
		sigbus_data = access->next;
		return;
	}
	for (struct nx_shm_sigbus_data *cur = sigbus_data; cur != NULL; cur = cur->next) {
		if (cur->next == access) {
			cur->next = access->next;
			return;
		}
	}
}

void nx_shm_mapping_end_access(struct nx_shm_sigbus_data *access) {
	sigbus_data_remove(access);

	if (sigbus_data == NULL && sigaction(SIGBUS, &access->prev_action, NULL) != 0) {
		nx_log_errno(NX_ERROR, "sigaction failed");
	}

	mapping_consider_destroy(access->mapping);
}

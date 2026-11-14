#ifndef PLATFORM_BACKEND_BACKEND_INTERFACE_H
#define PLATFORM_BACKEND_BACKEND_INTERFACE_H

#include <stdbool.h>
#include "platform/backend/backend.h"

struct nx_backend_impl {
	bool (*start)(struct nx_backend *backend);
	void (*destroy)(struct nx_backend *backend);
	int (*get_drm_fd)(struct nx_backend *backend);
};

void nx_backend_init(struct nx_backend *backend,
		const struct nx_backend_impl *impl);
// Emits destroy; listeners must be gone.
void nx_backend_finish(struct nx_backend *backend);

#endif

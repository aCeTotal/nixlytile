#ifndef PLATFORM_BACKEND_PRIMARY_WATCH_H
#define PLATFORM_BACKEND_PRIMARY_WATCH_H

#include <stdbool.h>
#include "platform/backend/backend.h"

// Destroys multi when primary dies.
bool backend_watch_primary(struct nx_backend *multi, struct nx_backend *primary);

#endif

#ifndef PLATFORM_RENDER_ALLOCATOR_GBM_H
#define PLATFORM_RENDER_ALLOCATOR_GBM_H

struct nx_allocator;

/* Owns drm_fd on success */
struct nx_allocator *nx_gbm_allocator_create(int drm_fd);

#endif

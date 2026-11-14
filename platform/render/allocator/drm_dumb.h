#ifndef PLATFORM_RENDER_ALLOCATOR_DRM_DUMB_H
#define PLATFORM_RENDER_ALLOCATOR_DRM_DUMB_H

struct nx_allocator;

/* Owns drm_fd on success */
struct nx_allocator *nx_drm_dumb_allocator_create(int drm_fd);

#endif

#ifndef PLATFORM_RENDER_DMABUF_H
#define PLATFORM_RENDER_DMABUF_H

#include <stdbool.h>
#include <stdint.h>

#define NX_DMABUF_MAX_PLANES 4

/* INVALID modifier means implicit layout */
struct nx_dmabuf_attributes {
	int32_t width, height;
	uint32_t format; // DRM fourcc
	uint64_t modifier;

	int n_planes;
	uint32_t offset[NX_DMABUF_MAX_PLANES];
	uint32_t stride[NX_DMABUF_MAX_PLANES];
	int fd[NX_DMABUF_MAX_PLANES];
};

/* Closes every plane FD */
void nx_dmabuf_attributes_finish(struct nx_dmabuf_attributes *attribs);

bool dmabuf_check_sync_file_import_export(void);

bool dmabuf_import_sync_file(int dmabuf_fd, uint32_t flags, int sync_file_fd);

/* Returns -1 on failure */
int dmabuf_export_sync_file(int dmabuf_fd, uint32_t flags);

#endif

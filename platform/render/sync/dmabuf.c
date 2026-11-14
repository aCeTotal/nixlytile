#include <linux/dma-buf.h>
#include <linux/version.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <xf86drm.h>
#include "platform/log/log.h"
#include "platform/render/dmabuf.h"

void nx_dmabuf_attributes_finish(struct nx_dmabuf_attributes *attribs) {
	for (int i = 0; i < attribs->n_planes; ++i) {
		close(attribs->fd[i]);
		attribs->fd[i] = -1;
	}
	attribs->n_planes = 0;
}

/* Only the kernel version tells */
bool dmabuf_check_sync_file_import_export(void) {
	struct utsname utsname = {0};
	if (uname(&utsname) != 0) {
		nx_log_errno(NX_ERROR, "uname failed");
		return false;
	}

	if (strcmp(utsname.sysname, "Linux") != 0) {
		return false;
	}

	for (size_t i = 0; utsname.release[i] != '\0'; i++) {
		char ch = utsname.release[i];
		if ((ch < '0' || ch > '9') && ch != '.') {
			utsname.release[i] = '\0';
			break;
		}
	}

	char *rel = strtok(utsname.release, ".");
	int major = atoi(rel);

	int minor = 0;
	rel = strtok(NULL, ".");
	if (rel != NULL) {
		minor = atoi(rel);
	}

	int patch = 0;
	rel = strtok(NULL, ".");
	if (rel != NULL) {
		patch = atoi(rel);
	}

	return KERNEL_VERSION(major, minor, patch) >= KERNEL_VERSION(5, 20, 0);
}

bool dmabuf_import_sync_file(int dmabuf_fd, uint32_t flags, int sync_file_fd) {
	struct dma_buf_import_sync_file data = {
		.flags = flags,
		.fd = sync_file_fd,
	};
	if (drmIoctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &data) != 0) {
		nx_log_errno(NX_ERROR, "drmIoctl(IMPORT_SYNC_FILE) failed");
		return false;
	}
	return true;
}

int dmabuf_export_sync_file(int dmabuf_fd, uint32_t flags) {
	struct dma_buf_export_sync_file data = {
		.flags = flags,
		.fd = -1,
	};
	if (drmIoctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &data) != 0) {
		nx_log_errno(NX_ERROR, "drmIoctl(EXPORT_SYNC_FILE) failed");
		return -1;
	}
	return data.fd;
}

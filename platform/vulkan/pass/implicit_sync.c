#include <assert.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "platform/buffer/buffer.h"
#include "platform/log/log.h"
#include "platform/render/dmabuf.h"
#include "platform/render/drm_syncobj.h"
#include "platform/vulkan/image/render_buffer.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/pass/pass.h"
#include "platform/vulkan/queue/command_buffer.h"
#include "platform/vulkan/renderer.h"

#define FENCE_TIMEOUT_MS 100

// Blocking fallback without sync_file interop.
static bool wait_dmabuf_fences(const struct nx_dmabuf_attributes *dmabuf, uint32_t flags) {
	for (int i = 0; i < dmabuf->n_planes; i++) {
		struct pollfd pollfd = {
			.fd = dmabuf->fd[i],
			.events = (flags & DMA_BUF_SYNC_WRITE) ? POLLOUT : POLLIN,
		};
		int ret = poll(&pollfd, 1, FENCE_TIMEOUT_MS);
		if (ret < 0) {
			nx_log_errno(NX_ERROR, "Failed to wait for DMA-BUF fence");
			return false;
		}
		if (ret == 0) {
			nx_log(NX_ERROR, "Timed out while waiting for DMA-BUF fence");
			return false;
		}
	}
	return true;
}

static bool buffer_export_sync_file(struct nx_vk_renderer *renderer, struct nx_buffer *buffer,
		uint32_t flags, int sync_file_fds[static NX_DMABUF_MAX_PLANES]) {
	struct nx_dmabuf_attributes dmabuf = {0};
	if (!nx_buffer_get_dmabuf(buffer, &dmabuf)) {
		nx_log(NX_ERROR, "nx_buffer_get_dmabuf() failed");
		return false;
	}

	if (!renderer->dev->implicit_sync_interop) {
		return wait_dmabuf_fences(&dmabuf, flags);
	}

	for (int i = 0; i < dmabuf.n_planes; i++) {
		int sync_file_fd = dmabuf_export_sync_file(dmabuf.fd[i], flags);
		if (sync_file_fd < 0) {
			nx_log(NX_ERROR, "Failed to extract DMA-BUF fence");
			return false;
		}
		sync_file_fds[i] = sync_file_fd;
	}
	return true;
}

bool vulkan_sync_foreign_texture_acquire(struct nx_vk_texture *texture,
		int sync_file_fds[static NX_DMABUF_MAX_PLANES]) {
	return buffer_export_sync_file(texture->renderer, texture->buffer,
		DMA_BUF_SYNC_READ, sync_file_fds);
}

bool vulkan_sync_render_buffer_acquire(struct nx_vk_render_buffer *render_buffer,
		int sync_file_fds[static NX_DMABUF_MAX_PLANES]) {
	return buffer_export_sync_file(render_buffer->renderer, render_buffer->nx_buffer,
		DMA_BUF_SYNC_WRITE, sync_file_fds);
}

static bool buffer_import_sync_file(struct nx_buffer *buffer, uint32_t flags,
		int sync_file_fd) {
	struct nx_dmabuf_attributes dmabuf = {0};
	if (!nx_buffer_get_dmabuf(buffer, &dmabuf)) {
		nx_log(NX_ERROR, "nx_buffer_get_dmabuf() failed");
		return false;
	}

	for (int i = 0; i < dmabuf.n_planes; i++) {
		if (!dmabuf_import_sync_file(dmabuf.fd[i], flags, sync_file_fd)) {
			return false;
		}
	}
	return true;
}

static bool import_into_buffers(struct nx_vk_render_pass *pass, int sync_file_fd) {
	if (!buffer_import_sync_file(pass->render_buffer->nx_buffer, DMA_BUF_SYNC_WRITE,
			sync_file_fd)) {
		return false;
	}

	struct nx_vk_render_pass_texture *pass_texture;
	wl_array_for_each(pass_texture, &pass->textures) {
		if (!buffer_import_sync_file(pass_texture->texture->buffer, DMA_BUF_SYNC_READ,
				sync_file_fd)) {
			return false;
		}
	}
	return true;
}

bool vulkan_sync_render_pass_release(struct nx_vk_render_pass *pass) {
	struct nx_vk_renderer *renderer = pass->renderer;
	struct nx_vk_command_buffer *cb = pass->command_buffer;

	// No interop nor timeline: block.
	if (!renderer->dev->implicit_sync_interop && pass->signal_timeline == NULL) {
		return vulkan_wait_command_buffer(cb, renderer);
	}

	assert(cb->binary_semaphore != VK_NULL_HANDLE);

	// Exporting resets the semaphore.
	const VkSemaphoreGetFdInfoKHR get_fence_fd_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
		.semaphore = cb->binary_semaphore,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	int sync_file_fd = -1;
	VkResult res = renderer->dev->api.vkGetSemaphoreFdKHR(renderer->dev->dev,
		&get_fence_fd_info, &sync_file_fd);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkGetSemaphoreFdKHR", res);
		return false;
	}

	bool ok = pass->signal_timeline != NULL ?
		nx_drm_syncobj_timeline_import_sync_file(pass->signal_timeline,
			pass->signal_point, sync_file_fd) :
		import_into_buffers(pass, sync_file_fd);
	close(sync_file_fd);
	return ok;
}

static VkSemaphore *wait_semaphore_slot(struct nx_vk_command_buffer *cb, size_t index) {
	VkSemaphore *semaphores = cb->wait_semaphores.data;
	size_t len = cb->wait_semaphores.size / sizeof(semaphores[0]);
	if (index < len) {
		return &semaphores[index];
	}

	VkSemaphore *slot = wl_array_add(&cb->wait_semaphores, sizeof(*slot));
	if (slot != NULL) {
		*slot = VK_NULL_HANDLE;
	}
	return slot;
}

// Slot semaphores are reused.
VkSemaphore vulkan_command_buffer_wait_sync_file(struct nx_vk_renderer *renderer,
		struct nx_vk_command_buffer *cb, size_t sem_index, int sync_file_fd) {
	VkSemaphore *sem = wait_semaphore_slot(cb, sem_index);
	if (sem == NULL) {
		return VK_NULL_HANDLE;
	}

	VkResult res;
	if (*sem == VK_NULL_HANDLE) {
		VkSemaphoreCreateInfo semaphore_info = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};
		res = vkCreateSemaphore(renderer->dev->dev, &semaphore_info, NULL, sem);
		if (res != VK_SUCCESS) {
			nx_vk_error("vkCreateSemaphore", res);
			return VK_NULL_HANDLE;
		}
	}

	VkImportSemaphoreFdInfoKHR import_info = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
		.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
		.semaphore = *sem,
		.fd = sync_file_fd,
	};
	res = renderer->dev->api.vkImportSemaphoreFdKHR(renderer->dev->dev, &import_info);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkImportSemaphoreFdKHR", res);
		return VK_NULL_HANDLE;
	}
	return *sem;
}

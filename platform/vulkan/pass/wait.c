#include <unistd.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/render/drm_syncobj.h"
#include "platform/vulkan/pass/pass.h"
#include "platform/vulkan/renderer.h"

static void clear_fds(int fds[static NX_DMABUF_MAX_PLANES]) {
	for (size_t i = 0; i < NX_DMABUF_MAX_PLANES; i++) {
		fds[i] = -1;
	}
}

// Takes ownership of the sync_files.
static void add_sync_file_waits(struct nx_vk_render_pass *pass, struct nx_vk_wait_list *waits,
		const int fds[static NX_DMABUF_MAX_PLANES], VkPipelineStageFlags2KHR stage) {
	for (size_t i = 0; i < NX_DMABUF_MAX_PLANES; i++) {
		if (fds[i] < 0) {
			continue;
		}

		VkSemaphore sem = vulkan_command_buffer_wait_sync_file(pass->renderer,
			pass->command_buffer, waits->len, fds[i]);
		if (sem == VK_NULL_HANDLE) {
			close(fds[i]);
			continue;
		}

		waits->infos[waits->len++] = (VkSemaphoreSubmitInfoKHR){
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR,
			.semaphore = sem,
			.stageMask = stage,
		};
	}
}

static bool export_texture_fences(const struct nx_vk_render_pass_texture *pass_texture,
		int fds[static NX_DMABUF_MAX_PLANES]) {
	if (pass_texture->wait_timeline == NULL) {
		bool ok = vulkan_sync_foreign_texture_acquire(pass_texture->texture, fds);
		if (!ok) {
			nx_log(NX_ERROR, "Failed to wait for foreign texture DMA-BUF fence");
		}
		return ok;
	}

	fds[0] = nx_drm_syncobj_timeline_export_sync_file(pass_texture->wait_timeline,
		pass_texture->wait_point);
	if (fds[0] < 0) {
		nx_log(NX_ERROR, "Failed to export wait timeline point as sync_file");
		return false;
	}
	return true;
}

static bool wait_render_buffer(struct nx_vk_render_pass *pass,
		struct nx_vk_wait_list *waits) {
	// Explicit sync never polls.
	if (pass->signal_timeline != NULL && !pass->renderer->dev->implicit_sync_interop) {
		return true;
	}

	int fds[NX_DMABUF_MAX_PLANES];
	clear_fds(fds);
	if (!vulkan_sync_render_buffer_acquire(pass->render_buffer, fds)) {
		return false;
	}
	add_sync_file_waits(pass, waits, fds, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR);
	return true;
}

void vulkan_collect_pass_waits(struct nx_vk_render_pass *pass, struct nx_vk_wait_list *waits) {
	struct nx_vk_render_pass_texture *pass_texture;
	wl_array_for_each(pass_texture, &pass->textures) {
		int fds[NX_DMABUF_MAX_PLANES];
		clear_fds(fds);
		if (export_texture_fences(pass_texture, fds)) {
			add_sync_file_waits(pass, waits, fds,
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT_KHR);
		}
	}

	if (!wait_render_buffer(pass, waits)) {
		nx_log(NX_ERROR, "Failed to wait for render buffer DMA-BUF fence");
	}
}

#ifndef NX_VULKAN_PASS_PASS_H
#define NX_VULKAN_PASS_PASS_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <wayland-util.h>
#include "platform/geometry/rect_union.h"
#include "platform/render/dmabuf.h"
#include "platform/render/renderer_interface.h"

struct nx_vk_renderer;
struct nx_vk_render_buffer;
struct nx_vk_render_target;
struct nx_vk_render_format_setup;
struct nx_vk_command_buffer;
struct nx_vk_texture;
struct nx_drm_syncobj_timeline;

struct nx_vk_render_pass_texture {
	struct nx_vk_texture *texture;

	struct nx_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
};

struct nx_vk_render_pass {
	struct nx_render_pass base;
	struct nx_vk_renderer *renderer;
	struct nx_vk_render_buffer *render_buffer;
	struct nx_vk_render_target *render_target;
	struct nx_vk_render_format_setup *render_setup;
	struct nx_vk_command_buffer *command_buffer;
	struct rect_union updated_region;
	VkPipeline bound_pipeline;
	float projection[9];
	bool failed;
	bool two_pass; // through the blend image
	struct nx_color_transform *color_transform;

	struct nx_drm_syncobj_timeline *signal_timeline;
	uint64_t signal_point;

	struct wl_array textures; // nx_vk_render_pass_texture
};

struct nx_vk_render_pass *vulkan_begin_render_pass(struct nx_vk_renderer *renderer,
	struct nx_vk_render_buffer *buffer, const struct nx_buffer_pass_options *options);
struct nx_vk_render_pass *vulkan_get_render_pass(struct nx_render_pass *nx_pass);
void vulkan_render_pass_destroy(struct nx_vk_render_pass *pass);

bool vulkan_render_pass_submit(struct nx_render_pass *nx_pass);
void vulkan_render_pass_add_rect(struct nx_render_pass *nx_pass,
	const struct nx_render_rect_options *options);
void vulkan_render_pass_add_texture(struct nx_render_pass *nx_pass,
	const struct nx_render_texture_options *options);

// Semaphores the render submit waits on.
struct nx_vk_wait_list {
	VkSemaphoreSubmitInfoKHR *infos;
	uint32_t len;
};

bool vulkan_record_output_subpass(struct nx_vk_render_pass *pass);
bool vulkan_record_pass_barriers(struct nx_vk_render_pass *pass,
	struct nx_vk_command_buffer *stage_cb);
// Infos holds a slot per plane.
void vulkan_collect_pass_waits(struct nx_vk_render_pass *pass, struct nx_vk_wait_list *waits);

bool vulkan_sync_foreign_texture_acquire(struct nx_vk_texture *texture,
	int sync_file_fds[static NX_DMABUF_MAX_PLANES]);
bool vulkan_sync_render_buffer_acquire(struct nx_vk_render_buffer *render_buffer,
	int sync_file_fds[static NX_DMABUF_MAX_PLANES]);
bool vulkan_sync_render_pass_release(struct nx_vk_render_pass *pass);
VkSemaphore vulkan_command_buffer_wait_sync_file(struct nx_vk_renderer *renderer,
	struct nx_vk_command_buffer *cb, size_t sem_index, int sync_file_fd);

#endif

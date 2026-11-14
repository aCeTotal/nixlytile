#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/image/texture.h"
#include "platform/vulkan/renderer.h"

static const VkExternalMemoryHandleTypeFlagBits dmabuf_handle_type =
	VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

struct dmabuf_import {
	struct nx_vk_renderer *renderer;
	const struct nx_dmabuf_attributes *attribs;
	const struct nx_vk_format *format;
	const struct nx_vk_format_modifier_props *mod;
	enum nx_vk_modifier_usage usage;
	bool disjoint;
	VkImage image;
};

static VkImageAspectFlagBits mem_plane_aspect(uint32_t plane) {
	switch (plane) {
	case 0: return VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
	case 1: return VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT;
	case 2: return VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT;
	case 3: return VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
	default: abort(); // unreachable
	}
}

static bool is_dmabuf_disjoint(const struct nx_dmabuf_attributes *attribs) {
	if (attribs->n_planes == 1) {
		return false;
	}

	struct stat first_stat;
	if (fstat(attribs->fd[0], &first_stat) != 0) {
		nx_log_errno(NX_ERROR, "fstat failed");
		return true;
	}

	for (int i = 1; i < attribs->n_planes; i++) {
		struct stat plane_stat;
		if (fstat(attribs->fd[i], &plane_stat) != 0) {
			nx_log_errno(NX_ERROR, "fstat failed");
			return true;
		}
		if (first_stat.st_ino != plane_stat.st_ino) {
			return true;
		}
	}
	return false;
}

static bool create_image(struct dmabuf_import *import) {
	const struct nx_dmabuf_attributes *attribs = import->attribs;
	VkFormat view_formats[] = { import->format->vk, import->format->vk_srgb };
	VkImageFormatListCreateInfoKHR list_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
		.pViewFormats = view_formats,
		.viewFormatCount = sizeof(view_formats) / sizeof(view_formats[0]),
	};

	VkSubresourceLayout plane_layouts[NX_DMABUF_MAX_PLANES] = {0};
	for (int i = 0; i < attribs->n_planes; ++i) {
		plane_layouts[i].offset = attribs->offset[i];
		plane_layouts[i].rowPitch = attribs->stride[i];
	}
	VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.pNext = import->mod->has_mutable_srgb ? &list_info : NULL,
		.drmFormatModifierPlaneCount = attribs->n_planes,
		.drmFormatModifier = import->mod->props.drmFormatModifier,
		.pPlaneLayouts = plane_layouts,
	};
	VkExternalMemoryImageCreateInfo eimg = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext = &mod_info,
		.handleTypes = dmabuf_handle_type,
	};

	VkImageCreateFlags flags = 0;
	if (import->disjoint) {
		flags |= VK_IMAGE_CREATE_DISJOINT_BIT;
	}
	if (import->mod->has_mutable_srgb) {
		flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	}
	VkImageCreateInfo img_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &eimg,
		.flags = flags,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = import->format->vk,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.extent = (VkExtent3D){ attribs->width, attribs->height, 1 },
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = vulkan_dmabuf_image_usage(import->usage),
	};

	VkResult res = vkCreateImage(import->renderer->dev->dev, &img_info, NULL, &import->image);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreateImage", res);
		return false;
	}
	return true;
}


// Plane allocation size and type.
static bool plane_requirements(const struct dmabuf_import *import, uint32_t plane,
		VkMemoryAllocateInfo *alloc) {
	struct nx_vk_device *dev = import->renderer->dev;
	VkMemoryFdPropertiesKHR fdp = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
	};
	VkResult res = dev->api.vkGetMemoryFdPropertiesKHR(dev->dev, dmabuf_handle_type,
		import->attribs->fd[plane], &fdp);
	if (res != VK_SUCCESS) {
		nx_vk_error("getMemoryFdPropertiesKHR", res);
		return false;
	}

	VkImagePlaneMemoryRequirementsInfo planeri = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
		.planeAspect = mem_plane_aspect(plane),
	};
	VkImageMemoryRequirementsInfo2 memri = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
		.pNext = import->disjoint ? &planeri : NULL,
		.image = import->image,
	};
	VkMemoryRequirements2 memr = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
	};
	vkGetImageMemoryRequirements2(dev->dev, &memri, &memr);

	int mem = vulkan_find_mem_type(dev, 0,
		memr.memoryRequirements.memoryTypeBits & fdp.memoryTypeBits);
	if (mem < 0) {
		nx_log(NX_ERROR, "no valid memory type index");
		return false;
	}
	alloc->allocationSize = memr.memoryRequirements.size;
	alloc->memoryTypeIndex = (uint32_t)mem;
	return true;
}

static bool import_plane(const struct dmabuf_import *import, uint32_t plane,
		VkDeviceMemory *memory) {
	VkMemoryAllocateInfo memi = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	};
	if (!plane_requirements(import, plane, &memi)) {
		return false;
	}

	// Vulkan closes the duplicate.
	int dfd = fcntl(import->attribs->fd[plane], F_DUPFD_CLOEXEC, 0);
	if (dfd < 0) {
		nx_log_errno(NX_ERROR, "fcntl(F_DUPFD_CLOEXEC) failed");
		return false;
	}

	VkMemoryDedicatedAllocateInfo dedi = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.image = import->image,
	};
	VkImportMemoryFdInfoKHR importi = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.pNext = &dedi,
		.fd = dfd,
		.handleType = dmabuf_handle_type,
	};
	memi.pNext = &importi;

	VkResult res = vkAllocateMemory(import->renderer->dev->dev, &memi, NULL, memory);
	if (res != VK_SUCCESS) {
		close(dfd);
		nx_vk_error("vkAllocateMemory failed", res);
		return false;
	}
	return true;
}

static bool bind_planes(const struct dmabuf_import *import, struct nx_vk_dmabuf_image *out) {
	uint32_t mem_count = import->disjoint ? (uint32_t)import->attribs->n_planes : 1u;
	VkBindImageMemoryInfo bindi[NX_DMABUF_MAX_PLANES] = {0};
	VkBindImagePlaneMemoryInfo planei[NX_DMABUF_MAX_PLANES] = {0};

	for (uint32_t i = 0; i < mem_count; ++i) {
		if (!import_plane(import, i, &out->memories[i])) {
			return false;
		}
		out->mem_count++;

		bindi[i] = (VkBindImageMemoryInfo){
			.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
			.image = import->image,
			.memory = out->memories[i],
			.memoryOffset = 0,
		};
		if (!import->disjoint) {
			continue;
		}
		planei[i] = (VkBindImagePlaneMemoryInfo){
			.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
			.planeAspect = mem_plane_aspect(i),
		};
		bindi[i].pNext = &planei[i];
	}

	VkResult res = vkBindImageMemory2(import->renderer->dev->dev, mem_count, bindi);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkBindMemory failed", res);
		return false;
	}
	return true;
}

bool vulkan_import_dmabuf(struct nx_vk_renderer *renderer,
		const struct nx_dmabuf_attributes *attribs, enum nx_vk_modifier_usage usage,
		struct nx_vk_dmabuf_image *out) {
	*out = (struct nx_vk_dmabuf_image){0};

	const struct nx_vk_format_props *fmt =
		vulkan_format_props_from_drm(renderer->dev, attribs->format);
	if (fmt == NULL) {
		vulkan_log_unsupported_format(attribs->format);
		return false;
	}

	const struct nx_vk_format_modifier_props *mod =
		vulkan_format_props_import_modifier(fmt, attribs, usage);
	if (!mod) {
		return false;
	}

	bool disjoint = is_dmabuf_disjoint(attribs);
	if (disjoint && !(mod->props.drmFormatModifierTilingFeatures &
			VK_FORMAT_FEATURE_DISJOINT_BIT)) {
		nx_log(NX_ERROR, "Format/Modifier does not support disjoint images");
		return false;
	}

	struct dmabuf_import import = {
		.renderer = renderer,
		.attribs = attribs,
		.format = &fmt->format,
		.mod = mod,
		.usage = usage,
		.disjoint = disjoint,
	};
	if (!create_image(&import)) {
		return false;
	}

	if (!bind_planes(&import, out)) {
		VkDevice dev = renderer->dev->dev;
		vkDestroyImage(dev, import.image, NULL);
		for (uint32_t i = 0; i < out->mem_count; ++i) {
			vkFreeMemory(dev, out->memories[i], NULL);
		}
		return false;
	}

	out->image = import.image;
	out->has_mutable_srgb = mod->has_mutable_srgb;
	return true;
}

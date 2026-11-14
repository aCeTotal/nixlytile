#include <drm_fourcc.h>
#include <inttypes.h>
#include <stdlib.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "platform/buffer/buffer.h"
#include "platform/container/addon.h"
#include "platform/drm/device/device.h"
#include "platform/drm/device/fb.h"
#include "platform/log/log.h"
#include "platform/render/drm_format_set.h"
#include "platform/render/pixel_format.h"

void drm_fb_clear(struct nx_drm_fb **fb_ptr) {
	if (*fb_ptr == NULL) {
		return;
	}

	struct nx_drm_fb *fb = *fb_ptr;
	nx_buffer_unlock(fb->nx_buf); // may destroy the buffer

	*fb_ptr = NULL;
}

struct nx_drm_fb *drm_fb_lock(struct nx_drm_fb *fb) {
	nx_buffer_lock(fb->nx_buf);
	return fb;
}

static void drm_fb_handle_destroy(struct nx_addon *addon) {
	struct nx_drm_fb *fb = wl_container_of(addon, fb, addon);
	drm_fb_destroy(fb);
}

static const struct nx_addon_interface fb_addon_impl = {
	.name = "nx_drm_fb",
	.destroy = drm_fb_handle_destroy,
};

/* Plain AddFB2 fallback for NVIDIA 590+. */
static uint32_t add_fb_with_modifiers(struct nx_drm_backend *drm,
		const struct nx_dmabuf_attributes *dmabuf, const uint32_t handles[static 4]) {
	// One modifier for all planes
	uint64_t modifiers[4] = {0};
	for (int i = 0; i < dmabuf->n_planes; i++) {
		modifiers[i] = dmabuf->modifier;
	}

	uint32_t id = 0;
	if (drmModeAddFB2WithModifiers(drm->fd, dmabuf->width, dmabuf->height,
			dmabuf->format, handles, dmabuf->stride, dmabuf->offset,
			modifiers, &id, DRM_MODE_FB_MODIFIERS) == 0) {
		return id;
	}
	nx_log_errno(NX_DEBUG, "drmModeAddFB2WithModifiers failed");

	if (drmModeAddFB2(drm->fd, dmabuf->width, dmabuf->height,
			dmabuf->format, handles, dmabuf->stride,
			dmabuf->offset, &id, 0) != 0) {
		nx_log_errno(NX_DEBUG, "drmModeAddFB2 fallback failed");
		return 0;
	}

	nx_log(NX_INFO, "drmModeAddFB2WithModifiers broken on this driver, "
		"using drmModeAddFB2 for all future buffers");
	drm->addfb2_modifiers_broken = true;
	return id;
}

static uint32_t add_fb_implicit(struct nx_drm_backend *drm,
		const struct nx_dmabuf_attributes *dmabuf, const uint32_t handles[static 4]) {
	if (dmabuf->modifier != DRM_FORMAT_MOD_INVALID &&
			dmabuf->modifier != DRM_FORMAT_MOD_LINEAR) {
		nx_log(NX_ERROR, "Cannot import DRM framebuffer with explicit "
			"modifier 0x%"PRIX64, dmabuf->modifier);
		return 0;
	}

	uint32_t id = 0;
	int ret = drmModeAddFB2(drm->fd, dmabuf->width, dmabuf->height,
		dmabuf->format, handles, dmabuf->stride, dmabuf->offset, &id, 0);
	if (ret == 0) {
		return id;
	}
	if (dmabuf->format != DRM_FORMAT_ARGB8888 || dmabuf->n_planes != 1 ||
			dmabuf->offset[0] != 0) {
		nx_log_errno(NX_DEBUG, "drmModeAddFB2 failed");
		return 0;
	}

	// Big-endian fallback, like Xorg's modesetting
	nx_log(NX_DEBUG, "drmModeAddFB2 failed (%s), falling back to "
		"legacy drmModeAddFB", strerror(-ret));
	uint32_t depth = 32;
	uint32_t bpp = 32;
	if (drmModeAddFB(drm->fd, dmabuf->width, dmabuf->height, depth,
			bpp, dmabuf->stride[0], handles[0], &id) != 0) {
		nx_log_errno(NX_DEBUG, "drmModeAddFB failed");
	}
	return id;
}

static bool handle_seen_before(const uint32_t handles[static 4], int i) {
	for (int j = 0; j < i; ++j) {
		if (handles[i] == handles[j]) {
			return true;
		}
	}
	return false;
}

static void close_all_bo_handles(struct nx_drm_backend *drm,
		const uint32_t handles[static 4]) {
	for (int i = 0; i < 4; ++i) {
		if (handles[i] == 0 || handle_seen_before(handles, i)) {
			continue;
		}
		if (drmCloseBufferHandle(drm->fd, handles[i]) != 0) {
			nx_log_errno(NX_ERROR, "drmCloseBufferHandle failed");
		}
	}
}

static void drm_poisoned_fb_handle_destroy(struct nx_addon *addon) {
	nx_addon_finish(addon);
	free(addon);
}

static const struct nx_addon_interface poisoned_fb_addon_impl = {
	.name = "nx_drm_poisoned_fb",
	.destroy = drm_poisoned_fb_handle_destroy,
};

static bool is_buffer_poisoned(struct nx_drm_backend *drm,
		struct nx_buffer *buf) {
	return nx_addon_find(&buf->addons, drm, &poisoned_fb_addon_impl) != NULL;
}

/* Skips re-importing unscanoutable buffers. */
static void poison_buffer(struct nx_drm_backend *drm,
		struct nx_buffer *buf) {
	struct nx_addon *addon = calloc(1, sizeof(*addon));
	if (addon == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return;
	}
	nx_addon_init(addon, &buf->addons, drm, &poisoned_fb_addon_impl);
	nx_log(NX_DEBUG, "Poisoning buffer");
}

/* Falls back to the opaque format. */
static bool pick_scanout_format(const struct nx_drm_format_set *formats,
		struct nx_dmabuf_attributes *attribs) {
	if (nx_drm_format_set_has(formats, attribs->format, attribs->modifier)) {
		return true;
	}

	const struct nx_pixel_format_info *info =
		drm_get_pixel_format_info(attribs->format);
	if (info == NULL || info->opaque_substitute == DRM_FORMAT_INVALID ||
			!nx_drm_format_set_has(formats, info->opaque_substitute, attribs->modifier)) {
		nx_log(NX_DEBUG, "Buffer format 0x%"PRIX32" with modifier "
			"0x%"PRIX64" cannot be scanned out",
			attribs->format, attribs->modifier);
		return false;
	}

	attribs->format = info->opaque_substitute;
	return true;
}

static uint32_t import_dmabuf(struct nx_drm_backend *drm, struct nx_buffer *buf,
		const struct nx_dmabuf_attributes *attribs) {
	uint32_t handles[4] = {0};
	for (int i = 0; i < attribs->n_planes; ++i) {
		if (drmPrimeFDToHandle(drm->fd, attribs->fd[i], &handles[i]) != 0) {
			nx_log_errno(NX_DEBUG, "drmPrimeFDToHandle failed");
			close_all_bo_handles(drm, handles);
			return 0;
		}
	}

	uint32_t id = drm->cap_addfb2_modifiers && !drm->addfb2_modifiers_broken ?
		add_fb_with_modifiers(drm, attribs, handles) :
		add_fb_implicit(drm, attribs, handles);
	if (id == 0) {
		nx_log(NX_DEBUG, "Failed to import BO in KMS");
		poison_buffer(drm, buf);
	}

	close_all_bo_handles(drm, handles);
	return id;
}

static struct nx_drm_fb *drm_fb_create(struct nx_drm_backend *drm,
		struct nx_buffer *buf, const struct nx_drm_format_set *formats) {
	struct nx_dmabuf_attributes attribs;
	if (!nx_buffer_get_dmabuf(buf, &attribs)) {
		nx_log(NX_DEBUG, "Failed to get DMA-BUF from buffer");
		return NULL;
	}

	if (is_buffer_poisoned(drm, buf)) {
		nx_log(NX_DEBUG, "Buffer is poisoned");
		return NULL;
	}

	if (!pick_scanout_format(formats, &attribs)) {
		return NULL;
	}

	struct nx_drm_fb *fb = calloc(1, sizeof(*fb));
	if (!fb) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return NULL;
	}

	fb->id = import_dmabuf(drm, buf, &attribs);
	if (!fb->id) {
		free(fb);
		return NULL;
	}

	fb->backend = drm;
	fb->nx_buf = buf;

	nx_addon_init(&fb->addon, &buf->addons, drm, &fb_addon_impl);
	wl_list_insert(&drm->fbs, &fb->link);

	return fb;
}

void drm_fb_destroy(struct nx_drm_fb *fb) {
	struct nx_drm_backend *drm = fb->backend;

	wl_list_remove(&fb->link);
	nx_addon_finish(&fb->addon);

	int ret = drmModeCloseFB(drm->fd, fb->id);
	if (ret == -EINVAL) {
		ret = drmModeRmFB(drm->fd, fb->id);
	}
	if (ret != 0) {
		nx_log(NX_ERROR, "Failed to close FB: %s", strerror(-ret));
	}

	free(fb);
}

bool drm_fb_import(struct nx_drm_fb **fb_ptr, struct nx_drm_backend *drm,
		struct nx_buffer *buf, const struct nx_drm_format_set *formats) {
	struct nx_drm_fb *fb;
	struct nx_addon *addon = nx_addon_find(&buf->addons, drm, &fb_addon_impl);
	if (addon != NULL) {
		fb = wl_container_of(addon, fb, addon);
	} else {
		fb = drm_fb_create(drm, buf, formats);
	}
	if (!fb) {
		return false;
	}

	nx_buffer_lock(buf);
	drm_fb_move(fb_ptr, &fb);
	return true;
}

void drm_fb_move(struct nx_drm_fb **new, struct nx_drm_fb **old) {
	drm_fb_clear(new);
	*new = *old;
	*old = NULL;
}

void drm_fb_copy(struct nx_drm_fb **new, struct nx_drm_fb *old) {
	drm_fb_clear(new);
	if (old != NULL) {
		*new = drm_fb_lock(old);
	}
}

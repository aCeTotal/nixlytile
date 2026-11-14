#include <assert.h>
#include <stdlib.h>
#include "platform/log/log.h"
#include "platform/render/allocator/allocator.h"
#include "platform/render/swapchain.h"
#include "platform/buffer/buffer.h"
#include "platform/render/drm_format_set.h"

static void swapchain_handle_allocator_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_swapchain *swapchain =
		wl_container_of(listener, swapchain, allocator_destroy);
	swapchain->allocator = NULL;
	wl_list_remove(&swapchain->allocator_destroy.link);
	wl_list_init(&swapchain->allocator_destroy.link);
}

struct nx_swapchain *nx_swapchain_create(
		struct nx_allocator *alloc, int width, int height,
		const struct nx_drm_format *format) {
	assert(width > 0 && height > 0);

	struct nx_swapchain *swapchain = calloc(1, sizeof(*swapchain));
	if (swapchain == NULL) {
		return NULL;
	}
	swapchain->allocator = alloc;
	swapchain->width = width;
	swapchain->height = height;

	if (!nx_drm_format_copy(&swapchain->format, format)) {
		free(swapchain);
		return NULL;
	}

	swapchain->allocator_destroy.notify = swapchain_handle_allocator_destroy;
	wl_signal_add(&alloc->events.destroy, &swapchain->allocator_destroy);

	return swapchain;
}

static void slot_reset(struct nx_swapchain_slot *slot) {
	if (slot->acquired) {
		wl_list_remove(&slot->release.link);
	}
	nx_buffer_drop(slot->buffer);
	*slot = (struct nx_swapchain_slot){0};
}

void nx_swapchain_destroy(struct nx_swapchain *swapchain) {
	if (swapchain == NULL) {
		return;
	}
	for (size_t i = 0; i < NX_SWAPCHAIN_CAP; i++) {
		slot_reset(&swapchain->slots[i]);
	}
	wl_list_remove(&swapchain->allocator_destroy.link);
	nx_drm_format_finish(&swapchain->format);
	free(swapchain);
}

static void slot_handle_release(struct wl_listener *listener, void *data) {
	struct nx_swapchain_slot *slot =
		wl_container_of(listener, slot, release);
	wl_list_remove(&slot->release.link);
	slot->acquired = false;
}

static struct nx_buffer *slot_acquire(struct nx_swapchain_slot *slot) {
	assert(!slot->acquired);
	assert(slot->buffer != NULL);

	slot->acquired = true;

	slot->release.notify = slot_handle_release;
	wl_signal_add(&slot->buffer->events.release, &slot->release);

	return nx_buffer_lock(slot->buffer);
}

struct nx_buffer *nx_swapchain_acquire(struct nx_swapchain *swapchain) {
	struct nx_swapchain_slot *free_slot = NULL;
	for (size_t i = 0; i < NX_SWAPCHAIN_CAP; i++) {
		struct nx_swapchain_slot *slot = &swapchain->slots[i];
		if (slot->acquired) {
			continue;
		}
		if (slot->buffer != NULL) {
			return slot_acquire(slot);
		}
		free_slot = slot;
	}
	if (free_slot == NULL) {
		nx_log(NX_ERROR, "No free output buffer slot");
		return NULL;
	}

	if (swapchain->allocator == NULL) {
		return NULL;
	}

	nx_log(NX_DEBUG, "Allocating new swapchain buffer");
	free_slot->buffer = nx_allocator_create_buffer(swapchain->allocator,
		swapchain->width, swapchain->height, &swapchain->format);
	if (free_slot->buffer == NULL) {
		nx_log(NX_ERROR, "Failed to allocate buffer");
		return NULL;
	}
	return slot_acquire(free_slot);
}

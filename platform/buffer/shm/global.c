#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <wayland-server-protocol.h>
#include "platform/buffer/buffer_interface.h"
#include "platform/buffer/shm/pool.h"
#include "platform/buffer/shm/shm.h"
#include "platform/log/log.h"
#include "platform/render/drm_format_set.h"
#include "platform/render/pixel_format.h"
#include "platform/render/renderer.h"

#define SHM_VERSION 2

static void shm_handle_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_shm_interface shm_impl = {
	.create_pool = nx_shm_create_pool,
	.release = shm_handle_release,
};

static void shm_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct nx_shm *shm = data;

	struct wl_resource *resource = wl_resource_create(client, &wl_shm_interface,
		version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &shm_impl, shm, NULL);

	for (size_t i = 0; i < shm->formats_len; i++) {
		wl_shm_send_format(resource, shm->formats[i]);
	}
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_shm *shm = wl_container_of(listener, shm, display_destroy);
	wl_list_remove(&shm->display_destroy.link);
	wl_global_destroy(shm->global);
	free(shm->formats);
	free(shm);
}

struct nx_shm *nx_shm_create_with_renderer(struct wl_display *display,
		uint32_t version, struct nx_renderer *renderer) {
	const struct nx_drm_format_set *format_set =
		nx_renderer_get_texture_formats(renderer, NX_BUFFER_CAP_DATA_PTR);
	if (format_set == NULL || format_set->len == 0) {
		nx_log(NX_ERROR, "Failed to initialize wl_shm: "
			"cannot get renderer formats");
		return NULL;
	}

	assert(version <= SHM_VERSION);
	// Mandatory wl_shm formats
	assert(nx_drm_format_set_get(format_set, DRM_FORMAT_ARGB8888) != NULL &&
		nx_drm_format_set_get(format_set, DRM_FORMAT_XRGB8888) != NULL);

	struct nx_shm *shm = calloc(1, sizeof(*shm));
	if (shm == NULL) {
		nx_log(NX_ERROR, "Allocation failed");
		return NULL;
	}

	shm->formats_len = format_set->len;
	shm->formats = malloc(shm->formats_len * sizeof(uint32_t));
	if (shm->formats == NULL) {
		nx_log(NX_ERROR, "Allocation failed");
		free(shm);
		return NULL;
	}
	for (size_t i = 0; i < shm->formats_len; i++) {
		shm->formats[i] = convert_drm_format_to_wl_shm(format_set->formats[i].format);
	}

	shm->global = wl_global_create(display, &wl_shm_interface, version,
		shm, shm_bind);
	if (shm->global == NULL) {
		nx_log(NX_ERROR, "wl_global_create failed");
		free(shm->formats);
		free(shm);
		return NULL;
	}

	shm->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &shm->display_destroy);

	nx_buffer_register_resource_interface(&nx_shm_buffer_resource_interface);

	return shm;
}

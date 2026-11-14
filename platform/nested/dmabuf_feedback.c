#include <assert.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <xf86drm.h>
#include "platform/log/log.h"
#include "platform/nested/backend.h"

#include "linux-dmabuf-v1-client-protocol.h"

struct format_table_entry {
	uint32_t format;
	uint32_t pad;
	uint64_t modifier;
};

struct dmabuf_feedback {
	struct nx_wl_backend *backend;
	dev_t main_device_id;
	struct format_table_entry *format_table;
	size_t format_table_size;
	dev_t tranche_target_device_id;
};

static dev_t read_dev_id(const struct wl_array *dev_id_arr) {
	dev_t dev_id;
	assert(dev_id_arr->size == sizeof(dev_id));
	memcpy(&dev_id, dev_id_arr->data, sizeof(dev_id));
	return dev_id;
}

static void feedback_handle_done(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *feedback) {
}

static void feedback_handle_format_table(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *feedback, int fd, uint32_t size) {
	struct dmabuf_feedback *feedback_data = data;

	feedback_data->format_table = NULL;
	void *table = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (table == MAP_FAILED) {
		nx_log_errno(NX_ERROR, "failed to mmap DMA-BUF format table");
	} else {
		feedback_data->format_table = table;
		feedback_data->format_table_size = size;
	}
	close(fd);
}

// Split display/render setups lack render nodes.
static const char *render_node_name(drmDevice *device) {
	if (device->available_nodes & (1 << DRM_NODE_RENDER)) {
		return device->nodes[DRM_NODE_RENDER];
	}
	assert(device->available_nodes & (1 << DRM_NODE_PRIMARY));
	const char *name = device->nodes[DRM_NODE_PRIMARY];
	nx_log(NX_DEBUG, "DRM device %s has no render node, "
		"falling back to primary node", name);
	return name;
}

static void feedback_handle_main_device(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *feedback,
		struct wl_array *dev_id_arr) {
	struct dmabuf_feedback *feedback_data = data;
	dev_t dev_id = read_dev_id(dev_id_arr);
	feedback_data->main_device_id = dev_id;

	drmDevice *device = NULL;
	if (drmGetDeviceFromDevId(dev_id, 0, &device) != 0) {
		nx_log_errno(NX_ERROR, "drmGetDeviceFromDevId failed");
		return;
	}

	struct nx_wl_backend *wl = feedback_data->backend;
	assert(wl->drm_render_name == NULL);
	wl->drm_render_name = strdup(render_node_name(device));
	drmFreeDevice(&device);
}

static void feedback_handle_tranche_done(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *feedback) {
	struct dmabuf_feedback *feedback_data = data;
	feedback_data->tranche_target_device_id = 0;
}

static void feedback_handle_tranche_target_device(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *feedback,
		struct wl_array *dev_id_arr) {
	struct dmabuf_feedback *feedback_data = data;
	feedback_data->tranche_target_device_id = read_dev_id(dev_id_arr);
}

static void feedback_handle_tranche_formats(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *feedback,
		struct wl_array *indices_arr) {
	struct dmabuf_feedback *feedback_data = data;
	if (feedback_data->format_table == NULL) {
		return;
	}
	if (feedback_data->tranche_target_device_id != feedback_data->main_device_id) {
		return;
	}

	size_t table_cap = feedback_data->format_table_size /
		sizeof(struct format_table_entry);
	uint16_t *index_ptr;
	wl_array_for_each(index_ptr, indices_arr) {
		assert(*index_ptr < table_cap);
		const struct format_table_entry *entry =
			&feedback_data->format_table[*index_ptr];
		nx_drm_format_set_add(&feedback_data->backend->linux_dmabuf_v1_formats,
			entry->format, entry->modifier);
	}
}

static void feedback_handle_tranche_flags(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *feedback, uint32_t flags) {
}

static const struct zwp_linux_dmabuf_feedback_v1_listener feedback_listener = {
	.done = feedback_handle_done,
	.format_table = feedback_handle_format_table,
	.main_device = feedback_handle_main_device,
	.tranche_done = feedback_handle_tranche_done,
	.tranche_target_device = feedback_handle_tranche_target_device,
	.tranche_formats = feedback_handle_tranche_formats,
	.tranche_flags = feedback_handle_tranche_flags,
};

bool nested_dmabuf_feedback_query(struct nx_wl_backend *wl) {
	if (wl->zwp_linux_dmabuf_v1 == NULL ||
			zwp_linux_dmabuf_v1_get_version(wl->zwp_linux_dmabuf_v1) <
			ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) {
		return true;
	}

	struct zwp_linux_dmabuf_feedback_v1 *feedback =
		zwp_linux_dmabuf_v1_get_default_feedback(wl->zwp_linux_dmabuf_v1);
	if (feedback == NULL) {
		nx_log(NX_ERROR, "Allocation failed");
		return false;
	}

	struct dmabuf_feedback feedback_data = { .backend = wl };
	zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &feedback_listener,
		&feedback_data);
	wl_display_roundtrip(wl->remote_display);

	if (feedback_data.format_table != NULL) {
		munmap(feedback_data.format_table, feedback_data.format_table_size);
	}
	zwp_linux_dmabuf_feedback_v1_destroy(feedback);
	return true;
}

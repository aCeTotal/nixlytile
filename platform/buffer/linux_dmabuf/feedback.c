#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "linux-dmabuf-v1-protocol.h"
#include "platform/buffer/linux_dmabuf/feedback.h"
#include "platform/log/log.h"
#include "platform/os/shm.h"

struct nx_linux_dmabuf_feedback_v1_compiled_tranche {
	dev_t target_device;
	uint32_t flags; // enum zwp_linux_dmabuf_feedback_v1_tranche_flags bits
	struct wl_array indices; // uint16_t
};

struct nx_linux_dmabuf_feedback_v1_compiled {
	dev_t main_device;
	int table_fd;
	size_t table_size;

	size_t tranches_len;
	struct nx_linux_dmabuf_feedback_v1_compiled_tranche tranches[];
};

struct nx_linux_dmabuf_feedback_v1_table_entry {
	uint32_t format;
	uint32_t pad;
	uint64_t modifier;
};

static_assert(sizeof(struct nx_linux_dmabuf_feedback_v1_table_entry) == 16,
	"Expected nx_linux_dmabuf_feedback_v1_table_entry to be tightly packed");

static ssize_t get_drm_format_set_index(const struct nx_drm_format_set *set,
		uint32_t format, uint64_t modifier) {
	bool format_found = false;
	const struct nx_drm_format *fmt;
	size_t idx = 0;
	for (size_t i = 0; i < set->len; i++) {
		fmt = &set->formats[i];
		if (fmt->format == format) {
			format_found = true;
			break;
		}
		idx += fmt->len;
	}
	if (!format_found) {
		return -1;
	}

	for (size_t i = 0; i < fmt->len; i++) {
		if (fmt->modifiers[i] == modifier) {
			return idx;
		}
		idx++;
	}
	return -1;
}

static size_t format_pair_count(const struct nx_drm_format_set *set) {
	size_t count = 0;
	for (size_t i = 0; i < set->len; i++) {
		count += set->formats[i].len;
	}
	return count;
}

/* Read-only table FD or -1 */
static int create_format_table(const struct nx_drm_format_set *formats,
		size_t table_len) {
	size_t table_size =
		table_len * sizeof(struct nx_linux_dmabuf_feedback_v1_table_entry);
	int rw_fd, ro_fd;
	if (!allocate_shm_file_pair(table_size, &rw_fd, &ro_fd)) {
		nx_log(NX_ERROR, "Failed to allocate shm file for format table");
		return -1;
	}

	struct nx_linux_dmabuf_feedback_v1_table_entry *table =
		mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, rw_fd, 0);
	if (table == MAP_FAILED) {
		nx_log_errno(NX_ERROR, "mmap failed");
		close(rw_fd);
		close(ro_fd);
		return -1;
	}

	close(rw_fd);

	size_t n = 0;
	for (size_t i = 0; i < formats->len; i++) {
		const struct nx_drm_format *fmt = &formats->formats[i];
		for (size_t k = 0; k < fmt->len; k++) {
			table[n++] = (struct nx_linux_dmabuf_feedback_v1_table_entry){
				.format = fmt->format,
				.modifier = fmt->modifiers[k],
			};
		}
	}
	assert(n == table_len);

	munmap(table, table_size);
	return ro_fd;
}

static void compile_tranche_indices(
		struct nx_linux_dmabuf_feedback_v1_compiled_tranche *compiled,
		const struct nx_drm_format_set *formats, const struct nx_drm_format_set *table) {
	uint16_t *indices = compiled->indices.data;
	size_t n = 0;
	for (size_t j = 0; j < formats->len; j++) {
		const struct nx_drm_format *fmt = &formats->formats[j];
		for (size_t k = 0; k < fmt->len; k++) {
			ssize_t index = get_drm_format_set_index(table, fmt->format, fmt->modifiers[k]);
			assert(index >= 0); // table is the union
			indices[n++] = index;
		}
	}
	compiled->indices.size = n * sizeof(uint16_t);
}

struct nx_linux_dmabuf_feedback_v1_compiled *nx_linux_dmabuf_feedback_compile(
		const struct nx_linux_dmabuf_feedback_v1 *feedback) {
	const struct nx_linux_dmabuf_feedback_v1_tranche *tranches = feedback->tranches.data;
	size_t tranches_len = feedback->tranches.size / sizeof(struct nx_linux_dmabuf_feedback_v1_tranche);
	assert(tranches_len > 0);

	// One set indexes every tranche
	struct nx_drm_format_set all_formats = {0};
	for (size_t i = 0; i < tranches_len; i++) {
		if (!nx_drm_format_set_union(&all_formats, &all_formats, &tranches[i].formats)) {
			nx_log(NX_ERROR, "Failed to union scanout formats into one tranche");
			goto err_all_formats;
		}
	}

	size_t table_len = format_pair_count(&all_formats);
	assert(table_len > 0);

	int table_fd = create_format_table(&all_formats, table_len);
	if (table_fd < 0) {
		goto err_all_formats;
	}

	struct nx_linux_dmabuf_feedback_v1_compiled *compiled = calloc(1, sizeof(*compiled) +
		tranches_len * sizeof(struct nx_linux_dmabuf_feedback_v1_compiled_tranche));
	if (compiled == NULL) {
		close(table_fd);
		goto err_all_formats;
	}

	compiled->main_device = feedback->main_device;
	compiled->tranches_len = tranches_len;
	compiled->table_fd = table_fd;
	compiled->table_size =
		table_len * sizeof(struct nx_linux_dmabuf_feedback_v1_table_entry);

	for (size_t i = 0; i < tranches_len; i++) {
		struct nx_linux_dmabuf_feedback_v1_compiled_tranche *compiled_tranche =
			&compiled->tranches[i];
		compiled_tranche->target_device = tranches[i].target_device;
		compiled_tranche->flags = tranches[i].flags;

		wl_array_init(&compiled_tranche->indices);
		if (!wl_array_add(&compiled_tranche->indices, table_len * sizeof(uint16_t))) {
			nx_log(NX_ERROR, "Failed to allocate tranche indices array");
			goto error_compiled;
		}
		compile_tranche_indices(compiled_tranche, &tranches[i].formats, &all_formats);
	}

	nx_drm_format_set_finish(&all_formats);

	return compiled;

error_compiled:
	close(compiled->table_fd);
	free(compiled);
err_all_formats:
	nx_drm_format_set_finish(&all_formats);
	return NULL;
}

void nx_linux_dmabuf_feedback_destroy(
		struct nx_linux_dmabuf_feedback_v1_compiled *feedback) {
	if (feedback == NULL) {
		return;
	}
	for (size_t i = 0; i < feedback->tranches_len; i++) {
		wl_array_release(&feedback->tranches[i].indices);
	}
	close(feedback->table_fd);
	free(feedback);
}

static void feedback_tranche_send(
		const struct nx_linux_dmabuf_feedback_v1_compiled_tranche *tranche,
		struct wl_resource *resource) {
	struct wl_array dev_array = {
		.size = sizeof(tranche->target_device),
		.data = (void *)&tranche->target_device,
	};
	zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(resource, &dev_array);
	zwp_linux_dmabuf_feedback_v1_send_tranche_flags(resource, tranche->flags);
	zwp_linux_dmabuf_feedback_v1_send_tranche_formats(resource,
		(struct wl_array *)&tranche->indices);
	zwp_linux_dmabuf_feedback_v1_send_tranche_done(resource);
}

void nx_linux_dmabuf_feedback_send(
		const struct nx_linux_dmabuf_feedback_v1_compiled *feedback,
		struct wl_resource *resource) {
	struct wl_array dev_array = {
		.size = sizeof(feedback->main_device),
		.data = (void *)&feedback->main_device,
	};
	zwp_linux_dmabuf_feedback_v1_send_main_device(resource, &dev_array);

	zwp_linux_dmabuf_feedback_v1_send_format_table(resource,
		feedback->table_fd, feedback->table_size);

	for (size_t i = 0; i < feedback->tranches_len; i++) {
		feedback_tranche_send(&feedback->tranches[i], resource);
	}

	zwp_linux_dmabuf_feedback_v1_send_done(resource);
}

static void feedback_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
	.destroy = feedback_handle_destroy,
};

struct wl_resource *nx_linux_dmabuf_feedback_resource_create(
		struct wl_resource *parent, uint32_t id, wl_resource_destroy_func_t destroy) {
	struct wl_resource *resource = wl_resource_create(wl_resource_get_client(parent),
		&zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(parent), id);
	if (resource == NULL) {
		return NULL;
	}
	wl_resource_set_implementation(resource, &feedback_impl, NULL, destroy);
	return resource;
}

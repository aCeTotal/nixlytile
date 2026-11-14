#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/pipeline/pipeline_cache.h"
#include "platform/vulkan/renderer.h"

#define CACHE_SUBDIR "nixlytile"
#define CACHE_FILENAME "vk_pipeline_cache.bin"
#define CACHE_MAX_SIZE (64 * 1024 * 1024)
#define CACHE_SAVE_PERIOD_SEC 30

struct cache_job {
	void *data;
	size_t size;
};

static atomic_bool save_inflight;

static bool cache_dir(char *out, size_t out_size) {
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	int n;
	if (xdg && xdg[0] == '/') {
		n = snprintf(out, out_size, "%s/%s", xdg, CACHE_SUBDIR);
	} else if (home && home[0] == '/') {
		n = snprintf(out, out_size, "%s/.cache/%s", home, CACHE_SUBDIR);
	} else {
		return false;
	}
	return n > 0 && (size_t)n < out_size;
}

static bool cache_file_path(char *out, size_t out_size) {
	char dir[PATH_MAX];
	if (!cache_dir(dir, sizeof(dir))) {
		return false;
	}
	int n = snprintf(out, out_size, "%s/%s", dir, CACHE_FILENAME);
	return n > 0 && (size_t)n < out_size;
}

static void *read_stream(FILE *f, size_t *out_size) {
	if (fseek(f, 0, SEEK_END) != 0) {
		return NULL;
	}
	long len = ftell(f);
	if (len <= 0 || len > CACHE_MAX_SIZE) {
		return NULL;
	}
	rewind(f);

	void *buf = malloc((size_t)len);
	if (!buf) {
		return NULL;
	}
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		return NULL;
	}
	*out_size = (size_t)len;
	return buf;
}

static void *read_cache_file(size_t *out_size) {
	*out_size = 0;
	char path[PATH_MAX];
	if (!cache_file_path(path, sizeof(path))) {
		return NULL;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	void *buf = read_stream(f, out_size);
	fclose(f);
	return buf;
}

static bool make_dir(const char *path) {
	if (mkdir(path, 0755) == 0 || errno == EEXIST) {
		return true;
	}
	nx_log(NX_ERROR, "vulkan pipeline cache: mkdir %s failed: %s", path, strerror(errno));
	return false;
}

// Creates the leaf and parent.
static bool make_cache_dir(const char *dir) {
	if (mkdir(dir, 0755) == 0 || errno == EEXIST) {
		return true;
	}

	char parent[PATH_MAX];
	strncpy(parent, dir, sizeof(parent));
	parent[sizeof(parent) - 1] = '\0';
	char *slash = strrchr(parent, '/');
	bool has_parent = slash && slash != parent;
	if (has_parent) {
		*slash = '\0';
	}
	if (has_parent && !make_dir(parent)) {
		return false;
	}
	return make_dir(dir);
}

static bool write_file(const char *path, const void *data, size_t size) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		nx_log(NX_ERROR, "vulkan pipeline cache: open %s failed: %s", path, strerror(errno));
		return false;
	}
	size_t put = fwrite(data, 1, size, f);
	int flush_err = fflush(f);
	int fsync_err = fsync(fileno(f)) != 0 ? errno : 0;
	fclose(f);
	if (put == size && flush_err == 0 && fsync_err == 0) {
		return true;
	}
	nx_log(NX_ERROR, "vulkan pipeline cache: incomplete write to %s", path);
	unlink(path);
	return false;
}

// Atomic replace via temporary file.
static void write_cache_file(const void *data, size_t size) {
	if (!data || size == 0) {
		return;
	}
	char dir[PATH_MAX];
	if (!cache_dir(dir, sizeof(dir)) || !make_cache_dir(dir)) {
		return;
	}

	char path[PATH_MAX], tmp[PATH_MAX];
	if (!cache_file_path(path, sizeof(path))) {
		return;
	}
	int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (n <= 0 || (size_t)n >= sizeof(tmp) || !write_file(tmp, data, size)) {
		return;
	}
	if (rename(tmp, path) != 0) {
		nx_log(NX_ERROR, "vulkan pipeline cache: rename %s -> %s failed: %s",
			tmp, path, strerror(errno));
		unlink(tmp);
		return;
	}
	nx_log(NX_INFO, "vulkan pipeline cache: saved %zu bytes to %s", size, path);
}

static void *writer_thread(void *arg) {
	struct cache_job *job = arg;
	write_cache_file(job->data, job->size);
	free(job->data);
	free(job);
	atomic_store(&save_inflight, false);
	return NULL;
}

// Owns data on success.
static bool start_writer(void *data, size_t size) {
	struct cache_job *job = malloc(sizeof(*job));
	if (!job) {
		return false;
	}
	*job = (struct cache_job){ .data = data, .size = size };

	atomic_store(&save_inflight, true);
	pthread_t thread;
	if (pthread_create(&thread, NULL, writer_thread, job) != 0) {
		atomic_store(&save_inflight, false);
		free(job);
		return false;
	}
	pthread_detach(thread);
	return true;
}

static void *snapshot(struct nx_vk_renderer *renderer, size_t *size) {
	VkDevice dev = renderer->dev->dev;
	*size = 0;
	VkResult res = vkGetPipelineCacheData(dev, renderer->pipeline_cache, size, NULL);
	if (res != VK_SUCCESS || *size == 0) {
		return NULL;
	}

	void *data = malloc(*size);
	if (!data) {
		return NULL;
	}
	res = vkGetPipelineCacheData(dev, renderer->pipeline_cache, size, data);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkGetPipelineCacheData", res);
		free(data);
		return NULL;
	}
	return data;
}

static bool can_save(struct nx_vk_renderer *renderer) {
	return renderer->pipeline_cache != VK_NULL_HANDLE && !atomic_load(&save_inflight);
}

static void mark_saved(struct nx_vk_renderer *renderer) {
	renderer->pipeline_cache_dirty = false;
	clock_gettime(CLOCK_MONOTONIC, &renderer->pipeline_cache_last_save);
}

bool vulkan_pipeline_cache_init(struct nx_vk_renderer *renderer) {
	size_t size = 0;
	void *data = read_cache_file(&size);

	VkPipelineCacheCreateInfo cache_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
		.initialDataSize = size,
		.pInitialData = data,
	};
	VkResult res = vkCreatePipelineCache(renderer->dev->dev, &cache_info, NULL,
		&renderer->pipeline_cache);
	free(data);
	if (res != VK_SUCCESS) {
		nx_vk_error("vkCreatePipelineCache", res);
		return false;
	}

	if (size > 0) {
		nx_log(NX_INFO, "vulkan pipeline cache: loaded %zu bytes", size);
	}
	clock_gettime(CLOCK_MONOTONIC, &renderer->pipeline_cache_last_save);
	return true;
}

// Writes off the render thread.
void vulkan_pipeline_cache_tick(struct nx_vk_renderer *renderer) {
	if (!renderer->pipeline_cache_dirty) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (now.tv_sec - renderer->pipeline_cache_last_save.tv_sec < CACHE_SAVE_PERIOD_SEC ||
			!can_save(renderer)) {
		return;
	}

	size_t size;
	void *data = snapshot(renderer, &size);
	if (data && !start_writer(data, size)) {
		write_cache_file(data, size);
		free(data);
	}
	mark_saved(renderer);
}

// Synchronous; exit may follow.
void vulkan_pipeline_cache_save(struct nx_vk_renderer *renderer) {
	if (!can_save(renderer)) {
		return;
	}
	size_t size;
	void *data = snapshot(renderer, &size);
	write_cache_file(data, size);
	free(data);
	mark_saved(renderer);
}

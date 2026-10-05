#include "nixlytile.h"

#include <fcntl.h>
#include <pwd.h>
#include <sys/mman.h>

#define MGPU_CONF "/.local/nixlyos/multigpu.conf"
#define FRAME_MAGIC 0x4e4c5446u
#define FRAME_PATH "/dev/shm/nixlytile-frame-%u"

/* Layout shared with nixly-multigpu. */
struct compositor_frame {
	uint32_t magic;
	uint32_t seq;
	uint64_t refresh_ns;
	uint64_t last_vblank_ns;
	uint32_t vrr;
	uint32_t pad;
};

static struct compositor_frame *frame_shm;
static int frame_shm_failed;

static void
mgpu_conf_path(char *out, size_t size)
{
	const char *home = getenv("HOME");
	struct passwd *pw;

	if (!home || !*home) {
		pw = getpwuid(getuid());
		home = pw ? pw->pw_dir : "/";
	}
	snprintf(out, size, "%s" MGPU_CONF, home);
}

static int
mgpu_enabled(const char *path)
{
	char line[256];
	int enabled = 0;
	FILE *f = fopen(path, "r");

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "enabled=", 8))
			enabled = atoi(line + 8) != 0;
	fclose(f);
	return enabled;
}

/* Keeps the layer's other keys. */
static int
mgpu_store(const char *path, int enabled)
{
	char tmp[PATH_MAX + 8], line[256];
	FILE *in = fopen(path, "r"), *out;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	out = fopen(tmp, "w");
	if (!out) {
		if (in)
			fclose(in);
		return -1;
	}
	fprintf(out, "enabled=%d\n", enabled);
	while (in && fgets(line, sizeof(line), in))
		if (strncmp(line, "enabled=", 8))
			fputs(line, out);
	if (in)
		fclose(in);
	if (fclose(out))
		return -1;
	return rename(tmp, path);
}

void
togglemultigpu(const Arg *arg)
{
	char path[PATH_MAX];
	int enabled;

	mgpu_conf_path(path, sizeof(path));
	enabled = !mgpu_enabled(path);
	if (mgpu_store(path, enabled) != 0) {
		osd_show_force(selmon, "Multi-GPU: cannot write config");
		return;
	}
	osd_show_force(selmon, enabled ? "Multi-GPU on — next game" : "Multi-GPU off — next game");
}

static int
frame_shm_open(void)
{
	char path[64];
	void *map;
	int fd;

	snprintf(path, sizeof(path), FRAME_PATH, (unsigned)getuid());
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0 || ftruncate(fd, sizeof(*frame_shm)) != 0) {
		if (fd >= 0)
			close(fd);
		frame_shm_failed = 1;
		return 0;
	}
	map = mmap(NULL, sizeof(*frame_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		frame_shm_failed = 1;
		return 0;
	}
	frame_shm = map;
	frame_shm->magic = FRAME_MAGIC;
	return 1;
}

void
multigpu_publish(Monitor *m, uint64_t present_ns)
{
	const struct wlr_output_mode *mode = m->wlr_output->current_mode;

	if (m != selmon || frame_shm_failed || (!frame_shm && !frame_shm_open()))
		return;
	__atomic_add_fetch(&frame_shm->seq, 1, __ATOMIC_RELEASE);
	frame_shm->refresh_ns = mode && mode->refresh > 0 ? 1000000000000ULL / (uint64_t)mode->refresh : 0;
	frame_shm->last_vblank_ns = present_ns;
	frame_shm->vrr = m->vrr_active || m->game_vrr_active;
	__atomic_add_fetch(&frame_shm->seq, 1, __ATOMIC_RELEASE);
}

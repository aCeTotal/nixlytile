/*
 * charge_limit.c — battery charge limit (80/90/100 %) for the battery
 * popup.  Writes charge_control_end_threshold in sysfs (made writable
 * by a nixlyos boot rule, same as the platform profile), persists the
 * choice in ~/.local/nixlyos/charge_limit.conf and re-applies it on
 * startup once the battery device is found.  Default is 80 %.
 */
#include "nixlytile.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>

static int charge_limit = 80;
static int cl_loaded;

/* Persistence lives in status_conf.c (~/.local/nixlyos/status.nix). */
static void
cl_load(void)
{
	int v;

	if (cl_loaded)
		return;
	cl_loaded = 1;
	v = status_conf_chargelimit();
	if (v == 80 || v == 90 || v == 100)
		charge_limit = v;
}

static void
cl_save(void)
{
	status_conf_update("chargelimit", charge_limit);
}

static int
cl_write_sysfs_int(const char *path, int val)
{
	char buf[8];
	int fd, len;
	ssize_t n;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	len = snprintf(buf, sizeof(buf), "%d", val);
	n = write(fd, buf, (size_t)len);
	close(fd);
	return n == len ? 0 : -1;
}

/* Push the limit to the battery. The start threshold (resume charging)
 * must stay below the end threshold or the kernel rejects the write, so
 * it is lowered first when present. */
static int
cl_read_int(const char *path, int *out)
{
	FILE *fp = fopen(path, "r");
	int v;

	if (!fp)
		return -1;
	if (fscanf(fp, "%d", &v) != 1) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*out = v;
	return 0;
}

static int
cl_apply_sysfs(int pct)
{
	char path[PATH_MAX];
	int start;

	if (!battery_device_dir[0])
		return -1;
	snprintf(path, sizeof(path), "%s/charge_control_start_threshold",
			battery_device_dir);
	if (cl_read_int(path, &start) == 0 && start >= pct)
		cl_write_sysfs_int(path, pct > 5 ? pct - 5 : 0);
	snprintf(path, sizeof(path), "%s/charge_control_end_threshold",
			battery_device_dir);
	return cl_write_sysfs_int(path, pct);
}

int
charge_limit_current(void)
{
	cl_load();
	return charge_limit;
}

/* Popup button click: apply now + remember. */
int
charge_limit_set(int pct)
{
	if (pct != 80 && pct != 90 && pct != 100)
		return -1;
	cl_load();
	charge_limit = pct;
	cl_save();
	if (cl_apply_sysfs(pct) != 0) {
		wlr_log(WLR_ERROR,
			"charge limit: writing %d%% to %s failed: %s",
			pct, battery_device_dir, strerror(errno));
		return -1;
	}
	return 0;
}

/* Called once the battery device dir is known (startup): re-apply the
 * remembered limit so it survives reboots without any service. */
void
charge_limit_apply_saved(void)
{
	static int applied;
	char path[PATH_MAX];

	if (applied || !battery_device_dir[0])
		return;
	applied = 1;
	snprintf(path, sizeof(path), "%s/charge_control_end_threshold",
			battery_device_dir);
	if (access(path, W_OK) != 0)
		return;   /* no knob (or not writable): nothing to enforce */
	cl_load();
	cl_apply_sysfs(charge_limit);
}

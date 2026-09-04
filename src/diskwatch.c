/*
 * diskwatch.c — every block-device scan and every format/mount job runs
 * on this thread, never on the compositor thread.
 *
 * Sampling is exec-free: /sys/block for the device tree, /run/udev/data
 * for filesystem type/label/uuid, /proc/self/mounts + statfs() for
 * usage.  Jobs (wipe/mkpart/mkfs/mount) go through the nixly-diskd root
 * helper socket and can block for seconds — exactly why they live here.
 * A snapshot is published under a mutex and the event loop is poked
 * through a pipe; the compositor thread only ever copies the snapshot.
 */
#include "nixlytile.h"
#include <dirent.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

#define DW_POLL_MS      3000
#define DW_FAST_MS      1000
#define DW_FAST_HOLD_MS 15000

extern char **environ;

static pthread_t dw_thread;
static pthread_mutex_t dw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dw_cond = PTHREAD_COND_INITIALIZER;
static DiskSnapshot dw_state;          /* guarded by dw_lock */
static DiskJob dw_job;                 /* guarded by dw_lock */
static int dw_job_pending;
static uint64_t dw_fast_until_ms;
static int dw_pipe[2] = { -1, -1 };
static struct wl_event_source *dw_src;
static volatile int dw_run;

/* ── sampling helpers (worker thread only) ───────────────────────── */

static int
dw_line(const char *path, char *buf, size_t len)
{
	FILE *fp = fopen(path, "r");
	char *nl;

	if (!fp)
		return -1;
	if (!fgets(buf, len, fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	nl = strchr(buf, '\n');
	if (nl)
		*nl = '\0';
	return 0;
}

static unsigned long long
dw_ull(const char *path)
{
	char buf[64];

	if (dw_line(path, buf, sizeof(buf)) != 0)
		return 0;
	return strtoull(buf, NULL, 10);
}

/* Filesystem probe results udev already collected — world-readable, so
 * no blkid and no device-read permission needed. */
static void
dw_udev_fs(const char *sys_dev_file, char *fstype, size_t ftlen,
		char *label, size_t lblen, char *uuid, size_t idlen)
{
	char majmin[32], path[64], line[256];
	FILE *fp;

	fstype[0] = label[0] = uuid[0] = '\0';
	if (dw_line(sys_dev_file, majmin, sizeof(majmin)) != 0)
		return;
	snprintf(path, sizeof(path), "/run/udev/data/b%s", majmin);
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *nl = strchr(line, '\n');

		if (nl)
			*nl = '\0';
		if (strncmp(line, "E:ID_FS_TYPE=", 13) == 0)
			snprintf(fstype, ftlen, "%s", line + 13);
		else if (strncmp(line, "E:ID_FS_LABEL=", 14) == 0)
			snprintf(label, lblen, "%s", line + 14);
		else if (strncmp(line, "E:ID_FS_UUID=", 13) == 0)
			snprintf(uuid, idlen, "%s", line + 13);
	}
	fclose(fp);
}

/* Mount table: /dev/<name> (or a /dev/disk/by-* alias resolving to it)
 * → mountpoint.  Octal escapes in mountpoints (\040) are decoded. */
static int
dw_find_mount(const char *dev, char *mount, size_t len)
{
	FILE *fp = fopen("/proc/self/mounts", "r");
	char mdev[128], mdir[192], rest[64];
	char real[PATH_MAX];
	int found = 0;

	if (!fp)
		return 0;
	while (fscanf(fp, "%127s %191s %63s %*s %*d %*d\n",
				mdev, mdir, rest) == 3) {
		const char *cand = mdev;

		if (strncmp(mdev, "/dev/", 5) != 0)
			continue;
		if (strchr(mdev, '/') && realpath(mdev, real))
			cand = real;
		if (strcmp(cand, dev) != 0)
			continue;
		/* decode \0NN escapes */
		{
			char *w = mount;
			const char *r = mdir;

			while (*r && (size_t)(w - mount) < len - 1) {
				if (r[0] == '\\' && r[1] && r[2] && r[3]) {
					*w++ = (char)(((r[1] - '0') << 6) |
							((r[2] - '0') << 3) |
							(r[3] - '0'));
					r += 4;
				} else {
					*w++ = *r++;
				}
			}
			*w = '\0';
		}
		found = 1;
		break;
	}
	fclose(fp);
	return found;
}

static int
dw_is_system_mount(const char *mount)
{
	static const char *crit[] = { "/", "/home", "/nix", "/boot",
		"/var", "/var/log" };

	for (size_t i = 0; i < sizeof(crit) / sizeof(crit[0]); i++)
		if (strcmp(mount, crit[i]) == 0)
			return 1;
	return 0;
}

static void
dw_fill_part(DiskPart *p, const char *disk_name, const char *part_name)
{
	char path[PATH_MAX];
	struct statvfs vfs;

	memset(p, 0, sizeof(*p));
	snprintf(p->dev, sizeof(p->dev), "/dev/%s", part_name);
	snprintf(path, sizeof(path), "/sys/block/%s/%s/size",
			disk_name, part_name);
	p->size_b = dw_ull(path) * 512ull;
	snprintf(path, sizeof(path), "/sys/block/%s/%s/start",
			disk_name, part_name);
	p->start_b = dw_ull(path) * 512ull;
	snprintf(path, sizeof(path), "/sys/block/%s/%s/dev",
			disk_name, part_name);
	dw_udev_fs(path, p->fstype, sizeof(p->fstype),
			p->label, sizeof(p->label), p->uuid, sizeof(p->uuid));
	if (dw_find_mount(p->dev, p->mount, sizeof(p->mount)) &&
			statvfs(p->mount, &vfs) == 0) {
		unsigned long long total = (unsigned long long)vfs.f_blocks *
			vfs.f_frsize;
		unsigned long long freeb = (unsigned long long)vfs.f_bfree *
			vfs.f_frsize;

		p->avail_b = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
		p->used_b = total > freeb ? total - freeb : 0;
		if (total)
			p->size_b = total;
	}
}

static int
dw_disk_wanted(const char *name)
{
	return strncmp(name, "sd", 2) == 0 ||
		strncmp(name, "nvme", 4) == 0 ||
		strncmp(name, "mmcblk", 6) == 0 ||
		strncmp(name, "vd", 2) == 0;
}

static int
dw_part_cmp(const void *a, const void *b)
{
	const DiskPart *pa = a, *pb = b;

	return pa->start_b < pb->start_b ? -1 : pa->start_b > pb->start_b;
}

static void
dw_sample(DiskSnapshot *s)
{
	DIR *dir = opendir("/sys/block");
	struct dirent *ent;

	s->ndisks = 0;
	if (!dir)
		return;
	while ((ent = readdir(dir)) && s->ndisks < DISK_MAX) {
		DiskDev *d;
		char path[PATH_MAX], real[PATH_MAX];
		DIR *pdir;
		struct dirent *pent;

		if (ent->d_name[0] == '.' || !dw_disk_wanted(ent->d_name))
			continue;
		snprintf(path, sizeof(path), "/sys/block/%s/size", ent->d_name);
		if (dw_ull(path) == 0)
			continue;

		d = &s->disks[s->ndisks];
		memset(d, 0, sizeof(*d));
		snprintf(d->dev, sizeof(d->dev), "/dev/%s", ent->d_name);
		d->size_b = dw_ull(path) * 512ull;
		snprintf(path, sizeof(path), "/sys/block/%s/device/model",
				ent->d_name);
		if (dw_line(path, d->model, sizeof(d->model)) == 0) {
			char *e = d->model + strlen(d->model);

			while (e > d->model && e[-1] == ' ')
				*--e = '\0';
		}
		snprintf(path, sizeof(path), "/sys/block/%s/removable",
				ent->d_name);
		d->is_usb = dw_ull(path) == 1;
		snprintf(path, sizeof(path), "/sys/block/%s", ent->d_name);
		if (!d->is_usb && realpath(path, real) &&
				strstr(real, "/usb"))
			d->is_usb = 1;

		snprintf(path, sizeof(path), "/sys/block/%s", ent->d_name);
		pdir = opendir(path);
		if (pdir) {
			while ((pent = readdir(pdir)) &&
					d->npart < DISK_PART_MAX) {
				char pp[PATH_MAX];

				if (pent->d_name[0] == '.')
					continue;
				if (snprintf(pp, sizeof(pp),
						"/sys/block/%s/%s/partition",
						ent->d_name, pent->d_name)
						>= (int)sizeof(pp))
					continue;
				if (access(pp, R_OK) != 0)
					continue;
				dw_fill_part(&d->parts[d->npart++],
						ent->d_name, pent->d_name);
			}
			closedir(pdir);
		}
		qsort(d->parts, (size_t)d->npart, sizeof(d->parts[0]),
				dw_part_cmp);

		/* Partition-less media (superfloppy USB sticks): probe the
		 * whole device as a single pseudo-partition. */
		if (d->npart == 0) {
			DiskPart *p = &d->parts[0];
			struct statvfs vfs;

			memset(p, 0, sizeof(*p));
			snprintf(p->dev, sizeof(p->dev), "%s", d->dev);
			p->size_b = d->size_b;
			snprintf(path, sizeof(path), "/sys/block/%s/dev",
					ent->d_name);
			dw_udev_fs(path, p->fstype, sizeof(p->fstype),
					p->label, sizeof(p->label),
					p->uuid, sizeof(p->uuid));
			if (p->fstype[0] &&
					dw_find_mount(p->dev, p->mount,
						sizeof(p->mount)) &&
					statvfs(p->mount, &vfs) == 0) {
				unsigned long long total =
					(unsigned long long)vfs.f_blocks *
					vfs.f_frsize;
				unsigned long long freeb =
					(unsigned long long)vfs.f_bfree *
					vfs.f_frsize;

				p->avail_b = (unsigned long long)vfs.f_bavail *
					vfs.f_frsize;
				p->used_b = total > freeb ? total - freeb : 0;
			}
			if (p->fstype[0])
				d->npart = 1;
		}

		for (int i = 0; i < d->npart; i++)
			if (d->parts[i].mount[0] &&
					dw_is_system_mount(d->parts[i].mount))
				d->is_system = 1;
		s->ndisks++;
	}
	closedir(dir);
	s->helper_ok = disk_helper_available();
}

/* ── nixlyos config persistence (worker thread) ──────────────────── */

/* Source of truth: ~/.local/nixlyos/disks.conf, one line per mount:
 *   mount <uuid> <fstype> <mountdir>
 * disks-auto.nix in the deploy repo is regenerated from it and staged
 * with `git add` (flake eval only sees tracked files; update.sh commits). */

static void
dw_conf_path(char *buf, size_t len)
{
	const char *home = getenv("HOME");
	char dir[PATH_MAX - 16];

	snprintf(dir, sizeof(dir), "%s/.local/nixlyos", home ? home : "/tmp");
	mkdir(dir, 0755);
	snprintf(buf, len, "%s/disks.conf", dir);
}

static void
dw_conf_add(const char *uuid, const char *fstype, const char *mountdir)
{
	char path[PATH_MAX], tmp[PATH_MAX], line[256];
	FILE *in, *out;

	dw_conf_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	out = fopen(tmp, "w");
	if (!out)
		return;
	in = fopen(path, "r");
	if (in) {
		while (fgets(line, sizeof(line), in)) {
			char u[64];

			if (sscanf(line, "mount %63s", u) == 1 &&
					strcmp(u, uuid) == 0)
				continue;   /* replaced below */
			fputs(line, out);
		}
		fclose(in);
	}
	fprintf(out, "mount %s %s %s\n", uuid, fstype, mountdir);
	fclose(out);
	rename(tmp, path);
}

static void
dw_run_cmd(char *const argv[])
{
	pid_t pid;
	int status;

	if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) != 0)
		return;
	waitpid(pid, &status, 0);
}

/* Regenerate modules/core/disks-auto.nix in the deploy repo and stage
 * it, so the next update.sh rebuild carries the mounts. */
static void
dw_write_nix(void)
{
	const char *home = getenv("HOME");
	char conf[PATH_MAX], repo[PATH_MAX], nix[PATH_MAX], tmp[PATH_MAX];
	char line[256];
	FILE *in, *out;

	if (!home)
		return;
	dw_conf_path(conf, sizeof(conf));
	snprintf(repo, sizeof(repo), "%s/.nixlyos", home);
	snprintf(nix, sizeof(nix), "%s/modules/core/disks-auto.nix", repo);
	if (access(repo, W_OK) != 0)
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", nix);
	out = fopen(tmp, "w");
	if (!out)
		return;
	fprintf(out, "# Generated by the nixlytile disk module — do not edit.\n"
			"# Source: ~/.local/nixlyos/disks.conf\n"
			"{ ... }:\n{\n");
	in = fopen(conf, "r");
	if (in) {
		while (fgets(line, sizeof(line), in)) {
			char uuid[64], fstype[32], dir[128];

			if (sscanf(line, "mount %63s %31s %127s",
						uuid, fstype, dir) != 3)
				continue;
			fprintf(out,
				"  fileSystems.\"%s\" = {\n"
				"    device = \"/dev/disk/by-uuid/%s\";\n"
				"    fsType = \"%s\";\n"
				"    options = [ \"nofail\" \"x-systemd.device-timeout=5s\" ];\n"
				"  };\n", dir, uuid, fstype);
		}
		fclose(in);
	}
	fprintf(out, "}\n");
	fclose(out);
	rename(tmp, nix);
	{
		char *argv[] = { "git", "-C", repo, "add",
			"modules/core/disks-auto.nix", NULL };

		dw_run_cmd(argv);
	}
}

/* ── job execution (worker thread) ───────────────────────────────── */

static void
dw_set_msg(DiskSnapshot *s, int failed, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(s->op_msg, sizeof(s->op_msg), fmt, ap);
	va_end(ap);
	s->op_failed = failed;
}

/* Sanitize a volume label into a mount directory name. */
static void
dw_mount_name(const DiskJob *j, char *out, size_t len)
{
	const char *src = j->label[0] ? j->label : "disk";
	size_t n = 0;

	for (; *src && n < len - 1; src++) {
		char c = *src;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '-' || c == '_')
			out[n++] = c;
		else if (c == ' ')
			out[n++] = '-';
	}
	out[n] = '\0';
	if (!n)
		snprintf(out, len, "disk");
}

/* After mkpart the new partition device name must be discovered: the
 * highest-numbered partition of the disk once udev settles. */
static int
dw_newest_part(const char *disk, char *out, size_t len)
{
	const char *name = strrchr(disk, '/');
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *ent;
	int best = -1;

	name = name ? name + 1 : disk;
	snprintf(path, sizeof(path), "/sys/block/%s", name);
	dir = opendir(path);
	if (!dir)
		return -1;
	while ((ent = readdir(dir))) {
		char pp[PATH_MAX];
		int num;
		size_t l;

		if (ent->d_name[0] == '.')
			continue;
		if (snprintf(pp, sizeof(pp), "/sys/block/%s/%s/partition",
					name, ent->d_name) >= (int)sizeof(pp))
			continue;
		if (access(pp, R_OK) != 0)
			continue;
		l = strlen(ent->d_name);
		while (l > 0 && ent->d_name[l - 1] >= '0' &&
				ent->d_name[l - 1] <= '9')
			l--;
		num = atoi(ent->d_name + l);
		if (num > best) {
			best = num;
			snprintf(out, len, "/dev/%s", ent->d_name);
		}
	}
	closedir(dir);
	return best > 0 ? 0 : -1;
}

/* mkfs/wipe refuse busy devices — unmount every mounted partition of
 * the target disk (or just the target partition) first. */
static void
dw_umount_targets(const DiskJob *j)
{
	DiskSnapshot probe = {0};
	char cmd[256], reply[128];

	dw_sample(&probe);
	for (int d = 0; d < probe.ndisks; d++) {
		if (strcmp(probe.disks[d].dev, j->disk) != 0)
			continue;
		for (int p = 0; p < probe.disks[d].npart; p++) {
			DiskPart *pt = &probe.disks[d].parts[p];

			if (!pt->mount[0])
				continue;
			if (!j->wipe && j->part[0] &&
					strcmp(pt->dev, j->part) != 0)
				continue;
			snprintf(cmd, sizeof(cmd), "umount %s", pt->mount);
			disk_helper_cmd(cmd, reply, sizeof(reply));
		}
	}
}

static void
dw_execute_job(const DiskJob *j, DiskSnapshot *s)
{
	char cmd[512], reply[256], part[64], mdir[192], name[64];

	if (!disk_helper_available()) {
		dw_set_msg(s, 1, "nixly-diskd helper not running");
		return;
	}

	if (j->wipe || j->rmpart > 0 || j->format)
		dw_umount_targets(j);

	if (j->rmpart > 0) {
		snprintf(cmd, sizeof(cmd), "rmpart %s %d", j->disk, j->rmpart);
		if (disk_helper_cmd(cmd, reply, sizeof(reply)) != 0) {
			dw_set_msg(s, 1, "Delete failed: %.60s", reply);
			return;
		}
		dw_set_msg(s, 0, "Partition deleted");
		return;
	}

	if (j->wipe) {
		snprintf(cmd, sizeof(cmd), "wipe %s", j->disk);
		if (disk_helper_cmd(cmd, reply, sizeof(reply)) != 0) {
			dw_set_msg(s, 1, "Erase failed: %.60s", reply);
			return;
		}
	}

	snprintf(part, sizeof(part), "%s", j->part);
	if (j->end_mib > j->start_mib) {
		snprintf(cmd, sizeof(cmd), "mkpart %s %llu %llu",
				j->disk, j->start_mib, j->end_mib);
		if (disk_helper_cmd(cmd, reply, sizeof(reply)) != 0) {
			dw_set_msg(s, 1, "Partition failed: %.60s", reply);
			return;
		}
		if (dw_newest_part(j->disk, part, sizeof(part)) != 0) {
			dw_set_msg(s, 1, "New partition not found");
			return;
		}
	}
	if (!part[0]) {
		dw_set_msg(s, 1, "No partition to format");
		return;
	}

	if (j->format && j->fstype[0]) {
		snprintf(cmd, sizeof(cmd), "mkfs %s %s %s", j->fstype, part,
				j->label[0] ? j->label : "-");
		if (disk_helper_cmd(cmd, reply, sizeof(reply)) != 0) {
			dw_set_msg(s, 1, "Format failed: %.60s", reply);
			return;
		}
	}

	if (!j->mount_after) {
		dw_set_msg(s, 0, "Done: %s", part);
		return;
	}

	dw_mount_name(j, name, sizeof(name));
	snprintf(mdir, sizeof(mdir), "%s/%s",
			j->is_usb ? "/run/media" : "/mnt", name);
	snprintf(cmd, sizeof(cmd), "mount %s %s %s %d",
			j->fstype[0] ? j->fstype : "auto", part, mdir,
			(int)getuid());
	if (disk_helper_cmd(cmd, reply, sizeof(reply)) != 0) {
		dw_set_msg(s, 1, "Mount failed: %.60s", reply);
		return;
	}

	/* Internal disks persist through the nixlyos config; the mount
	 * above already makes it usable everywhere without a reboot. */
	if (!j->is_usb && j->format && j->fstype[0]) {
		DiskSnapshot probe = {0};

		dw_sample(&probe);
		for (int d = 0; d < probe.ndisks; d++)
			for (int p = 0; p < probe.disks[d].npart; p++)
				if (strcmp(probe.disks[d].parts[p].dev,
							part) == 0 &&
						probe.disks[d].parts[p]
							.uuid[0]) {
					dw_conf_add(probe.disks[d].parts[p]
							.uuid, j->fstype,
							mdir);
					dw_write_nix();
				}
	}
	dw_set_msg(s, 0, "Ready at %s", mdir);
}

/* ── worker loop / event plumbing ────────────────────────────────── */

static void *
dw_worker(void *data)
{
	DiskSnapshot local = {0};

	(void)data;
	pthread_setname_np(pthread_self(), "nixly-diskwatch");

	while (dw_run) {
		DiskJob job;
		int have_job = 0;
		int changed;
		uint64_t now;

		pthread_mutex_lock(&dw_lock);
		if (dw_job_pending) {
			job = dw_job;
			dw_job_pending = 0;
			have_job = 1;
		}
		pthread_mutex_unlock(&dw_lock);

		if (have_job) {
			local.op_running = 1;
			local.op_msg[0] = '\0';
			local.op_failed = 0;
			local.stamp_ms = monotonic_msec();
			pthread_mutex_lock(&dw_lock);
			dw_state = local;
			pthread_mutex_unlock(&dw_lock);
			if (dw_pipe[1] >= 0)
				(void)!write(dw_pipe[1], "d", 1);

			dw_execute_job(&job, &local);
			local.op_running = 0;
		}

		dw_sample(&local);
		local.stamp_ms = monotonic_msec();

		pthread_mutex_lock(&dw_lock);
		/* keep the last job message until the next job starts */
		if (!local.op_msg[0] && dw_state.op_msg[0] && !have_job) {
			snprintf(local.op_msg, sizeof(local.op_msg), "%s",
					dw_state.op_msg);
			local.op_failed = dw_state.op_failed;
		}
		changed = memcmp(&dw_state, &local, sizeof(local)) != 0;
		dw_state = local;
		pthread_mutex_unlock(&dw_lock);

		if (changed && dw_pipe[1] >= 0)
			(void)!write(dw_pipe[1], "d", 1);

		now = monotonic_msec();
		pthread_mutex_lock(&dw_lock);
		if (!dw_job_pending && dw_run) {
			struct timespec ts;
			uint64_t wait = now < dw_fast_until_ms ?
				DW_FAST_MS : DW_POLL_MS;

			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += (time_t)(wait / 1000);
			ts.tv_nsec += (long)(wait % 1000) * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&dw_cond, &dw_lock, &ts);
		}
		pthread_mutex_unlock(&dw_lock);
	}
	return NULL;
}

static int
dw_event(int fd, uint32_t mask, void *data)
{
	char buf[16];

	(void)mask;
	(void)data;
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	refreshstatusdisk();
	return 0;
}

int
diskwatch_get(DiskSnapshot *out)
{
	pthread_mutex_lock(&dw_lock);
	*out = dw_state;
	pthread_mutex_unlock(&dw_lock);
	return out->stamp_ms != 0;
}

/* Popup open: sample faster for a while so plug/unplug shows quickly. */
void
diskwatch_refresh(void)
{
	pthread_mutex_lock(&dw_lock);
	dw_fast_until_ms = monotonic_msec() + DW_FAST_HOLD_MS;
	pthread_cond_signal(&dw_cond);
	pthread_mutex_unlock(&dw_lock);
}

void
diskwatch_run_job(const DiskJob *job)
{
	pthread_mutex_lock(&dw_lock);
	dw_job = *job;
	dw_job_pending = 1;
	pthread_cond_signal(&dw_cond);
	pthread_mutex_unlock(&dw_lock);
}

void
diskwatch_init(void)
{
	if (pipe2(dw_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
		return;
	dw_src = wl_event_loop_add_fd(event_loop, dw_pipe[0],
			WL_EVENT_READABLE, dw_event, NULL);
	dw_run = 1;
	if (pthread_create(&dw_thread, NULL, dw_worker, NULL) != 0) {
		dw_run = 0;
		wlr_log(WLR_ERROR, "diskwatch: thread start failed — "
				"disk module unavailable");
	}
}

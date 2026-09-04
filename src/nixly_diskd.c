/*
 * nixly-diskd — tiny root helper for the nixlytile disk module:
 *
 *   wipe <disk>                      wipefs + fresh GPT label
 *   mkpart <disk> <start> <end>      parted mkpart (MiB offsets)
 *   rmpart <disk> <n>                parted rm
 *   mkfs <fstype> <part> <label>     mkfs.<fstype>; label "-" = none
 *   mount <fstype> <part> <dir> <uid>  mkdir -p + mount, user-writable
 *   umount <dir-or-dev>
 *   ping                             liveness probe
 *
 * One text line per connection round on /run/nixly-diskd.sock (0666 —
 * every mutating command is refused on the disks holding the system
 * mounts, and device/dir arguments are strictly validated).  Replies
 * "ok" or "err <msg>".  Built standalone (no compositor deps), run as a
 * systemd service from the nixlyos flake with parted + mkfs tools on
 * PATH.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SOCK_PATH "/run/nixly-diskd.sock"
#define MAX_SYS_DISKS 8

/* Disks that back /, /home, /nix, ... — resolved once at startup; every
 * mutating command against them is refused. */
static char sys_disks[MAX_SYS_DISKS][64];
static int n_sys_disks;

/* /dev/sda1 → /dev/sda, /dev/nvme0n1p2 → /dev/nvme0n1.  A name that is
 * already a whole disk (present in /sys/block) is returned unchanged —
 * digit-stripping alone would turn nvme0n1 into nvme0n. */
static void
parent_disk(const char *dev, char *out, size_t len)
{
	const char *name = strrchr(dev, '/');
	char sys[96];
	size_t l;

	snprintf(out, len, "%s", dev);
	snprintf(sys, sizeof(sys), "/sys/block/%s",
			name ? name + 1 : dev);
	if (access(sys, F_OK) == 0)
		return;
	l = strlen(out);
	while (l > 0 && isdigit((unsigned char)out[l - 1]))
		l--;
	if (l > 1 && out[l - 1] == 'p' &&
			isdigit((unsigned char)out[l - 2]))
		l--;   /* nvme0n1p2 / mmcblk0p1 */
	if (l < strlen(out))
		out[l] = '\0';
}

static void
find_system_disks(void)
{
	static const char *crit[] = { "/", "/home", "/nix", "/boot", "/var" };
	FILE *fp = fopen("/proc/self/mounts", "r");
	char mdev[128], mdir[192];

	if (!fp)
		return;
	while (fscanf(fp, "%127s %191s %*s %*s %*d %*d\n", mdev, mdir) == 2) {
		char real[PATH_MAX], disk[64];
		int hit = 0;

		for (size_t i = 0; i < sizeof(crit) / sizeof(crit[0]); i++)
			if (strcmp(mdir, crit[i]) == 0)
				hit = 1;
		if (!hit || strncmp(mdev, "/dev/", 5) != 0)
			continue;
		if (!realpath(mdev, real))
			snprintf(real, sizeof(real), "%s", mdev);
		parent_disk(real, disk, sizeof(disk));
		for (int i = 0; i < n_sys_disks; i++)
			if (strcmp(sys_disks[i], disk) == 0) {
				hit = 0;
				break;
			}
		if (hit && n_sys_disks < MAX_SYS_DISKS)
			snprintf(sys_disks[n_sys_disks++],
					sizeof(sys_disks[0]), "%s", disk);
	}
	fclose(fp);
}

/* Valid /dev node name: /dev/ + [a-z0-9] only (sd*, nvme*, mmcblk*, vd*). */
static int
dev_ok(const char *dev)
{
	const char *p;

	if (strncmp(dev, "/dev/", 5) != 0 || strlen(dev) > 32)
		return 0;
	for (p = dev + 5; *p; p++)
		if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p))
			return 0;
	return *dev != '\0';
}

static int
dev_is_system(const char *dev)
{
	char disk[64];

	parent_disk(dev, disk, sizeof(disk));
	for (int i = 0; i < n_sys_disks; i++)
		if (strcmp(sys_disks[i], disk) == 0)
			return 1;
	return 0;
}

static int
dir_ok(const char *dir)
{
	if (strstr(dir, ".."))
		return 0;
	return strncmp(dir, "/mnt/", 5) == 0 ||
		strncmp(dir, "/run/media/", 11) == 0;
}

static int
label_ok(const char *s)
{
	if (strlen(s) > 32)
		return 0;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_')
			return 0;
	return 1;
}

/* Run argv, capture the tail of stderr for error replies. */
static int
run(char *const argv[], char *errbuf, size_t errlen)
{
	int pfd[2];
	pid_t pid;
	int status;
	ssize_t n;
	char tmp[512];

	if (errlen)
		errbuf[0] = '\0';
	if (pipe2(pfd, O_CLOEXEC) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		if (null >= 0)
			dup2(null, 0);
		dup2(pfd[1], 1);
		dup2(pfd[1], 2);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pfd[1]);
	n = 0;
	{
		ssize_t r;

		while ((r = read(pfd[0], tmp + n,
					sizeof(tmp) - 1 - (size_t)n)) > 0)
			if ((n += r) >= (ssize_t)sizeof(tmp) - 1)
				break;
	}
	close(pfd[0]);
	tmp[n] = '\0';
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	if (errlen) {
		char *last = tmp, *p;

		/* last non-empty line is usually the useful one */
		for (p = tmp; *p; p++)
			if (*p == '\n' && p[1])
				last = p + 1;
		for (p = last; *p; p++)
			if (*p == '\n')
				*p = '\0';
		snprintf(errbuf, errlen, "%s", last);
	}
	return -1;
}

static void
settle(void)
{
	char *argv[] = { "udevadm", "settle", "--timeout=10", NULL };
	char eb[8];

	run(argv, eb, 0);
}

static void
cmd_wipe(FILE *out, const char *args)
{
	char dev[64], eb[128];

	if (sscanf(args, "%63s", dev) != 1 || !dev_ok(dev)) {
		fprintf(out, "err parse\n");
		return;
	}
	if (dev_is_system(dev)) {
		fprintf(out, "err system-disk\n");
		return;
	}
	{
		char *a1[] = { "wipefs", "-a", dev, NULL };
		char *a2[] = { "parted", "-s", dev, "mklabel", "gpt", NULL };

		if (run(a1, eb, sizeof(eb)) != 0 ||
				run(a2, eb, sizeof(eb)) != 0) {
			fprintf(out, "err %s\n", eb[0] ? eb : "wipe");
			return;
		}
	}
	settle();
	fprintf(out, "ok\n");
}

static void
cmd_mkpart(FILE *out, const char *args)
{
	char dev[64], start[32], end[32], eb[128];
	unsigned long long s, e;

	if (sscanf(args, "%63s %llu %llu", dev, &s, &e) != 3 ||
			!dev_ok(dev) || e <= s) {
		fprintf(out, "err parse\n");
		return;
	}
	if (dev_is_system(dev)) {
		fprintf(out, "err system-disk\n");
		return;
	}
	snprintf(start, sizeof(start), "%lluMiB", s);
	snprintf(end, sizeof(end), "%lluMiB", e);
	{
		char *argv[] = { "parted", "-s", "-a", "optimal", dev,
			"mkpart", "primary", start, end, NULL };

		if (run(argv, eb, sizeof(eb)) != 0) {
			fprintf(out, "err %s\n", eb[0] ? eb : "mkpart");
			return;
		}
	}
	settle();
	fprintf(out, "ok\n");
}

static void
cmd_rmpart(FILE *out, const char *args)
{
	char dev[64], num[16], eb[128];
	int n;

	if (sscanf(args, "%63s %d", dev, &n) != 2 || !dev_ok(dev) ||
			n < 1 || n > 128) {
		fprintf(out, "err parse\n");
		return;
	}
	if (dev_is_system(dev)) {
		fprintf(out, "err system-disk\n");
		return;
	}
	snprintf(num, sizeof(num), "%d", n);
	{
		char *argv[] = { "parted", "-s", dev, "rm", num, NULL };

		if (run(argv, eb, sizeof(eb)) != 0) {
			fprintf(out, "err %s\n", eb[0] ? eb : "rm");
			return;
		}
	}
	settle();
	fprintf(out, "ok\n");
}

static void
cmd_mkfs(FILE *out, const char *args)
{
	char fstype[16], dev[64], label[40], eb[128];
	char *argv[8];
	int n = 0, r;

	if (sscanf(args, "%15s %63s %39s", fstype, dev, label) != 3 ||
			!dev_ok(dev)) {
		fprintf(out, "err parse\n");
		return;
	}
	if (dev_is_system(dev)) {
		fprintf(out, "err system-disk\n");
		return;
	}
	if (strcmp(label, "-") == 0)
		label[0] = '\0';
	if (label[0] && !label_ok(label)) {
		fprintf(out, "err label\n");
		return;
	}

	if (strcmp(fstype, "ext4") == 0) {
		argv[n++] = "mkfs.ext4";
		argv[n++] = "-F";
		if (label[0]) { argv[n++] = "-L"; argv[n++] = label; }
	} else if (strcmp(fstype, "btrfs") == 0) {
		argv[n++] = "mkfs.btrfs";
		argv[n++] = "-f";
		if (label[0]) { argv[n++] = "-L"; argv[n++] = label; }
	} else if (strcmp(fstype, "xfs") == 0) {
		argv[n++] = "mkfs.xfs";
		argv[n++] = "-f";
		if (label[0]) { argv[n++] = "-L"; argv[n++] = label; }
	} else if (strcmp(fstype, "vfat") == 0) {
		argv[n++] = "mkfs.vfat";
		if (label[0]) { argv[n++] = "-n"; argv[n++] = label; }
	} else if (strcmp(fstype, "exfat") == 0) {
		argv[n++] = "mkfs.exfat";
		if (label[0]) { argv[n++] = "-n"; argv[n++] = label; }
	} else if (strcmp(fstype, "ntfs") == 0) {
		argv[n++] = "mkfs.ntfs";
		argv[n++] = "-Q";
		if (label[0]) { argv[n++] = "-L"; argv[n++] = label; }
	} else {
		fprintf(out, "err fstype\n");
		return;
	}
	argv[n++] = dev;
	argv[n] = NULL;
	r = run(argv, eb, sizeof(eb));
	settle();
	if (r == 0)
		fprintf(out, "ok\n");
	else
		fprintf(out, "err %s\n", eb[0] ? eb : "mkfs");
}

/* mkdir -p for a validated /mnt/... or /run/media/... path. */
static int
mkdirs(const char *dir)
{
	char tmp[192];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", dir);
	for (p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static void
cmd_mount(FILE *out, const char *args)
{
	char fstype[16], dev[64], dir[192], opts[96], eb[128];
	int uid, perm_fs;

	if (sscanf(args, "%15s %63s %191s %d", fstype, dev, dir, &uid) != 4 ||
			!dev_ok(dev) || !dir_ok(dir) || uid < 0) {
		fprintf(out, "err parse\n");
		return;
	}
	if (mkdirs(dir) != 0) {
		fprintf(out, "err mkdir\n");
		return;
	}
	/* vfat/exfat/ntfs have no unix permissions — map ownership at
	 * mount time; unix filesystems get a chown on the mount root. */
	perm_fs = strcmp(fstype, "vfat") == 0 ||
		strcmp(fstype, "exfat") == 0 || strcmp(fstype, "ntfs") == 0;
	if (perm_fs)
		snprintf(opts, sizeof(opts), "uid=%d,gid=%d", uid, uid);
	{
		char *argv[10];
		int n = 0, r;

		argv[n++] = "mount";
		if (strcmp(fstype, "auto") != 0) {
			argv[n++] = "-t";
			argv[n++] = strcmp(fstype, "ntfs") == 0 ?
				"ntfs3" : fstype;
		}
		if (perm_fs) {
			argv[n++] = "-o";
			argv[n++] = opts;
		}
		argv[n++] = dev;
		argv[n++] = dir;
		argv[n] = NULL;
		r = run(argv, eb, sizeof(eb));
		if (r != 0 && strcmp(fstype, "ntfs") == 0) {
			/* kernel ntfs3 missing → FUSE driver */
			argv[2] = "ntfs-3g";
			r = run(argv, eb, sizeof(eb));
		}
		if (r != 0) {
			fprintf(out, "err %s\n", eb[0] ? eb : "mount");
			return;
		}
	}
	if (!perm_fs)
		(void)!chown(dir, (uid_t)uid, (gid_t)uid);
	fprintf(out, "ok\n");
}

static void
cmd_umount(FILE *out, const char *args)
{
	char target[192], eb[128];

	if (sscanf(args, "%191s", target) != 1 ||
			(!dir_ok(target) && !dev_ok(target))) {
		fprintf(out, "err parse\n");
		return;
	}
	if (dev_ok(target) && dev_is_system(target)) {
		fprintf(out, "err system-disk\n");
		return;
	}
	{
		char *argv[] = { "umount", target, NULL };

		if (run(argv, eb, sizeof(eb)) == 0)
			fprintf(out, "ok\n");
		else
			fprintf(out, "err %s\n", eb[0] ? eb : "umount");
	}
}

int
main(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	const char *sock = getenv("NIXLY_DISKD_SOCK"); /* test override */
	int lfd;

	if (!sock || !*sock)
		sock = SOCK_PATH;
	signal(SIGPIPE, SIG_IGN);
	find_system_disks();
	lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (lfd < 0) {
		perror("socket");
		return 1;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
	unlink(sock);
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
			listen(lfd, 8) < 0) {
		perror("bind");
		return 1;
	}
	chmod(sock, 0666);

	for (;;) {
		char line[256];
		int cfd = accept(lfd, NULL, NULL);
		FILE *io;
		ssize_t n;

		if (cfd < 0)
			continue;
		n = read(cfd, line, sizeof(line) - 1);
		if (n <= 0) {
			close(cfd);
			continue;
		}
		line[n] = '\0';
		io = fdopen(cfd, "w");
		if (!io) {
			close(cfd);
			continue;
		}
		if (strncmp(line, "ping", 4) == 0)
			fprintf(io, "ok\n");
		else if (strncmp(line, "wipe ", 5) == 0)
			cmd_wipe(io, line + 5);
		else if (strncmp(line, "mkpart ", 7) == 0)
			cmd_mkpart(io, line + 7);
		else if (strncmp(line, "rmpart ", 7) == 0)
			cmd_rmpart(io, line + 7);
		else if (strncmp(line, "mkfs ", 5) == 0)
			cmd_mkfs(io, line + 5);
		else if (strncmp(line, "mount ", 6) == 0)
			cmd_mount(io, line + 6);
		else if (strncmp(line, "umount ", 7) == 0)
			cmd_umount(io, line + 7);
		else
			fprintf(io, "err cmd\n");
		fclose(io);
	}
}

/*
 * nixly-fand — tiny root helper for fan control paths that need
 * privileges the compositor doesn't have:
 *
 *   ecr <hexoff>            read one MSI EC byte (debugfs ec_sys)
 *   ecw <hexoff> <val>      write one EC byte — fan-table offsets only
 *   pwm <hwmonN> <idx> <v>  hwmon pwm write; v 0-255 manual, -1 auto
 *   nv <gpu> <fan> <pct>    NVML fan set (root-only in the driver);
 *                           pct -1 restores the driver's auto policy
 *   ping                    liveness probe
 *
 * One text line per connection round on /run/nixly-fand.sock (0666 —
 * the command surface is deliberately too narrow to matter: fan-table
 * EC bytes, pwm files, fan percent).  Replies "ok[ <val>]" or
 * "err <msg>".  Built standalone (no compositor deps), run as a
 * systemd service from the nixlyos flake.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_PATH "/run/nixly-fand.sock"
#define EC_IO     "/sys/kernel/debug/ec/ec0/io"

/* MSI EC fan tables: CPU temps 0x6A-0x6F, speeds 0x72-0x78; GPU temps
 * 0x82-0x87, speeds 0x8A-0x90.  Writes outside these are refused. */
static int
ec_writable(int off)
{
	return (off >= 0x6A && off <= 0x6F) || (off >= 0x72 && off <= 0x78) ||
		(off >= 0x82 && off <= 0x87) || (off >= 0x8A && off <= 0x90);
}

static int
ec_open(void)
{
	static int fd = -2;

	if (fd == -2) {
		const char *p = getenv("NIXLY_FAND_EC_IO"); /* test override */

		fd = open(p && *p ? p : EC_IO, O_RDWR | O_CLOEXEC);
	}
	return fd;
}

static void
cmd_ec(FILE *out, const char *args, int write_op)
{
	unsigned off;
	int val = 0, fd;
	unsigned char b;

	if (write_op ? sscanf(args, "%x %d", &off, &val) != 2 :
			sscanf(args, "%x", &off) != 1) {
		fprintf(out, "err parse\n");
		return;
	}
	if (off > 0xFF || val < 0 || val > 0xFF ||
			(write_op && !ec_writable((int)off))) {
		fprintf(out, "err range\n");
		return;
	}
	fd = ec_open();
	if (fd < 0) {
		fprintf(out, "err no-ec\n");
		return;
	}
	if (write_op) {
		b = (unsigned char)val;
		fprintf(out, pwrite(fd, &b, 1, off) == 1 ? "ok\n" : "err io\n");
	} else {
		if (pread(fd, &b, 1, off) == 1)
			fprintf(out, "ok %d\n", b);
		else
			fprintf(out, "err io\n");
	}
}

static int
write_sysfs(const char *path, int val)
{
	char buf[16];
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = snprintf(buf, sizeof(buf), "%d", val);
	n = write(fd, buf, (size_t)n) == n ? 0 : -1;
	close(fd);
	return (int)n;
}

static void
cmd_pwm(FILE *out, const char *args)
{
	char name[32], path[128];
	int idx, val;

	if (sscanf(args, "%31s %d %d", name, &idx, &val) != 3 ||
			idx < 1 || idx > 10 || val < -1 || val > 255) {
		fprintf(out, "err parse\n");
		return;
	}
	if (strncmp(name, "hwmon", 5) != 0) {
		fprintf(out, "err name\n");
		return;
	}
	for (const char *p = name + 5; *p; p++)
		if (!isdigit((unsigned char)*p)) {
			fprintf(out, "err name\n");
			return;
		}
	snprintf(path, sizeof(path), "/sys/class/hwmon/%s/pwm%d_enable",
			name, idx);
	if (val < 0) {
		fprintf(out, write_sysfs(path, 2) == 0 ? "ok\n" : "err io\n");
		return;
	}
	write_sysfs(path, 1); /* some drivers have no _enable; best effort */
	snprintf(path, sizeof(path), "/sys/class/hwmon/%s/pwm%d", name, idx);
	fprintf(out, write_sysfs(path, val) == 0 ? "ok\n" : "err io\n");
}

/* NVML, loaded on first nv command. */
static void *nv_dev[8];
static unsigned nv_count;
static int (*p_set_fan)(void *, unsigned, unsigned);
static int (*p_set_default)(void *, unsigned);

static int
nv_setup(void)
{
	static int ready = -1;
	void *lib;
	int (*p_init)(void);
	int (*p_count)(unsigned *);
	int (*p_by_index)(unsigned, void **);

	if (ready >= 0)
		return ready;
	ready = 0;
	lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
	if (!lib)
		lib = dlopen("/run/opengl-driver/lib/libnvidia-ml.so.1",
				RTLD_NOW);
	if (!lib)
		return 0;
	p_init = dlsym(lib, "nvmlInit_v2");
	p_count = dlsym(lib, "nvmlDeviceGetCount_v2");
	p_by_index = dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
	p_set_fan = dlsym(lib, "nvmlDeviceSetFanSpeed_v2");
	p_set_default = dlsym(lib, "nvmlDeviceSetDefaultFanSpeed_v2");
	if (!p_init || !p_count || !p_by_index || !p_set_fan)
		return 0;
	if (p_init() != 0 || p_count(&nv_count) != 0 || nv_count == 0)
		return 0;
	if (nv_count > 8)
		nv_count = 8;
	for (unsigned i = 0; i < nv_count; i++)
		if (p_by_index(i, &nv_dev[i]) != 0)
			return 0;
	ready = 1;
	return 1;
}

static void
cmd_nv(FILE *out, const char *args)
{
	int gpu, fan, pct, r;

	if (sscanf(args, "%d %d %d", &gpu, &fan, &pct) != 3 ||
			gpu < 0 || fan < 0 || fan > 15 ||
			pct < -1 || pct > 100) {
		fprintf(out, "err parse\n");
		return;
	}
	if (!nv_setup() || (unsigned)gpu >= nv_count) {
		fprintf(out, "err no-nvml\n");
		return;
	}
	if (pct < 0)
		r = p_set_default ? p_set_default(nv_dev[gpu], (unsigned)fan)
				: -1;
	else
		r = p_set_fan(nv_dev[gpu], (unsigned)fan, (unsigned)pct);
	fprintf(out, r == 0 ? "ok\n" : "err nvml\n");
}

int
main(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	const char *sock = getenv("NIXLY_FAND_SOCK"); /* test override */
	int lfd;

	if (!sock || !*sock)
		sock = SOCK_PATH;
	signal(SIGPIPE, SIG_IGN);
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
		char line[128];
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
		else if (strncmp(line, "ecr ", 4) == 0)
			cmd_ec(io, line + 4, 0);
		else if (strncmp(line, "ecw ", 4) == 0)
			cmd_ec(io, line + 4, 1);
		else if (strncmp(line, "pwm ", 4) == 0)
			cmd_pwm(io, line + 4);
		else if (strncmp(line, "nv ", 3) == 0)
			cmd_nv(io, line + 3);
		else
			fprintf(io, "err cmd\n");
		fclose(io);
	}
}

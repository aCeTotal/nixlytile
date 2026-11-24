/* Client for the nixly-fand helper. */
#include "nixlytile.h"
#include <sys/socket.h>
#include <sys/un.h>

#define FAND_SOCK "/run/nixly-fand.sock"
#define FAND_TIMEOUT_MS 300
#define FAND_RECHECK_MS 10000

static int fh_ok = -1;           /* -1 unknown, 0 down, 1 up */
static uint64_t fh_checked_ms;

static int
fh_cmd(const char *cmd, char *reply, size_t len)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct timeval tv = { .tv_sec = 0, .tv_usec = FAND_TIMEOUT_MS * 1000 };
	int fd;
	ssize_t n;

	const char *sock = getenv("NIXLY_FAND_SOCK"); /* test override */

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
			sock && *sock ? sock : FAND_SOCK);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	if (write(fd, cmd, strlen(cmd)) < 0 ||
			write(fd, "\n", 1) < 0) {
		close(fd);
		return -1;
	}
	n = read(fd, reply, len - 1);
	close(fd);
	if (n <= 0)
		return -1;
	reply[n] = '\0';
	return strncmp(reply, "ok", 2) == 0 ? 0 : -1;
}

int
fan_helper_available(void)
{
	uint64_t now = monotonic_msec();
	char reply[64];

	if (fh_ok >= 0 && now - fh_checked_ms < FAND_RECHECK_MS)
		return fh_ok;
	fh_checked_ms = now;
	fh_ok = fh_cmd("ping", reply, sizeof(reply)) == 0;
	return fh_ok;
}

/* val 0-255 sets manual pwm, -1 hands back to the firmware curve. */
int
fan_helper_pwm(const char *hwmon, int idx, int val)
{
	char cmd[96], reply[64];
	const char *base = strrchr(hwmon, '/');

	base = base ? base + 1 : hwmon;
	snprintf(cmd, sizeof(cmd), "pwm %s %d %d", base, idx, val);
	return fh_cmd(cmd, reply, sizeof(reply));
}

int
fan_helper_nv(int gpu, int fan, int pct)
{
	char cmd[48], reply[64];

	snprintf(cmd, sizeof(cmd), "nv %d %d %d", gpu, fan, pct);
	return fh_cmd(cmd, reply, sizeof(reply));
}

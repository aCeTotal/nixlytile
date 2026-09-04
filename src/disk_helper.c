/*
 * disk_helper.c — client for the nixly-diskd root helper.
 *
 * The helper (systemd service, see flake.nix) owns partitioning, mkfs
 * and mount for the disk popup.  Protocol: one text line per command on
 * /run/nixly-diskd.sock, reply "ok[ ...]" or "err <msg>".
 *
 * Worker-thread only (diskwatch) — mkfs on a large disk takes seconds,
 * so the receive timeout is generous; the liveness probe stays short.
 */
#include "nixlytile.h"
#include <sys/socket.h>
#include <sys/un.h>

#define DISKD_SOCK       "/run/nixly-diskd.sock"
#define DISKD_PING_MS    300
#define DISKD_CMD_MS     120000
#define DISKD_RECHECK_MS 10000

static int dh_ok = -1;           /* -1 unknown, 0 down, 1 up */
static uint64_t dh_checked_ms;

static int
dh_cmd(const char *cmd, char *reply, size_t len, int timeout_ms)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct timeval tv = { .tv_sec = timeout_ms / 1000,
		.tv_usec = (timeout_ms % 1000) * 1000 };
	const char *sock = getenv("NIXLY_DISKD_SOCK"); /* test override */
	int fd;
	ssize_t n;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
			sock && *sock ? sock : DISKD_SOCK);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		snprintf(reply, len, "err no-helper");
		return -1;
	}
	if (write(fd, cmd, strlen(cmd)) < 0 || write(fd, "\n", 1) < 0) {
		close(fd);
		snprintf(reply, len, "err io");
		return -1;
	}
	n = read(fd, reply, len - 1);
	close(fd);
	if (n <= 0) {
		snprintf(reply, len, "err timeout");
		return -1;
	}
	reply[n] = '\0';
	return strncmp(reply, "ok", 2) == 0 ? 0 : -1;
}

int
disk_helper_available(void)
{
	uint64_t now = monotonic_msec();
	char reply[64];

	if (dh_ok >= 0 && now - dh_checked_ms < DISKD_RECHECK_MS)
		return dh_ok;
	dh_checked_ms = now;
	dh_ok = dh_cmd("ping", reply, sizeof(reply), DISKD_PING_MS) == 0;
	return dh_ok;
}

int
disk_helper_cmd(const char *cmd, char *reply, size_t len)
{
	return dh_cmd(cmd, reply, len, DISKD_CMD_MS);
}

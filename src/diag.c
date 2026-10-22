/*
 * diag.c — always-on diagnostic logger. See diag.h.
 */
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "diag.h"

#define DIAG_PATH "/tmp/nixlytile-diag.log"
/* /tmp is RAM: two chunks max. */
#define DIAG_ROTATE_BYTES (8L << 20)

static FILE *diag_fp;
static char diag_path[PATH_MAX];
static time_t diag_flush_sec;

static void
diag_flush(void)
{
	if (diag_fp)
		fflush(diag_fp);
}

static void
diag_rotate(void)
{
	char old[PATH_MAX + 2];

	snprintf(old, sizeof(old), "%s.1", diag_path);
	rename(diag_path, old);
	diag_fp = freopen(diag_path, "we", diag_fp);
	if (diag_fp)
		setvbuf(diag_fp, NULL, _IOFBF, 64 * 1024);
}

void
diag_init(void)
{
	/* Truncate: one self-contained log per compositor session.
	 * NIXLYTILE_DIAG_PATH override keeps a nested debug instance from
	 * clobbering the live session's log. */
	const char *path = getenv("NIXLYTILE_DIAG_PATH");
	if (!path)
		path = DIAG_PATH;
	snprintf(diag_path, sizeof(diag_path), "%s", path);
	diag_fp = fopen(path, "we");
	if (!diag_fp)
		return;
	/* Fully buffered: diag_logf runs inside rendermon (heartbeat,
	 * ANIMHITCH — synchronously, exactly when a frame is already late),
	 * and line buffering made every call a blocking write(2).  Flushed
	 * at most once per second from diag_logf and at exit. */
	setvbuf(diag_fp, NULL, _IOFBF, 64 * 1024);
	atexit(diag_flush);
	diag_logf("START", "nixlytile diagnostic log (%s)", DIAG_PATH);
}

void
diag_logf(const char *cat, const char *fmt, ...)
{
	va_list ap;
	struct timespec ts;
	struct tm tm;
	char stamp[16];

	if (!diag_fp)
		return;

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);

	fprintf(diag_fp, "%s.%03ld %-10s ", stamp, ts.tv_nsec / 1000000, cat);
	va_start(ap, fmt);
	vfprintf(diag_fp, fmt, ap);
	va_end(ap);
	fputc('\n', diag_fp);

	/* At most one write(2) per second — reuses the timestamp already
	 * fetched for the log line, so no extra clock read. */
	if (ts.tv_sec != diag_flush_sec) {
		diag_flush_sec = ts.tv_sec;
		fflush(diag_fp);
		if (ftell(diag_fp) > DIAG_ROTATE_BYTES)
			diag_rotate();
	}
}

/*
 * diag.c — always-on diagnostic logger. See diag.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "diag.h"

#define DIAG_PATH "/tmp/nixlytile-diag.log"

static FILE *diag_fp;
static time_t diag_flush_sec;

static void
diag_flush(void)
{
	if (diag_fp)
		fflush(diag_fp);
}

void
diag_init(void)
{
	/* Truncate: one self-contained log per compositor session. */
	diag_fp = fopen(DIAG_PATH, "w");
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
	}
}

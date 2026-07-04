/*
 * diag.c — always-on diagnostic logger. See diag.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "diag.h"

#define DIAG_PATH "/tmp/nixlytile-diag.log"

static FILE *diag_fp;

void
diag_init(void)
{
	/* Truncate: one self-contained log per compositor session. */
	diag_fp = fopen(DIAG_PATH, "w");
	if (!diag_fp)
		return;
	/* Line-buffered so a hang or crash still leaves every line on disk. */
	setvbuf(diag_fp, NULL, _IOLBF, 0);
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
}

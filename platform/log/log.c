#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "platform/log/log.h"
#include "platform/os/clock.h"

static enum nx_log_importance log_importance = NX_ERROR;
static struct timespec start_time = { .tv_sec = -1 };

static const char *const level_colors[] = {
	[NX_ERROR] = "\x1B[1;31m",
	[NX_INFO] = "\x1B[1;34m",
	[NX_DEBUG] = "\x1B[1;90m",
};

static const char *const level_headers[] = {
	[NX_ERROR] = "[ERROR]",
	[NX_INFO] = "[INFO]",
	[NX_DEBUG] = "[DEBUG]",
};

static void log_stderr(enum nx_log_importance verbosity, const char *fmt,
		va_list args) {
	if (start_time.tv_sec < 0) {
		clock_gettime(CLOCK_MONOTONIC, &start_time);
	}
	if (verbosity > log_importance) {
		return;
	}

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	timespec_sub(&ts, &ts, &start_time);
	fprintf(stderr, "%02d:%02d:%02d.%03ld ", (int)(ts.tv_sec / 60 / 60),
		(int)(ts.tv_sec / 60 % 60), (int)(ts.tv_sec % 60),
		ts.tv_nsec / 1000000);

	bool tty = isatty(STDERR_FILENO);
	if (tty) {
		fputs(level_colors[verbosity], stderr);
	} else {
		fprintf(stderr, "%s ", level_headers[verbosity]);
	}
	vfprintf(stderr, fmt, args);
	fputs(tty ? "\x1B[0m\n" : "\n", stderr);
}

static nx_log_func_t log_callback = log_stderr;

static void log_wl(const char *fmt, va_list args) {
	static char nx_fmt[1024];
	snprintf(nx_fmt, sizeof(nx_fmt), "[wayland] %s", fmt);
	size_t len = strlen(nx_fmt);
	if (nx_fmt[len - 1] == '\n') {
		nx_fmt[len - 1] = '\0';
	}
	_nx_vlog(NX_INFO, nx_fmt, args);
}

void nx_log_init(enum nx_log_importance verbosity, nx_log_func_t callback) {
	log_importance = verbosity;
	log_callback = callback;
	wl_log_set_handler_server(log_wl);
}

void _nx_vlog(enum nx_log_importance verbosity, const char *fmt, va_list args) {
	log_callback(verbosity, fmt, args);
}

void _nx_log(enum nx_log_importance verbosity, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	log_callback(verbosity, fmt, args);
	va_end(args);
}

enum nx_log_importance nx_log_get_verbosity(void) {
	return log_importance;
}

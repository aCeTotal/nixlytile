#ifndef PLATFORM_LOG_LOG_H
#define PLATFORM_LOG_LOG_H

#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

enum nx_log_importance {
	NX_ERROR = 1,
	NX_INFO = 2,
	NX_DEBUG = 3,
};

typedef void (*nx_log_func_t)(enum nx_log_importance importance,
	const char *fmt, va_list args);

// Replaces the stderr sink.
void nx_log_init(enum nx_log_importance verbosity, nx_log_func_t callback);
enum nx_log_importance nx_log_get_verbosity(void);

void _nx_log(enum nx_log_importance verbosity, const char *format, ...)
	__attribute__((format(printf, 2, 3)));
void _nx_vlog(enum nx_log_importance verbosity, const char *format, va_list args)
	__attribute__((format(printf, 2, 0)));

#define nx_log(verb, fmt, ...) \
	_nx_log(verb, "[%s:%d] " fmt, __FILE__, __LINE__ __VA_OPT__(,) __VA_ARGS__)
#define nx_log_errno(verb, fmt, ...) \
	nx_log(verb, fmt ": %s" __VA_OPT__(,) __VA_ARGS__, strerror(errno))

#endif

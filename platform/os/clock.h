#ifndef PLATFORM_OS_CLOCK_H
#define PLATFORM_OS_CLOCK_H

#include <stdint.h>
#include <time.h>

static const long NSEC_PER_SEC = 1000000000;

// CLOCK_MONOTONIC in milliseconds.
int64_t get_current_time_msec(void);
int64_t timespec_to_msec(const struct timespec *a);
// r = a - b
void timespec_sub(struct timespec *r, const struct timespec *a,
		const struct timespec *b);

#endif

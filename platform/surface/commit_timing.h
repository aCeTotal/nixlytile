#ifndef PLATFORM_SURFACE_COMMIT_TIMING_H
#define PLATFORM_SURFACE_COMMIT_TIMING_H

#include <wayland-server-core.h>

struct nx_commit_timing_manager_v1;

// Targets use CLOCK_MONOTONIC
struct nx_commit_timing_manager_v1 *nx_commit_timing_manager_v1_create(
	struct wl_display *display, uint32_t version);

#endif

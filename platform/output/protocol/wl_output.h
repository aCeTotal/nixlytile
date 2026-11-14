#ifndef PLATFORM_OUTPUT_PROTOCOL_WL_OUTPUT_H
#define PLATFORM_OUTPUT_PROTOCOL_WL_OUTPUT_H

#include "platform/output/output.h"

enum output_global_change {
	OUTPUT_GLOBAL_MODE = 1 << 0,
	OUTPUT_GLOBAL_GEOMETRY = 1 << 1,
	OUTPUT_GLOBAL_SCALE = 1 << 2,
};

// Resends changed wl_output state.
void output_global_send_changes(struct nx_output *output, uint32_t changes);

#endif

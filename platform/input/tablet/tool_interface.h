#ifndef PLATFORM_INPUT_TABLET_TOOL_INTERFACE_H
#define PLATFORM_INPUT_TABLET_TOOL_INTERFACE_H

#include "platform/input/tablet/tool_device.h"

struct nx_tablet_impl {
	const char *name;
};

void nx_tablet_init(struct nx_tablet *tablet,
	const struct nx_tablet_impl *impl, const char *name);
void nx_tablet_finish(struct nx_tablet *tablet);

#endif

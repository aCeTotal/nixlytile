#ifndef PLATFORM_INPUT_TABLET_PAD_INTERFACE_H
#define PLATFORM_INPUT_TABLET_PAD_INTERFACE_H

#include "platform/input/tablet/pad_device.h"

struct nx_tablet_pad_impl {
	const char *name;
};

void nx_tablet_pad_init(struct nx_tablet_pad *pad,
	const struct nx_tablet_pad_impl *impl, const char *name);
// Groups stay owned by the caller.
void nx_tablet_pad_finish(struct nx_tablet_pad *pad);

#endif

#ifndef PLATFORM_INPUT_POINTER_POINTER_INTERFACE_H
#define PLATFORM_INPUT_POINTER_POINTER_INTERFACE_H

#include "platform/input/pointer/pointer.h"

struct nx_pointer_impl {
	const char *name;
};

void nx_pointer_init(struct nx_pointer *pointer,
	const struct nx_pointer_impl *impl, const char *name);
// Releases held buttons first.
void nx_pointer_finish(struct nx_pointer *pointer);

// Tracks held buttons, then emits button.
void nx_pointer_notify_button(struct nx_pointer *pointer,
	struct nx_pointer_button_event *event);

#endif

#ifndef PLATFORM_SESSION_UDEV_H
#define PLATFORM_SESSION_UDEV_H

#include <stdbool.h>

struct nx_session;
struct udev_device;

/* Watches DRM card hotplug. */
bool session_udev_init(struct nx_session *session);
void session_udev_finish(struct nx_session *session);

bool session_on_seat(const struct nx_session *session,
	struct udev_device *dev);

#endif

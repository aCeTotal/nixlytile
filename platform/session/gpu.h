#ifndef PLATFORM_SESSION_GPU_H
#define PLATFORM_SESSION_GPU_H

#include "platform/session/session.h"

/* NULL unless a KMS device. */
struct nx_device *session_open_if_kms(struct nx_session *restrict session,
	const char *restrict path);

#endif

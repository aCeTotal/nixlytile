#ifndef NIXLY_VRS_LINK_H
#define NIXLY_VRS_LINK_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define VRS_LINK_SOCKET  "nixlytile-vrs-%u"
#define VRS_LINK_MAGIC   0x5356584eu
#define VRS_LINK_VERSION 6u
#define VRS_FOCUS_NONE   UINT32_MAX
#define VRS_FOCUS_SHIFT  16
#define VRS_FOCUS_MASK   0xffffu
#define VRS_UNIT         65534u /* 0xffff reserved for NONE */
#define VRS_SIZE_UNIT    1000u  /* radii: per mille of height */
#define VRS_QUALITY_MAX  100u

/* Compositor writes, layer reads. */
struct vrs_link {
	uint32_t magic, version;
	_Atomic uint32_t active;
	_Atomic uint32_t focus;         /* gaze x, y; NONE: centre */
	_Atomic uint32_t fovea;         /* native radius */
	_Atomic uint32_t blend;         /* transition width */
	_Atomic uint32_t quality;       /* periphery, 0..VRS_QUALITY_MAX */
};

_Static_assert(sizeof(struct vrs_link) == 28, "vrs_link ABI");

struct vrs_link_address {
	struct sockaddr_un addr;
	socklen_t len;
};

/* Abstract socket for this uid. */
static inline struct vrs_link_address
vrs_link_address(void)
{
	struct vrs_link_address a = { .addr = { .sun_family = AF_UNIX } };
	int n = snprintf(a.addr.sun_path + 1, sizeof(a.addr.sun_path) - 1,
		VRS_LINK_SOCKET, (unsigned int)getuid());

	a.len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
	return a;
}

#endif

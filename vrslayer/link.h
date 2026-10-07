#ifndef NIXLY_VRS_LINK_H
#define NIXLY_VRS_LINK_H

#include <stdatomic.h>
#include <stdint.h>

#define VRS_LINK_SOCKET  "nixlytile-vrs-%u"
#define VRS_LINK_MAGIC   0x5356584eu
#define VRS_LINK_VERSION 2u
#define VRS_CURSOR_NONE  UINT32_MAX
#define VRS_CURSOR_SCALE 65535u
#define VRS_LEVELS       16
/* Budget share coarsening aims for. */
#define VRS_FILL         0.9f

/* 32-bit fields: identical i686 layout. */
struct vrs_link {
	uint32_t magic;
	uint32_t version;
	_Atomic uint32_t active;
	_Atomic uint32_t cursor;          /* x << 16 | y, scaled */
	_Atomic uint32_t interval_ns;
	_Atomic uint32_t budget_ns;
	_Atomic uint32_t gpu_ns;
	_Atomic uint32_t gpu_min_ns;      /* predicted at coarsest level */
	_Atomic uint32_t level;
	_Atomic uint32_t level_max;
	_Atomic uint32_t saturated;
	_Atomic uint32_t frames;
};

_Static_assert(sizeof(struct vrs_link) == 48, "vrs_link is a shared ABI");

#endif

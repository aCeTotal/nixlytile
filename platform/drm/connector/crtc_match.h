#ifndef PLATFORM_DRM_CONNECTOR_CRTC_MATCH_H
#define PLATFORM_DRM_CONNECTOR_CRTC_MATCH_H

#include <stddef.h>
#include <stdint.h>

#define DRM_UNMATCHED ((uint32_t)-1)

/* CRTC bitmasks in, connector indices out. */
void drm_match_connectors_with_crtcs(size_t num_conns,
	const uint32_t conns[static restrict num_conns],
	size_t num_crtcs, const uint32_t prev_crtcs[static restrict num_crtcs],
	uint32_t new_crtcs[static restrict num_crtcs]);

#endif

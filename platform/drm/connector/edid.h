#ifndef PLATFORM_DRM_CONNECTOR_EDID_H
#define PLATFORM_DRM_CONNECTOR_EDID_H

#include <stddef.h>
#include <stdint.h>

struct nx_drm_connector;

/* Generated from hwdata pnp.ids. */
const char *drm_pnp_manufacturer(const char code[static 3]);

/* Sets make, model and colorimetry. */
void drm_parse_edid(struct nx_drm_connector *conn, size_t len,
	const uint8_t *data);

#endif

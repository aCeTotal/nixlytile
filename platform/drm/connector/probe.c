#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <xf86drmMode.h>
#include "platform/drm/connector/connector.h"
#include "platform/log/log.h"

/* Zero when unknown or unset. */
static uint32_t current_crtc_id(struct nx_drm_connector *conn,
		const drmModeConnector *drm_conn) {
	int fd = conn->backend->fd;

	if (conn->props.crtc_id != 0) {
		uint64_t value;
		if (!drm_get_prop(fd, conn->id, conn->props.crtc_id, &value)) {
			drm_conn_log(conn, NX_ERROR, "Failed to get CRTC_ID connector property");
			return 0;
		}
		return (uint32_t)value;
	}

	if (drm_conn->encoder_id == 0) {
		return 0;
	}

	drmModeEncoder *enc = drmModeGetEncoder(fd, drm_conn->encoder_id);
	if (enc == NULL) {
		drm_conn_log(conn, NX_ERROR, "drmModeGetEncoder() failed");
		return 0;
	}
	uint32_t crtc_id = enc->crtc_id;
	drmModeFreeEncoder(enc);
	return crtc_id;
}

static struct nx_drm_crtc *current_crtc(struct nx_drm_connector *conn,
		const drmModeConnector *drm_conn) {
	uint32_t crtc_id = current_crtc_id(conn, drm_conn);
	if (crtc_id == 0) {
		return NULL;
	}

	struct nx_drm_backend *drm = conn->backend;
	for (size_t i = 0; i < drm->num_crtcs; ++i) {
		if (drm->crtcs[i].id == crtc_id) {
			return &drm->crtcs[i];
		}
	}

	drm_conn_log(conn, NX_ERROR,
		"Failed to find current CRTC ID %" PRIu32, crtc_id);
	return NULL;
}

struct nx_drm_connector *drm_connector_create(struct nx_drm_backend *drm,
		const drmModeConnector *drm_conn) {
	struct nx_drm_connector *conn = calloc(1, sizeof(*conn));
	if (!conn) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return NULL;
	}

	conn->backend = drm;
	conn->status = DRM_MODE_DISCONNECTED;
	conn->id = drm_conn->connector_id;

	if (!drm_get_connector_props(drm->fd, conn->id, &conn->props)) {
		free(conn);
		return NULL;
	}

	const char *conn_type_name =
		drmModeGetConnectorTypeName(drm_conn->connector_type);
	if (conn_type_name == NULL) {
		conn_type_name = "Unknown";
	}

	snprintf(conn->name, sizeof(conn->name),
		"%s-%"PRIu32, conn_type_name, drm_conn->connector_type_id);

	conn->possible_crtcs =
		drmModeConnectorGetPossibleCrtcs(drm->fd, drm_conn);
	if (conn->possible_crtcs == 0) {
		drm_conn_log(conn, NX_ERROR, "No CRTC possible");
	}

	conn->crtc = current_crtc(conn, drm_conn);

	wl_list_insert(drm->connectors.prev, &conn->link);
	nx_log(NX_INFO, "Found connector '%s'", conn->name);
	return conn;
}

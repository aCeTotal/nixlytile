#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <xf86drmMode.h>
#include "platform/drm/connector/connector.h"
#include "platform/log/log.h"

/* Tears down the output only. */
static void disconnect(struct nx_drm_connector *conn) {
	if (conn->status == DRM_MODE_DISCONNECTED) {
		return;
	}

	nx_output_destroy(&conn->output);

	assert(conn->status == DRM_MODE_DISCONNECTED);
}

void drm_connector_destroy(struct nx_drm_connector *conn) {
	disconnect(conn);

	wl_list_remove(&conn->link);
	free(conn);
}

static ssize_t find_connector(struct nx_drm_connector *const *conns,
		size_t len, uint32_t id) {
	for (size_t i = 0; i < len; i++) {
		if (conns[i]->id == id) {
			return (ssize_t)i;
		}
	}
	return -1;
}

static void mark_seen(bool *seen, ssize_t index) {
	if (index >= 0) {
		seen[index] = true;
	}
}

/* Bad links need fresh modes. */
static bool check_link_status(struct nx_drm_connector *conn) {
	if (conn->props.link_status == 0) {
		return true;
	}

	uint64_t link_status;
	if (!drm_get_prop(conn->backend->fd, conn->id,
			conn->props.link_status, &link_status)) {
		drm_conn_log(conn, NX_ERROR, "Failed to get link status prop");
		return false;
	}

	if (link_status == DRM_MODE_LINK_STATUS_BAD) {
		drm_conn_log(conn, NX_INFO, "Bad link detected");
		disconnect(conn);
	}
	return true;
}

/* True when newly connected. */
static bool update_connection(struct nx_drm_connector *conn,
		const drmModeConnector *drm_conn) {
	if (!check_link_status(conn)) {
		return false;
	}

	if (conn->status == DRM_MODE_DISCONNECTED &&
			drm_conn->connection == DRM_MODE_CONNECTED) {
		nx_log(NX_INFO, "'%s' connected", conn->name);
		if (!drm_connector_connect(conn, drm_conn)) {
			drm_conn_log(conn, NX_ERROR, "Failed to connect DRM connector");
			return false;
		}
		return true;
	}

	if (conn->status == DRM_MODE_CONNECTED &&
			drm_conn->connection != DRM_MODE_CONNECTED) {
		nx_log(NX_INFO, "'%s' disconnected", conn->name);
		disconnect(conn);
	}
	return false;
}

/* Reverse list order. */
static void destroy_unseen(struct nx_drm_connector *const *conns,
		const bool *seen, size_t len) {
	for (size_t i = len; i-- > 0;) {
		if (seen[i]) {
			continue;
		}
		nx_log(NX_INFO, "'%s' disappeared", conns[i]->name);
		drm_connector_destroy(conns[i]);
	}
}

void drm_scan_connectors(struct nx_drm_backend *drm,
		struct nx_device_hotplug_event *event) {
	uint32_t only_id = event != NULL ? event->connector_id : 0;
	if (only_id != 0) {
		nx_log(NX_INFO, "Scanning DRM connector %"PRIu32" on %s",
			only_id, drm->name);
	} else {
		nx_log(NX_INFO, "Scanning DRM connectors on %s", drm->name);
	}

	drmModeRes *res = drmModeGetResources(drm->fd);
	if (!res) {
		nx_log_errno(NX_ERROR, "Failed to get DRM resources");
		return;
	}

	// +1 avoids zero-length arrays
	size_t old_len = wl_list_length(&drm->connectors);
	struct nx_drm_connector *old[old_len + 1];
	bool seen[old_len + 1];
	memset(seen, false, sizeof(seen));
	size_t n = 0;
	struct nx_drm_connector *conn;
	wl_list_for_each(conn, &drm->connectors, link) {
		old[n++] = conn;
	}

	size_t new_outputs_len = 0;
	struct nx_drm_connector *new_outputs[res->count_connectors + 1];

	for (int i = 0; i < res->count_connectors; ++i) {
		uint32_t conn_id = res->connectors[i];
		ssize_t index = find_connector(old, old_len, conn_id);

		if (only_id != 0 && only_id != conn_id) {
			mark_seen(seen, index);
			continue;
		}

		drmModeConnector *drm_conn = drmModeGetConnector(drm->fd, conn_id);
		if (!drm_conn) {
			nx_log_errno(NX_ERROR, "Failed to get DRM connector");
			continue;
		}

		mark_seen(seen, index);
		conn = index >= 0 ? old[index] : drm_connector_create(drm, drm_conn);
		if (conn != NULL && update_connection(conn, drm_conn)) {
			new_outputs[new_outputs_len++] = conn;
		}
		drmModeFreeConnector(drm_conn);
	}

	drmModeFreeResources(res);

	destroy_unseen(old, seen, old_len);
	for (size_t i = 0; i < new_outputs_len; ++i) {
		drm_conn_log(new_outputs[i], NX_INFO, "Requesting modeset");
		wl_signal_emit_mutable(&drm->backend.events.new_output,
			&new_outputs[i]->output);
	}
}

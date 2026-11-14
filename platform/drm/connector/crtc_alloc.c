#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include "platform/drm/commit/commit.h"
#include "platform/drm/connector/connector.h"
#include "platform/drm/connector/crtc_match.h"
#include "platform/log/log.h"

static const char *connection_str(drmModeConnection status) {
	switch (status) {
	case DRM_MODE_CONNECTED:
		return "connected";
	case DRM_MODE_DISCONNECTED:
		return "disconnected";
	case DRM_MODE_UNKNOWNCONNECTION:
		return "unknown";
	}
	return "<unsupported>";
}

static void format_nullable_crtc(char *str, size_t size, const struct nx_drm_crtc *crtc) {
	if (crtc != NULL) {
		snprintf(str, size, "CRTC %"PRIu32, crtc->id);
	} else {
		snprintf(str, size, "no CRTC");
	}
}

static void log_crtc_change(const struct nx_drm_connector *conn,
		const struct nx_drm_crtc *new_crtc, bool needs_crtc) {
	char old_crtc_str[16], new_crtc_str[16];
	format_nullable_crtc(old_crtc_str, sizeof(old_crtc_str), conn->crtc);
	format_nullable_crtc(new_crtc_str, sizeof(new_crtc_str), new_crtc);

	char crtc_str[64];
	if (conn->crtc != new_crtc) {
		snprintf(crtc_str, sizeof(crtc_str), "%s → %s", old_crtc_str, new_crtc_str);
	} else {
		snprintf(crtc_str, sizeof(crtc_str), "%s (no change)", new_crtc_str);
	}

	nx_log(NX_DEBUG, "  Connector %s (%s%s): %s",
		conn->name, connection_str(conn->status),
		needs_crtc ? ", needs CRTC" : "", crtc_str);
}

/* Enabled outputs keep their CRTC. */
static bool keeps_enabled_crtc(const struct nx_drm_connector *conn,
		const struct nx_drm_crtc *new_crtc) {
	if (conn->status != DRM_MODE_CONNECTED || !conn->output.enabled) {
		return true;
	}
	if (new_crtc == NULL) {
		nx_log(NX_DEBUG, "Could not match a CRTC for previously connected output; "
			"keeping old configuration");
		return false;
	}
	assert(conn->crtc != NULL);
	if (new_crtc != conn->crtc) {
		nx_log(NX_DEBUG, "Cannot switch CRTC for enabled output; "
			"keeping old configuration");
		return false;
	}
	return true;
}

static void assign_crtc(struct nx_drm_connector *conn, struct nx_drm_crtc *crtc) {
	if (conn->crtc != NULL && crtc != NULL) {
		return;
	}
	drm_connector_dealloc_crtc(conn);
	if (crtc != NULL) {
		conn->crtc = crtc;
	}
}

static void realloc_crtcs(struct nx_drm_backend *drm,
		struct nx_drm_connector *want_conn) {
	assert(drm->num_crtcs > 0);

	size_t num_connectors = wl_list_length(&drm->connectors);
	if (num_connectors == 0) {
		return;
	}

	nx_log(NX_DEBUG, "Reallocating CRTCs");

	struct nx_drm_connector *connectors[num_connectors];
	uint32_t connector_constraints[num_connectors];
	uint32_t previous_match[drm->num_crtcs];
	uint32_t new_match[drm->num_crtcs];
	struct nx_drm_crtc *connector_match[num_connectors];

	for (size_t i = 0; i < drm->num_crtcs; ++i) {
		previous_match[i] = DRM_UNMATCHED;
	}

	size_t n = 0;
	struct nx_drm_connector *conn;
	wl_list_for_each(conn, &drm->connectors, link) {
		connectors[n] = conn;
		connector_match[n] = NULL;
		if (conn->crtc) {
			previous_match[conn->crtc - drm->crtcs] = n;
		}
		// Enabled or requested connectors only
		bool want_crtc = conn == want_conn || conn->output.enabled;
		connector_constraints[n] = conn->status == DRM_MODE_CONNECTED && want_crtc ?
			conn->possible_crtcs : 0;
		++n;
	}

	drm_match_connectors_with_crtcs(num_connectors, connector_constraints,
		drm->num_crtcs, previous_match, new_match);

	for (size_t i = 0; i < drm->num_crtcs; ++i) {
		if (new_match[i] != DRM_UNMATCHED) {
			connector_match[new_match[i]] = &drm->crtcs[i];
		}
	}

	for (size_t i = 0; i < num_connectors; ++i) {
		log_crtc_change(connectors[i], connector_match[i], connector_constraints[i] != 0);
	}

	for (size_t i = 0; i < num_connectors; ++i) {
		if (!keeps_enabled_crtc(connectors[i], connector_match[i])) {
			return;
		}
	}

	for (size_t i = 0; i < num_connectors; ++i) {
		assign_crtc(connectors[i], connector_match[i]);
	}
}

bool drm_connector_alloc_crtc(struct nx_drm_connector *conn) {
	if (conn->crtc == NULL) {
		realloc_crtcs(conn->backend, conn);
	}
	if (conn->crtc != NULL) {
		return true;
	}
	drm_conn_log(conn, NX_DEBUG, "Failed to find free CRTC");
	return false;
}

void drm_connector_dealloc_crtc(struct nx_drm_connector *conn) {
	if (conn->crtc == NULL) {
		return;
	}

	drm_conn_log(conn, NX_DEBUG, "De-allocating CRTC %" PRIu32,
		conn->crtc->id);

	struct nx_output_state state;
	nx_output_state_init(&state);
	nx_output_state_set_enabled(&state, false);
	if (!drm_connector_commit_state(conn, &state, false)) {
		// Fails with EPERM on unplug
		drm_conn_log(conn, NX_ERROR, "Failed to disable CRTC %"PRIu32,
			conn->crtc->id);
	}
	nx_output_state_finish(&state);
}

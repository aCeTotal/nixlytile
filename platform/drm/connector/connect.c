#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drmMode.h>
#include "platform/drm/connector/connector.h"
#include "platform/drm/connector/edid.h"
#include "platform/log/log.h"
#include "platform/output/output_interface.h"

static const int32_t subpixel_map[] = {
	[DRM_MODE_SUBPIXEL_UNKNOWN] = WL_OUTPUT_SUBPIXEL_UNKNOWN,
	[DRM_MODE_SUBPIXEL_HORIZONTAL_RGB] = WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB,
	[DRM_MODE_SUBPIXEL_HORIZONTAL_BGR] = WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR,
	[DRM_MODE_SUBPIXEL_VERTICAL_RGB] = WL_OUTPUT_SUBPIXEL_VERTICAL_RGB,
	[DRM_MODE_SUBPIXEL_VERTICAL_BGR] = WL_OUTPUT_SUBPIXEL_VERTICAL_BGR,
	[DRM_MODE_SUBPIXEL_NONE] = WL_OUTPUT_SUBPIXEL_NONE,
};

static drmModeModeInfo *legacy_current_mode(int fd, uint32_t crtc_id) {
	drmModeCrtc *drm_crtc = drmModeGetCrtc(fd, crtc_id);
	if (drm_crtc == NULL) {
		nx_log_errno(NX_ERROR, "drmModeGetCrtc failed");
		return NULL;
	}
	if (!drm_crtc->mode_valid) {
		drmModeFreeCrtc(drm_crtc);
		return NULL;
	}

	drmModeModeInfo *mode = malloc(sizeof(*mode));
	if (mode == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
	} else {
		*mode = drm_crtc->mode;
	}
	drmModeFreeCrtc(drm_crtc);
	return mode;
}

static drmModeModeInfo *current_mode(struct nx_drm_connector *conn) {
	if (conn->crtc == NULL) {
		return NULL;
	}
	if (conn->crtc->props.mode_id == 0) {
		return legacy_current_mode(conn->backend->fd, conn->crtc->id);
	}

	size_t size = 0;
	drmModeModeInfo *mode = drm_get_prop_blob(conn->backend->fd,
		conn->crtc->id, conn->crtc->props.mode_id, &size);
	assert(mode == NULL || size == sizeof(*mode));
	return mode;
}

static void free_modes(struct wl_list *modes) {
	struct nx_drm_mode *mode, *tmp;
	wl_list_for_each_safe(mode, tmp, modes, nx_mode.link) {
		wl_list_remove(&mode->nx_mode.link);
		free(mode);
	}
}

static bool collect_modes(const drmModeConnector *drm_conn, struct wl_list *modes) {
	nx_log(NX_INFO, "Detected modes:");

	for (int i = 0; i < drm_conn->count_modes; ++i) {
		if (drm_conn->modes[i].flags & DRM_MODE_FLAG_INTERLACE) {
			continue;
		}

		struct nx_drm_mode *mode = drm_mode_create(&drm_conn->modes[i]);
		if (!mode) {
			nx_log_errno(NX_ERROR, "Allocation failed");
			free_modes(modes);
			return false;
		}
		mode->probed = true;

		nx_log(NX_INFO, "  %"PRId32"x%"PRId32" @ %.3f Hz %s",
			mode->nx_mode.width, mode->nx_mode.height,
			(float)mode->nx_mode.refresh / 1000,
			mode->nx_mode.preferred ? "(preferred)" : "");

		wl_list_insert(modes->prev, &mode->nx_mode.link);
	}
	return true;
}

/* Adopt the live CRTC mode. */
static void adopt_current_mode(struct nx_drm_connector *conn,
		struct wl_list *modes, struct nx_output_state *state) {
	drmModeModeInfo *current = current_mode(conn);
	if (current == NULL) {
		return;
	}

	struct nx_drm_mode *match = NULL, *mode;
	wl_list_for_each(mode, modes, nx_mode.link) {
		if (memcmp(&mode->drm_mode, current, sizeof(*current)) == 0) {
			match = mode;
		}
	}

	int32_t refresh = drm_calculate_refresh_rate(current);
	if (match != NULL) {
		nx_output_state_set_mode(state, &match->nx_mode);
	} else {
		nx_output_state_set_custom_mode(state,
			current->hdisplay, current->vdisplay, refresh);
	}

	uint64_t mode_id = 0;
	drm_get_prop(conn->backend->fd, conn->crtc->id,
		conn->crtc->props.mode_id, &mode_id);

	conn->crtc->own_mode_id = false;
	conn->crtc->mode_id = mode_id;
	conn->refresh = refresh;
	free(current);
}

static void read_geometry(struct nx_drm_connector *conn,
		const drmModeConnector *drm_conn) {
	struct nx_output *output = &conn->output;

	output->phys_width = drm_conn->mmWidth;
	output->phys_height = drm_conn->mmHeight;
	nx_log(NX_INFO, "Physical size: %"PRId32"x%"PRId32,
		output->phys_width, output->phys_height);
	if (drm_conn->subpixel >= sizeof(subpixel_map) / sizeof(subpixel_map[0])) {
		nx_log(NX_ERROR, "Unknown subpixel value: %d", (int)drm_conn->subpixel);
		return;
	}
	output->subpixel = subpixel_map[drm_conn->subpixel];
}

static void read_capabilities(struct nx_drm_connector *conn) {
	struct nx_output *output = &conn->output;
	int fd = conn->backend->fd;

	uint64_t non_desktop;
	if (drm_get_prop(fd, conn->id, conn->props.non_desktop, &non_desktop)) {
		if (non_desktop == 1) {
			nx_log(NX_INFO, "Non-desktop connector");
		}
		output->non_desktop = non_desktop;
	}

	memset(conn->max_bpc_bounds, 0, sizeof(conn->max_bpc_bounds));
	if (conn->props.max_bpc != 0 && !drm_get_prop_range(fd, conn->props.max_bpc,
			&conn->max_bpc_bounds[0], &conn->max_bpc_bounds[1])) {
		nx_log(NX_ERROR, "Failed to introspect 'max bpc' property");
	}

	uint64_t vrr_capable = 0;
	if (conn->props.vrr_capable != 0) {
		drm_get_prop(fd, conn->id, conn->props.vrr_capable, &vrr_capable);
	}
	output->adaptive_sync_supported = vrr_capable;
}

static void read_edid(struct nx_drm_connector *conn) {
	size_t edid_len = 0;
	uint8_t *edid = drm_get_prop_blob(conn->backend->fd,
		conn->id, conn->props.edid, &edid_len);
	if (edid_len > 0) {
		drm_parse_edid(conn, edid_len, edid);
	} else {
		nx_log(NX_DEBUG, "Connector has no EDID");
	}
	free(edid);
}

static void set_description(struct nx_drm_connector *conn) {
	struct nx_output *output = &conn->output;

	char *subconnector = NULL;
	if (conn->props.subconnector) {
		subconnector = drm_get_prop_enum(conn->backend->fd,
			conn->id, conn->props.subconnector);
	}
	if (subconnector && strcmp(subconnector, "Native") == 0) {
		free(subconnector);
		subconnector = NULL;
	}

	char description[128];
	snprintf(description, sizeof(description), "%s %s%s%s (%s%s%s)",
		output->make, output->model,
		output->serial ? " " : "",
		output->serial ? output->serial : "",
		output->name,
		subconnector ? " via " : "",
		subconnector ? subconnector : "");
	nx_output_set_description(output, description);

	free(subconnector);
}

bool drm_connector_connect(struct nx_drm_connector *conn,
		const drmModeConnector *drm_conn) {
	struct nx_drm_backend *drm = conn->backend;
	struct nx_output *output = &conn->output;

	nx_log(NX_DEBUG, "Current CRTC: %d",
		conn->crtc ? (int)conn->crtc->id : -1);

	// Added after nx_output_init
	struct wl_list modes;
	wl_list_init(&modes);

	struct nx_output_state state;
	nx_output_state_init(&state);
	nx_output_state_set_enabled(&state, conn->crtc != NULL);

	if (!collect_modes(drm_conn, &modes)) {
		nx_output_state_finish(&state);
		return false;
	}
	adopt_current_mode(conn, &modes, &state);

	nx_output_init(output, &drm->backend, &drm_output_impl, drm->session->event_loop, &state);
	nx_output_state_finish(&state);

	wl_list_insert_list(&output->modes, &modes);
	nx_output_set_name(output, conn->name);

	read_geometry(conn, drm_conn);
	read_capabilities(conn);
	read_edid(conn);
	set_description(conn);

	conn->status = DRM_MODE_CONNECTED;
	return true;
}

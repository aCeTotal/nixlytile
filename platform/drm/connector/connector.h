#ifndef PLATFORM_DRM_CONNECTOR_CONNECTOR_H
#define PLATFORM_DRM_CONNECTOR_CONNECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-util.h>
#include <xf86drmMode.h>
#include "platform/drm/device/device.h"
#include "platform/log/log.h"
#include "platform/output/output.h"

struct nx_drm_page_flip;

/* Layout mirrored by src/output.c. */
struct nx_drm_mode {
	struct nx_output_mode nx_mode;
	drmModeModeInfo drm_mode;
	/* Flips every Nth vblank */
	int pace_divisor;
};

struct nx_drm_connector {
	struct nx_output output; // valid while connected

	struct nx_drm_backend *backend;
	char name[24];
	drmModeConnection status;
	uint32_t id;
	uint64_t max_bpc_bounds[2];

	struct nx_drm_crtc *crtc;
	uint32_t possible_crtcs;

	struct nx_drm_connector_props props;

	bool cursor_enabled;
	int cursor_x, cursor_y;
	int cursor_width, cursor_height;
	int cursor_hotspot_x, cursor_hotspot_y;
	/* Submitted with the next page-flip */
	struct nx_drm_fb *cursor_pending_fb;

	struct wl_list link; // nx_drm_backend.connectors

	/* Last committed page-flip */
	struct nx_drm_page_flip *pending_page_flip;

	/* Atomic only */
	uint32_t colorspace;
	uint32_t hdr_output_metadata;
	uint32_t content_type;
	bool content_type_needs_modeset;

	int32_t refresh;
};

#if __STDC_VERSION__ >= 202311L
#define drm_conn_log(conn, verb, fmt, ...) \
	nx_log(verb, "connector %s: " fmt, (conn)->name __VA_OPT__(,) __VA_ARGS__)
#define drm_conn_log_errno(conn, verb, fmt, ...) \
	nx_log_errno(verb, "connector %s: " fmt, (conn)->name __VA_OPT__(,) __VA_ARGS__)
#else
#define drm_conn_log(conn, verb, fmt, ...) \
	nx_log(verb, "connector %s: " fmt, (conn)->name, ##__VA_ARGS__)
#define drm_conn_log_errno(conn, verb, fmt, ...) \
	nx_log_errno(verb, "connector %s: " fmt, (conn)->name, ##__VA_ARGS__)
#endif

extern const struct nx_output_impl drm_output_impl;

struct nx_drm_connector *drm_connector_from_output(struct nx_output *output);
struct nx_drm_connector *drm_connector_create(struct nx_drm_backend *drm,
	const drmModeConnector *drm_conn);
bool drm_connector_connect(struct nx_drm_connector *conn,
	const drmModeConnector *drm_conn);
void drm_connector_destroy(struct nx_drm_connector *conn);
void drm_scan_connectors(struct nx_drm_backend *drm,
	struct nx_device_hotplug_event *event);

bool drm_connector_alloc_crtc(struct nx_drm_connector *conn);
void drm_connector_dealloc_crtc(struct nx_drm_connector *conn);

bool drm_connector_set_cursor(struct nx_output *output,
	struct nx_buffer *buffer, int hotspot_x, int hotspot_y);
bool drm_connector_move_cursor(struct nx_output *output, int x, int y);
bool drm_connector_is_cursor_visible(struct nx_drm_connector *conn);
const struct nx_drm_format_set *drm_connector_get_cursor_formats(
	struct nx_output *output, uint32_t buffer_caps);
const struct nx_output_cursor_size *drm_connector_get_cursor_sizes(
	struct nx_output *output, size_t *len);

/* Refresh rate in mHz. */
int32_t drm_calculate_refresh_rate(const drmModeModeInfo *mode);
struct nx_drm_mode *drm_mode_create(const drmModeModeInfo *modeinfo);
bool drm_scaled_edid_mode(struct nx_output *output, drmModeModeInfo *out,
	int32_t width, int32_t height, int32_t refresh);
void drm_generate_cvt_mode(drmModeModeInfo *mode, int hdisplay, int vdisplay,
	float vrefresh);

#endif

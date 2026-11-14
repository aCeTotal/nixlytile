#ifndef PLATFORM_DRM_COMMIT_COMMIT_H
#define PLATFORM_DRM_COMMIT_COMMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xf86drmMode.h>
#include "platform/drm/connector/connector.h"
#include "platform/output/output.h"

/* Output state applied through KMS. */
#define DRM_COMMIT_OUTPUT_STATE ( \
	NX_OUTPUT_STATE_BUFFER | \
	NX_OUTPUT_STATE_MODE | \
	NX_OUTPUT_STATE_ENABLED | \
	NX_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED | \
	NX_OUTPUT_STATE_WAIT_TIMELINE | \
	NX_OUTPUT_STATE_SIGNAL_TIMELINE | \
	NX_OUTPUT_STATE_COLOR_TRANSFORM | \
	NX_OUTPUT_STATE_IMAGE_DESCRIPTION | \
	NX_OUTPUT_STATE_CONTENT_TYPE)

struct nx_drm_device_state {
	bool modeset;
	bool nonblock;

	struct nx_drm_connector_state *connectors;
	size_t connectors_len;
};

struct nx_drm_connector_state {
	struct nx_drm_connector *connector;
	const struct nx_output_state *base;
	bool active;
	drmModeModeInfo mode;
	struct nx_drm_fb *primary_fb;
	struct nx_drm_viewport primary_viewport;
	struct nx_drm_fb *cursor_fb;

	struct nx_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;

	/* Atomic only */
	uint32_t mode_id;
	uint32_t gamma_lut;
	uint32_t fb_damage_clips;
	int primary_in_fence_fd;
	bool vrr_enabled;
	uint32_t colorspace;
	uint32_t hdr_output_metadata;
	uint32_t content_type;
	bool content_type_deferred;
};

/* Superseded connectors become NULL. */
struct nx_drm_page_flip {
	struct wl_list link; // nx_drm_backend.page_flips
	struct nx_drm_page_flip_connector *connectors;
	size_t connectors_len;
	bool async;
};

struct nx_drm_page_flip_connector {
	uint32_t crtc_id;
	struct nx_drm_connector *connector;
};

/* Atomic or legacy KMS. */
struct nx_drm_interface {
	bool (*commit)(struct nx_drm_backend *drm,
		const struct nx_drm_device_state *state,
		struct nx_drm_page_flip *page_flip, uint32_t flags, bool test_only);
};

extern const struct nx_drm_interface drm_atomic_iface;
extern const struct nx_drm_interface drm_legacy_iface;

bool drm_connector_commit_state(struct nx_drm_connector *conn,
	const struct nx_output_state *state, bool test_only);

void drm_connector_state_init(struct nx_drm_connector_state *state,
	struct nx_drm_connector *conn, const struct nx_output_state *base);
void drm_connector_state_finish(struct nx_drm_connector_state *state);
bool drm_connector_prepare(struct nx_drm_connector_state *conn_state,
	bool test_only);

struct nx_drm_page_flip *drm_page_flip_create(struct nx_drm_backend *drm,
	const struct nx_drm_device_state *state);
void drm_page_flip_destroy(struct nx_drm_page_flip *page_flip);
struct nx_drm_connector *drm_page_flip_pop(struct nx_drm_page_flip *page_flip,
	uint32_t crtc_id);
void drm_connector_set_pending_page_flip(struct nx_drm_connector *conn,
	struct nx_drm_page_flip *page_flip);
int drm_handle_event(int fd, uint32_t mask, void *data);

#endif

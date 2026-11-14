#ifndef PLATFORM_DRM_ATOMIC_ATOMIC_H
#define PLATFORM_DRM_ATOMIC_ATOMIC_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <xf86drmMode.h>
#include "platform/drm/commit/commit.h"

struct nx_buffer;
struct nx_output_image_description;

/* Errors stick until commit. */
struct drm_atomic {
	drmModeAtomicReq *req;
	bool failed;
};

void drm_atomic_begin(struct drm_atomic *atom);
void drm_atomic_add(struct drm_atomic *atom, uint32_t id, uint32_t prop, uint64_t val);
bool drm_atomic_commit(struct drm_atomic *atom, struct nx_drm_backend *drm,
	const struct nx_drm_device_state *state,
	struct nx_drm_page_flip *page_flip, uint32_t flags);
void drm_atomic_finish(struct drm_atomic *atom);

bool drm_atomic_mode_blob(const struct nx_drm_connector_state *state,
	uint32_t *blob_id);
bool drm_atomic_gamma_blob(struct nx_drm_backend *drm,
	struct nx_color_transform *transform, uint32_t *blob_id);
uint32_t drm_atomic_damage_blob(struct nx_drm_backend *drm,
	const struct nx_buffer *buffer, const pixman_region32_t *damage);
bool drm_atomic_hdr_metadata_blob(struct nx_drm_backend *drm,
	const struct nx_output_image_description *img_desc, uint32_t *blob_id);

bool drm_atomic_connector_prepare(struct nx_drm_connector_state *state,
	bool modeset);
void drm_atomic_connector_apply_commit(struct nx_drm_connector_state *state);
void drm_atomic_connector_rollback_commit(struct nx_drm_connector_state *state);
void drm_atomic_connector_add(struct drm_atomic *atom,
	struct nx_drm_connector_state *state, bool modeset);

/* CPU-side fence wait, then close. */
void drm_atomic_wait_fence(int fd);

#endif

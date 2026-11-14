#ifndef PLATFORM_SURFACE_COMPOSITOR_H
#define PLATFORM_SURFACE_COMPOSITOR_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>
#include "platform/container/addon.h"
#include "platform/geometry/box.h"

struct nx_surface;

enum nx_surface_state_field {
	NX_SURFACE_STATE_BUFFER = 1 << 0,
	NX_SURFACE_STATE_SURFACE_DAMAGE = 1 << 1,
	NX_SURFACE_STATE_BUFFER_DAMAGE = 1 << 2,
	NX_SURFACE_STATE_OPAQUE_REGION = 1 << 3,
	NX_SURFACE_STATE_INPUT_REGION = 1 << 4,
	NX_SURFACE_STATE_TRANSFORM = 1 << 5,
	NX_SURFACE_STATE_SCALE = 1 << 6,
	NX_SURFACE_STATE_FRAME_CALLBACK_LIST = 1 << 7,
	NX_SURFACE_STATE_VIEWPORT = 1 << 8,
	NX_SURFACE_STATE_OFFSET = 1 << 9,
};

struct nx_surface_state {
	uint32_t committed; // enum nx_surface_state_field
	uint32_t seq; // wraps around

	struct nx_buffer *buffer;
	int32_t dx, dy; // relative to previous position
	pixman_region32_t surface_damage, buffer_damage; // clipped to bounds
	pixman_region32_t opaque, input;
	enum wl_output_transform transform;
	int32_t scale;
	struct wl_list frame_callback_list; // wl_resource

	int width, height; // surface-local
	int buffer_width, buffer_height;

	struct wl_list subsurfaces_below;
	struct wl_list subsurfaces_above;

	// Applied after transform and scale
	struct {
		bool has_src, has_dst;
		struct nx_fbox src;
		int dst_width, dst_height; // surface-local
	} viewport;

	size_t cached_state_locks;
	struct wl_list cached_state_link; // nx_surface.cached

	struct wl_array synced; // void *, indexed by nx_surface_synced.index
};

struct nx_surface_role {
	const char *name;
	bool no_object; // e.g. cursor surfaces

	// Optional; need the role object
	void (*client_commit)(struct nx_surface *surface);
	void (*commit)(struct nx_surface *surface);
	void (*map)(struct nx_surface *surface);
	void (*unmap)(struct nx_surface *surface);
	void (*destroy)(struct nx_surface *surface);
};

struct nx_surface_output {
	struct nx_surface *surface;
	struct nx_output *output;

	struct wl_list link; // nx_surface.current_outputs

	struct {
		struct wl_listener bind;
		struct wl_listener destroy;

		bool suspended;
	};
};

struct nx_surface {
	struct wl_resource *resource;
	struct nx_compositor *compositor;
	struct nx_client_buffer *buffer; // NULL if upload failed
	pixman_region32_t buffer_damage; // last commit, buffer-local
	pixman_region32_t opaque_region; // surface-local, clipped
	pixman_region32_t input_region; // surface-local, clipped

	struct nx_surface_state current, pending;
	struct wl_list cached; // nx_surface_state.cached_link

	bool mapped;
	const struct nx_surface_role *role; // set for the surface lifetime
	struct wl_resource *role_resource;

	struct {
		struct wl_signal client_commit;
		struct wl_signal commit;
		struct wl_signal map;
		struct wl_signal unmap;
		struct wl_signal new_subsurface; // struct nx_subsurface, on parent commit
		struct wl_signal destroy;
	} events;

	struct wl_list current_outputs; // nx_surface_output.link

	struct nx_addon_set addons;
	void *data;

	struct {
		struct wl_listener role_resource_destroy;

		bool unmap_commit;

		bool opaque;

		bool handling_commit;
		bool pending_rejected;

		int32_t preferred_buffer_scale;
		bool preferred_buffer_transform_sent;
		enum wl_output_transform preferred_buffer_transform;

		struct wl_list synced; // nx_surface_synced.link
		size_t synced_len;

		struct wl_resource *pending_buffer_resource;
		struct wl_listener pending_buffer_resource_destroy;
	};
};

struct nx_renderer;

struct nx_compositor {
	struct wl_global *global;
	struct nx_renderer *renderer; // may be NULL

	struct {
		struct wl_signal new_surface;
		struct wl_signal destroy;
	} events;

	struct {
		struct wl_listener display_destroy;
		struct wl_listener renderer_destroy;
	};
};

typedef void (*nx_surface_iterator_func_t)(struct nx_surface *surface,
	int sx, int sy, void *data);

bool nx_surface_set_role(struct nx_surface *surface, const struct nx_surface_role *role,
	struct wl_resource *error_resource, uint32_t error_code);
void nx_surface_set_role_object(struct nx_surface *surface, struct wl_resource *role_resource);

// Only for role implementations
void nx_surface_map(struct nx_surface *surface);
void nx_surface_unmap(struct nx_surface *surface);

// Only while handling a commit
void nx_surface_reject_pending(struct nx_surface *surface, struct wl_resource *resource,
	uint32_t code, const char *msg, ...);

bool nx_surface_has_buffer(struct nx_surface *surface);

// True even after buffer release
bool nx_surface_state_has_buffer(const struct nx_surface_state *state);

struct nx_texture *nx_surface_get_texture(struct nx_surface *surface);

struct nx_surface *nx_surface_get_root_surface(struct nx_surface *surface);

// Ignores subsurfaces
bool nx_surface_point_accepts_input(struct nx_surface *surface,
	double sx, double sy);

void nx_surface_send_enter(struct nx_surface *surface,
	struct nx_output *output);
void nx_surface_send_leave(struct nx_surface *surface,
	struct nx_output *output);
void nx_surface_send_frame_done(struct nx_surface *surface,
	const struct timespec *when);

// Includes subsurfaces, may be negative
void nx_surface_get_extents(struct nx_surface *surface, struct nx_box *box);

struct nx_surface *nx_surface_from_resource(struct wl_resource *resource);

// Mapped surfaces, root to leaves
void nx_surface_for_each_surface(struct nx_surface *surface,
	nx_surface_iterator_func_t iterator, void *user_data);

// Buffer-local sampling rectangle
void nx_surface_get_buffer_source_box(struct nx_surface *surface,
	struct nx_fbox *box);

// Locked pending states stay cached
uint32_t nx_surface_lock_pending(struct nx_surface *surface);
void nx_surface_unlock_cached(struct nx_surface *surface, uint32_t seq);

void nx_surface_set_preferred_buffer_scale(struct nx_surface *surface,
	int32_t scale);
void nx_surface_set_preferred_buffer_transform(struct nx_surface *surface,
	enum wl_output_transform transform);

struct nx_surface_synced;

struct nx_surface_synced_impl {
	size_t state_size;
	// Optional; move defaults to memcpy
	void (*init_state)(void *state);
	void (*finish_state)(void *state);
	void (*move_state)(void *dst, void *src);
	void (*commit)(struct nx_surface_synced *synced);
};

// State committed with the surface
struct nx_surface_synced {
	struct nx_surface *surface;
	const struct nx_surface_synced_impl *impl;
	struct wl_list link; // nx_surface.synced
	size_t index;
};

bool nx_surface_synced_init(struct nx_surface_synced *synced,
	struct nx_surface *surface, const struct nx_surface_synced_impl *impl,
	void *pending, void *current);
// Call before the surface dies
void nx_surface_synced_finish(struct nx_surface_synced *synced);
void *nx_surface_synced_get_state(struct nx_surface_synced *synced,
	const struct nx_surface_state *state);

const pixman_region32_t *nx_region_from_resource(struct wl_resource *resource);

struct nx_compositor *nx_compositor_create(struct wl_display *display,
	uint32_t version, struct nx_renderer *renderer);
void nx_compositor_set_renderer(struct nx_compositor *compositor,
	struct nx_renderer *renderer);

#endif

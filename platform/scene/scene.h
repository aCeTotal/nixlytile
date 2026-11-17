#ifndef PLATFORM_SCENE_SCENE_H
#define PLATFORM_SCENE_SCENE_H

#include <assert.h>
#include <pixman.h>
#include <time.h>
#include <wayland-server-core.h>
#include "platform/render/renderer.h"
#include "platform/buffer/linux_dmabuf.h"
#include "platform/container/addon.h"
#include "platform/geometry/box.h"
#include "platform/scene/output/viewport.h"

struct nx_xdg_surface;
struct nx_layer_surface_v1;
struct nx_drag_icon;
struct nx_surface;

struct nx_scene_node;
struct nx_scene_buffer;

struct nx_linux_dmabuf_v1;
struct nx_gamma_control_manager_v1;
struct nx_color_manager_v1;

// Node-local coordinates, adjustable in place.
typedef bool (*nx_scene_buffer_point_accepts_input_func_t)(
	struct nx_scene_buffer *buffer, double *sx, double *sy);

typedef void (*nx_scene_buffer_iterator_func_t)(
	struct nx_scene_buffer *buffer, int sx, int sy, void *user_data);

enum nx_scene_node_type {
	NX_SCENE_NODE_TREE,
	NX_SCENE_NODE_RECT,
	NX_SCENE_NODE_BUFFER,
};

struct nx_scene_node {
	enum nx_scene_node_type type;
	struct nx_scene_tree *parent;

	struct wl_list link; // nx_scene_tree.children

	bool enabled;
	int x, y; // relative to parent

	struct {
		struct wl_signal destroy;
	} events;

	void *data;

	struct nx_addon_set addons;

	struct {
		pixman_region32_t visible;
	};
};

enum nx_scene_debug_damage_option {
	NX_SCENE_DEBUG_DAMAGE_NONE,
	NX_SCENE_DEBUG_DAMAGE_RERENDER,
	NX_SCENE_DEBUG_DAMAGE_HIGHLIGHT
};

struct nx_scene_tree {
	struct nx_scene_node node;
	float opacity; // multiplies into every descendant

	struct wl_list children; // nx_scene_node.link
};

struct nx_scene {
	struct nx_scene_tree tree;

	struct wl_list outputs; // nx_scene_output.link

	// May be NULL
	struct nx_linux_dmabuf_v1 *linux_dmabuf_v1;
	struct nx_gamma_control_manager_v1 *gamma_control_manager_v1;
	struct nx_color_manager_v1 *color_manager_v1;

	struct {
		struct wl_listener linux_dmabuf_v1_destroy;
		struct wl_listener gamma_control_manager_v1_destroy;
		struct wl_listener gamma_control_manager_v1_set_gamma;
		struct wl_listener color_manager_v1_destroy;

		enum nx_scene_debug_damage_option debug_damage_option;
		bool direct_scanout;
		bool calculate_visibility;
		bool highlight_transparent_region;
	};
};

struct nx_scene_surface {
	struct nx_scene_buffer *buffer;
	struct nx_surface *surface;

	struct {
		struct nx_box clip;

		struct nx_addon addon;

		struct wl_listener outputs_update;
		struct wl_listener output_sample;
		struct wl_listener frame_done;
		struct wl_listener surface_destroy;
		struct wl_listener surface_commit;
	};
};

struct nx_scene_rect_shape {
	int radius; // corner radius
	int softness; // feathered edge width
	int border; // outline width, 0 fills
};

struct nx_scene_rect {
	struct nx_scene_node node;
	int width, height;
	float color[4]; // premultiplied
	struct nx_scene_rect_shape shape;
};

struct nx_scene_outputs_update_event {
	struct nx_scene_output **active;
	size_t size;
};

struct nx_scene_output_sample_event {
	struct nx_scene_output *output;
	bool direct_scanout;
	struct nx_drm_syncobj_timeline *release_timeline;
	uint64_t release_point;
};

struct nx_scene_frame_done_event {
	struct nx_scene_output *output;
	struct timespec when;
};

struct nx_scene_buffer {
	struct nx_scene_node node;

	// May be NULL
	struct nx_buffer *buffer;

	struct {
		struct wl_signal outputs_update; // struct nx_scene_outputs_update_event
		struct wl_signal output_sample; // struct nx_scene_output_sample_event
		struct wl_signal frame_done; // struct nx_scene_frame_done_event
	} events;

	// May be NULL
	nx_scene_buffer_point_accepts_input_func_t point_accepts_input;

	struct nx_scene_output *primary_output; // most visible area, or NULL

	float opacity;
	struct nx_box corner_box; // node-local rounded clip
	int corner_radius; // 0 disables
	struct nx_fbox src_box; // empty samples everything
	int dst_width, dst_height; // zero means buffer size
	enum wl_output_transform transform;
	pixman_region32_t opaque_region;
	enum nx_color_transfer_function transfer_function;
	enum nx_color_named_primaries primaries;
	enum nx_color_encoding color_encoding;
	enum nx_color_range color_range;

	struct {
		uint64_t active_outputs;
		struct nx_texture *texture;
		struct nx_linux_dmabuf_feedback_v1_init_options prev_feedback_options;

		bool own_buffer;
		int buffer_width, buffer_height;
		bool buffer_is_opaque;

		struct nx_drm_syncobj_timeline *wait_timeline;
		uint64_t wait_point;

		struct wl_listener buffer_release;
		struct wl_listener renderer_destroy;

		bool is_single_pixel_buffer;
		uint32_t single_pixel_buffer_color[4]; // RGBA, UINT32_MAX full
	};
};

struct nx_scene_layer_surface_v1 {
	struct nx_scene_tree *tree;
	struct nx_layer_surface_v1 *layer_surface;

	struct {
		struct wl_listener tree_destroy;
		struct wl_listener layer_surface_destroy;
	};
};

struct nx_scene_buffer_set_buffer_options {
	const pixman_region32_t *damage; // buffer-local, NULL damages all

	struct nx_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
};

// Destroys the whole subtree.
void nx_scene_node_destroy(struct nx_scene_node *node);
// Disabling hides the whole subtree.
void nx_scene_node_set_enabled(struct nx_scene_node *node, bool enabled);
void nx_scene_node_set_position(struct nx_scene_node *node, int x, int y);
void nx_scene_node_place_above(struct nx_scene_node *node,
	struct nx_scene_node *sibling);
void nx_scene_node_place_below(struct nx_scene_node *node,
	struct nx_scene_node *sibling);
void nx_scene_node_raise_to_top(struct nx_scene_node *node);
void nx_scene_node_lower_to_bottom(struct nx_scene_node *node);
void nx_scene_node_reparent(struct nx_scene_node *node,
	struct nx_scene_tree *new_parent);
// Layout coordinates; true if all enabled.
bool nx_scene_node_coords(struct nx_scene_node *node, int *lx, int *ly);
// Root to leaves, in rendering order.
void nx_scene_node_for_each_buffer(struct nx_scene_node *node,
	nx_scene_buffer_iterator_func_t iterator, void *user_data);
// Topmost input-accepting node, or NULL.
struct nx_scene_node *nx_scene_node_at(struct nx_scene_node *node,
	double lx, double ly, double *nx, double *ny);

struct nx_scene *nx_scene_create(void);
void nx_scene_set_linux_dmabuf_v1(struct nx_scene *scene,
	struct nx_linux_dmabuf_v1 *linux_dmabuf_v1);
void nx_scene_set_gamma_control_manager_v1(struct nx_scene *scene,
	struct nx_gamma_control_manager_v1 *gamma_control);
void nx_scene_set_color_manager_v1(struct nx_scene *scene, struct nx_color_manager_v1 *manager);

struct nx_scene_tree *nx_scene_tree_create(struct nx_scene_tree *parent);
void nx_scene_tree_set_opacity(struct nx_scene_tree *tree, float opacity);

// Sub-surfaces are not included.
struct nx_scene_surface *nx_scene_surface_create(struct nx_scene_tree *parent,
	struct nx_surface *surface);

static inline struct nx_scene_buffer *nx_scene_buffer_from_node(
		struct nx_scene_node *node) {
	assert(node->type == NX_SCENE_NODE_BUFFER);
	struct nx_scene_buffer *buffer = wl_container_of(node, buffer, node);
	return buffer;
}

static inline struct nx_scene_tree *nx_scene_tree_from_node(struct nx_scene_node *node) {
	assert(node->type == NX_SCENE_NODE_TREE);
	struct nx_scene_tree *tree = wl_container_of(node, tree, node);
	return tree;
}

static inline struct nx_scene_rect *nx_scene_rect_from_node(struct nx_scene_node *node) {
	assert(node->type == NX_SCENE_NODE_RECT);
	struct nx_scene_rect *rect = wl_container_of(node, rect, node);
	return rect;
}

// NULL unless surface-backed.
struct nx_scene_surface *nx_scene_surface_try_from_buffer(
	struct nx_scene_buffer *scene_buffer);

struct nx_scene_rect *nx_scene_rect_create(struct nx_scene_tree *parent,
	int width, int height, const float color[static 4]);
void nx_scene_rect_set_size(struct nx_scene_rect *rect, int width, int height);
void nx_scene_rect_set_color(struct nx_scene_rect *rect, const float color[static 4]);
void nx_scene_rect_set_shape(struct nx_scene_rect *rect,
	const struct nx_scene_rect_shape *shape);

// A NULL buffer hides the node.
struct nx_scene_buffer *nx_scene_buffer_create(struct nx_scene_tree *parent,
	struct nx_buffer *buffer);
void nx_scene_buffer_set_buffer(struct nx_scene_buffer *scene_buffer,
	struct nx_buffer *buffer);
void nx_scene_buffer_set_buffer_with_options(struct nx_scene_buffer *scene_buffer,
	struct nx_buffer *buffer, const struct nx_scene_buffer_set_buffer_options *options);
// Lets hidden nodes below skip rendering.
void nx_scene_buffer_set_opaque_region(struct nx_scene_buffer *scene_buffer,
	const pixman_region32_t *region);
void nx_scene_buffer_set_source_box(struct nx_scene_buffer *scene_buffer,
	const struct nx_fbox *box);
void nx_scene_buffer_set_dest_size(struct nx_scene_buffer *scene_buffer,
	int width, int height);
void nx_scene_buffer_set_transform(struct nx_scene_buffer *scene_buffer,
	enum wl_output_transform transform);
void nx_scene_buffer_set_opacity(struct nx_scene_buffer *scene_buffer,
	float opacity);
// Empty or radius 0 disables.
void nx_scene_buffer_set_corner_clip(struct nx_scene_buffer *scene_buffer,
	const struct nx_box *box, int radius);
void nx_scene_buffer_set_transfer_function(struct nx_scene_buffer *scene_buffer,
	enum nx_color_transfer_function transfer_function);
void nx_scene_buffer_set_primaries(struct nx_scene_buffer *scene_buffer,
	enum nx_color_named_primaries primaries);
void nx_scene_buffer_set_color_encoding(struct nx_scene_buffer *scene_buffer,
	enum nx_color_encoding encoding);
void nx_scene_buffer_set_color_range(struct nx_scene_buffer *scene_buffer,
	enum nx_color_range range);
// Only while visible.
void nx_scene_buffer_send_frame_done(struct nx_scene_buffer *scene_buffer,
	struct nx_scene_frame_done_event *event);

struct nx_scene_tree *nx_scene_subsurface_tree_create(
	struct nx_scene_tree *parent, struct nx_surface *surface);
// Root-surface coordinates; NULL or empty disables.
void nx_scene_subsurface_tree_set_clip(struct nx_scene_node *node,
	const struct nx_box *clip);

// Origin at the window geometry.
struct nx_scene_tree *nx_scene_xdg_surface_create(
	struct nx_scene_tree *parent, struct nx_xdg_surface *xdg_surface);

struct nx_scene_layer_surface_v1 *nx_scene_layer_surface_v1_create(
	struct nx_scene_tree *parent, struct nx_layer_surface_v1 *layer_surface);
// Shrinks usable_area by positive exclusive zones.
void nx_scene_layer_surface_v1_configure(
	struct nx_scene_layer_surface_v1 *scene_layer_surface,
	const struct nx_box *full_area, struct nx_box *usable_area);

struct nx_scene_tree *nx_scene_drag_icon_create(
	struct nx_scene_tree *parent, struct nx_drag_icon *drag_icon);

#endif

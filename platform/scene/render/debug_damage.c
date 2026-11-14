#include <stdlib.h>
#include "platform/os/clock.h"
#include "platform/scene/output/scene_output.h"
#include "platform/scene/render/frame.h"

#define HIGHLIGHT_DAMAGE_FADEOUT_MS 250

struct highlight_region {
	pixman_region32_t region;
	struct timespec when;
	struct wl_list link;
};

static void highlight_region_destroy(struct highlight_region *damage) {
	wl_list_remove(&damage->link);
	pixman_region32_fini(&damage->region);
	free(damage);
}

void scene_debug_damage_finish(struct nx_scene_output *scene_output) {
	struct highlight_region *damage, *tmp_damage;
	wl_list_for_each_safe(damage, tmp_damage, &scene_output->damage_highlight_regions, link) {
		highlight_region_destroy(damage);
	}
}

static void highlight_region_add(struct wl_list *regions,
		const pixman_region32_t *damage, const struct timespec *now) {
	if (pixman_region32_empty(damage)) {
		return;
	}

	struct highlight_region *region = calloc(1, sizeof(*region));
	if (!region) {
		return;
	}

	pixman_region32_init(&region->region);
	pixman_region32_copy(&region->region, damage);
	region->when = *now;
	wl_list_insert(regions, &region->link);
}

// Keeps fading regions damaged until gone.
static void highlight_damage_accumulate(struct nx_scene_output *scene_output,
		const struct timespec *now) {
	struct wl_list *regions = &scene_output->damage_highlight_regions;
	highlight_region_add(regions, &scene_output->damage_ring.current, now);

	pixman_region32_t acc_damage;
	pixman_region32_init(&acc_damage);
	struct highlight_region *damage, *tmp_damage;
	wl_list_for_each_safe(damage, tmp_damage, regions, link) {
		pixman_region32_subtract(&damage->region, &damage->region, &acc_damage);
		pixman_region32_union(&acc_damage, &acc_damage, &damage->region);

		struct timespec time_diff;
		timespec_sub(&time_diff, now, &damage->when);
		if (timespec_to_msec(&time_diff) >= HIGHLIGHT_DAMAGE_FADEOUT_MS ||
				pixman_region32_empty(&damage->region)) {
			highlight_region_destroy(damage);
		}
	}

	scene_output_damage(scene_output, &acc_damage);
	pixman_region32_fini(&acc_damage);
}

void scene_debug_damage_prepare(struct nx_scene_output *scene_output,
		struct timespec *now) {
	switch (scene_output->scene->debug_damage_option) {
	case NX_SCENE_DEBUG_DAMAGE_NONE:
		return;
	case NX_SCENE_DEBUG_DAMAGE_RERENDER:
		scene_output_damage_whole(scene_output);
		return;
	case NX_SCENE_DEBUG_DAMAGE_HIGHLIGHT:
		clock_gettime(CLOCK_MONOTONIC, now);
		highlight_damage_accumulate(scene_output, now);
		return;
	}
}

void scene_debug_damage_draw(const struct render_data *data,
		const struct nx_buffer *buffer) {
	if (data->output->scene->debug_damage_option != NX_SCENE_DEBUG_DAMAGE_HIGHLIGHT) {
		return;
	}

	struct highlight_region *damage;
	wl_list_for_each(damage, &data->output->damage_highlight_regions, link) {
		struct timespec time_diff;
		timespec_sub(&time_diff, &data->now, &damage->when);
		int64_t time_diff_ms = timespec_to_msec(&time_diff);
		float alpha = (float)(1.0 - (double)time_diff_ms / HIGHLIGHT_DAMAGE_FADEOUT_MS);

		nx_render_pass_add_rect(data->render_pass, &(struct nx_render_rect_options){
			.box = { .width = buffer->width, .height = buffer->height },
			.color = { .r = alpha * 0.5f, .g = 0, .b = 0, .a = alpha * 0.5f },
			.clip = &damage->region,
		});
	}
}

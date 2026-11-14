#include <assert.h>
#include <stdlib.h>
#include "platform/capture/screencopy/frame.h"
#include "platform/output/output.h"

struct screencopy_damage {
	struct wl_list link;
	struct nx_output *output;
	struct pixman_region32 damage;
	struct wl_listener output_precommit;
	struct wl_listener output_destroy;
};

static struct screencopy_damage *screencopy_damage_find(
		struct screencopy_client *client, struct nx_output *output) {
	struct screencopy_damage *damage;
	wl_list_for_each(damage, &client->damages, link) {
		if (damage->output == output) {
			return damage;
		}
	}
	return NULL;
}

static void screencopy_damage_accumulate(struct screencopy_damage *damage,
		const struct nx_output_state *state) {
	struct pixman_region32 *region = &damage->damage;
	struct nx_output *output = damage->output;

	if (state->committed & NX_OUTPUT_STATE_DAMAGE) {
		pixman_region32_union(region, region, &state->damage);
		pixman_region32_intersect_rect(region, region, 0, 0,
			output->width, output->height);
		return;
	}
	// Undamaged buffer means full damage.
	if (state->committed & NX_OUTPUT_STATE_BUFFER) {
		pixman_region32_union_rect(region, region, 0, 0,
			output->width, output->height);
	}
}

static void screencopy_damage_handle_output_precommit(
		struct wl_listener *listener, void *data) {
	struct screencopy_damage *damage =
		wl_container_of(listener, damage, output_precommit);
	const struct nx_output_event_precommit *event = data;
	screencopy_damage_accumulate(damage, event->state);
}

static void screencopy_damage_destroy(struct screencopy_damage *damage) {
	wl_list_remove(&damage->output_destroy.link);
	wl_list_remove(&damage->output_precommit.link);
	wl_list_remove(&damage->link);
	pixman_region32_fini(&damage->damage);
	free(damage);
}

static void screencopy_damage_handle_output_destroy(
		struct wl_listener *listener, void *data) {
	struct screencopy_damage *damage =
		wl_container_of(listener, damage, output_destroy);
	screencopy_damage_destroy(damage);
}

static struct screencopy_damage *screencopy_damage_create(
		struct screencopy_client *client, struct nx_output *output) {
	struct screencopy_damage *damage = calloc(1, sizeof(*damage));
	if (!damage) {
		return NULL;
	}

	damage->output = output;
	pixman_region32_init_rect(&damage->damage, 0, 0, output->width,
		output->height);
	wl_list_insert(&client->damages, &damage->link);

	wl_signal_add(&output->events.precommit, &damage->output_precommit);
	damage->output_precommit.notify =
		screencopy_damage_handle_output_precommit;

	wl_signal_add(&output->events.destroy, &damage->output_destroy);
	damage->output_destroy.notify = screencopy_damage_handle_output_destroy;

	return damage;
}

pixman_region32_t *screencopy_client_damage(struct screencopy_client *client,
		struct nx_output *output) {
	struct screencopy_damage *damage = screencopy_damage_find(client, output);
	if (damage == NULL) {
		damage = screencopy_damage_create(client, output);
	}
	return damage != NULL ? &damage->damage : NULL;
}

void screencopy_client_unref(struct screencopy_client *client) {
	assert(client->ref > 0);

	if (--client->ref != 0) {
		return;
	}

	struct screencopy_damage *damage, *tmp_damage;
	wl_list_for_each_safe(damage, tmp_damage, &client->damages, link) {
		screencopy_damage_destroy(damage);
	}

	free(client);
}

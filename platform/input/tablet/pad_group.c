#include <stdlib.h>
#include <string.h>
#include "platform/input/tablet/pad_device.h"
#include "platform/input/tablet/pad_group.h"
#include "platform/input/tablet/tablet_v2.h"

// Ring, strip or group data.
struct tablet_pad_auxiliary_user_data {
	struct nx_tablet_pad_client_v2 *pad;
	size_t index;
};

static void destroy_tablet_pad_ring_v2(struct wl_resource *resource) {
	struct tablet_pad_auxiliary_user_data *aux = wl_resource_get_user_data(resource);
	if (!aux) {
		return;
	}

	aux->pad->rings[aux->index] = NULL;
	free(aux);
	wl_resource_set_user_data(resource, NULL);
}

static void destroy_tablet_pad_strip_v2(struct wl_resource *resource) {
	struct tablet_pad_auxiliary_user_data *aux = wl_resource_get_user_data(resource);
	if (!aux) {
		return;
	}

	aux->pad->strips[aux->index] = NULL;
	free(aux);
	wl_resource_set_user_data(resource, NULL);
}

static void destroy_tablet_pad_group_v2(struct wl_resource *resource) {
	struct tablet_pad_auxiliary_user_data *aux = wl_resource_get_user_data(resource);
	if (!aux) {
		return;
	}

	aux->pad->groups[aux->index] = NULL;
	free(aux);
	wl_resource_set_user_data(resource, NULL);
}

static void handle_resource_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

// Feedback is not shown anywhere.
static void handle_set_feedback(struct wl_client *client,
		struct wl_resource *resource, const char *description,
		uint32_t serial) {
}

static const struct zwp_tablet_pad_ring_v2_interface tablet_pad_ring_impl = {
	.set_feedback = handle_set_feedback,
	.destroy = handle_resource_destroy,
};

static const struct zwp_tablet_pad_strip_v2_interface tablet_pad_strip_impl = {
	.set_feedback = handle_set_feedback,
	.destroy = handle_resource_destroy,
};

static const struct zwp_tablet_pad_group_v2_interface tablet_pad_group_impl = {
	.destroy = handle_resource_destroy,
};

static struct tablet_pad_auxiliary_user_data *aux_create(
		struct nx_tablet_pad_client_v2 *client, size_t index) {
	struct tablet_pad_auxiliary_user_data *aux = calloc(1, sizeof(*aux));
	if (!aux) {
		wl_client_post_no_memory(client->client);
		return NULL;
	}
	aux->pad = client;
	aux->index = index;
	return aux;
}

static bool add_strip(struct nx_tablet_pad_client_v2 *client,
		struct wl_resource *group, size_t strip) {
	struct tablet_pad_auxiliary_user_data *aux = aux_create(client, strip);
	if (!aux) {
		return false;
	}
	client->strips[strip] = wl_resource_create(client->client,
		&zwp_tablet_pad_strip_v2_interface, wl_resource_get_version(group), 0);
	if (!client->strips[strip]) {
		free(aux);
		wl_client_post_no_memory(client->client);
		return false;
	}
	wl_resource_set_implementation(client->strips[strip],
		&tablet_pad_strip_impl, aux, destroy_tablet_pad_strip_v2);
	zwp_tablet_pad_group_v2_send_strip(group, client->strips[strip]);
	return true;
}

static bool add_ring(struct nx_tablet_pad_client_v2 *client,
		struct wl_resource *group, size_t ring) {
	struct tablet_pad_auxiliary_user_data *aux = aux_create(client, ring);
	if (!aux) {
		return false;
	}
	client->rings[ring] = wl_resource_create(client->client,
		&zwp_tablet_pad_ring_v2_interface, wl_resource_get_version(group), 0);
	if (!client->rings[ring]) {
		free(aux);
		wl_client_post_no_memory(client->client);
		return false;
	}
	wl_resource_set_implementation(client->rings[ring],
		&tablet_pad_ring_impl, aux, destroy_tablet_pad_ring_v2);
	zwp_tablet_pad_group_v2_send_ring(group, client->rings[ring]);
	return true;
}

static void send_group_buttons(struct wl_resource *resource,
		struct nx_tablet_pad_group *group) {
	struct wl_array button_array;
	wl_array_init(&button_array);
	wl_array_add(&button_array, group->button_count * sizeof(int));
	memcpy(button_array.data, group->buttons, group->button_count * sizeof(int));
	zwp_tablet_pad_group_v2_send_buttons(resource, &button_array);
	wl_array_release(&button_array);
}

void input_tablet_pad_add_group(struct nx_tablet_pad_client_v2 *client,
		struct nx_tablet_pad_group *group, size_t index) {
	struct wl_resource *resource = wl_resource_create(client->client,
		&zwp_tablet_pad_group_v2_interface,
		wl_resource_get_version(client->resource), 0);
	client->groups[index] = resource;
	if (!resource) {
		wl_client_post_no_memory(client->client);
		return;
	}
	struct tablet_pad_auxiliary_user_data *aux = aux_create(client, index);
	if (!aux) {
		return;
	}
	wl_resource_set_implementation(resource, &tablet_pad_group_impl,
		aux, destroy_tablet_pad_group_v2);

	zwp_tablet_pad_v2_send_group(client->resource, resource);
	zwp_tablet_pad_group_v2_send_modes(resource, group->mode_count);
	send_group_buttons(resource, group);

	client->strip_count = group->strip_count;
	for (size_t i = 0; i < group->strip_count; ++i) {
		if (!add_strip(client, resource, group->strips[i])) {
			return;
		}
	}

	client->ring_count = group->ring_count;
	for (size_t i = 0; i < group->ring_count; ++i) {
		if (!add_ring(client, resource, group->rings[i])) {
			return;
		}
	}

	zwp_tablet_pad_group_v2_send_done(resource);
}

void input_tablet_pad_destroy_groups(struct nx_tablet_pad_client_v2 *client) {
	for (size_t i = 0; i < client->group_count; ++i) {
		if (client->groups[i]) {
			destroy_tablet_pad_group_v2(client->groups[i]);
		}
	}
	free(client->groups);

	for (size_t i = 0; i < client->ring_count; ++i) {
		if (client->rings[i]) {
			destroy_tablet_pad_ring_v2(client->rings[i]);
		}
	}
	free(client->rings);

	for (size_t i = 0; i < client->strip_count; ++i) {
		if (client->strips[i]) {
			destroy_tablet_pad_strip_v2(client->strips[i]);
		}
	}
	free(client->strips);
}

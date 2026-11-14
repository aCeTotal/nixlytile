#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "platform/log/log.h"
#include "platform/protocol/global.h"
#include "platform/output/protocol/wl_output.h"

#define OUTPUT_VERSION 4

static void send_geometry(struct wl_resource *resource) {
	struct nx_output *output = nx_output_from_resource(resource);
	wl_output_send_geometry(resource, 0, 0,
		output->phys_width, output->phys_height, output->subpixel,
		"Unknown", "Unknown", output->transform);
}

static void send_current_mode(struct wl_resource *resource) {
	struct nx_output *output = nx_output_from_resource(resource);
	struct nx_output_mode *mode = output->current_mode;
	if (mode == NULL) {
		wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT, output->width,
			output->height, output->refresh);
		return;
	}
	wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT,
		mode->width, mode->height, mode->refresh);
}

static void send_scale(struct wl_resource *resource) {
	struct nx_output *output = nx_output_from_resource(resource);
	if (wl_resource_get_version(resource) >= WL_OUTPUT_SCALE_SINCE_VERSION) {
		wl_output_send_scale(resource, (uint32_t)ceil(output->scale));
	}
}

static void send_name(struct wl_resource *resource) {
	struct nx_output *output = nx_output_from_resource(resource);
	if (wl_resource_get_version(resource) >= WL_OUTPUT_NAME_SINCE_VERSION) {
		wl_output_send_name(resource, output->name);
	}
}

static void send_description(struct wl_resource *resource) {
	struct nx_output *output = nx_output_from_resource(resource);
	if (output->description != NULL &&
			wl_resource_get_version(resource) >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION) {
		wl_output_send_description(resource, output->description);
	}
}

static void send_done(struct wl_resource *resource) {
	if (wl_resource_get_version(resource) >= WL_OUTPUT_DONE_SINCE_VERSION) {
		wl_output_send_done(resource);
	}
}

void output_global_send_changes(struct nx_output *output, uint32_t changes) {
	struct wl_resource *resource;
	wl_resource_for_each(resource, &output->resources) {
		if (changes & OUTPUT_GLOBAL_MODE) {
			send_current_mode(resource);
		}
		if (changes & OUTPUT_GLOBAL_GEOMETRY) {
			send_geometry(resource);
		}
		if (changes & OUTPUT_GLOBAL_SCALE) {
			send_scale(resource);
		}
	}
	nx_output_schedule_done(output);
}

static void output_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void output_handle_release(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_output_interface output_impl = {
	.release = output_handle_release,
};

static void output_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	// NULL during global destruction.
	struct nx_output *output = data;

	struct wl_resource *resource = wl_resource_create(wl_client,
		&wl_output_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &output_impl, output,
		output_handle_resource_destroy);

	if (output == NULL) {
		wl_list_init(wl_resource_get_link(resource));
		return;
	}

	wl_list_insert(&output->resources, wl_resource_get_link(resource));

	send_geometry(resource);
	send_current_mode(resource);
	send_scale(resource);
	send_name(resource);
	send_description(resource);
	send_done(resource);

	struct nx_output_event_bind event = {
		.resource = resource,
	};
	wl_signal_emit_mutable(&output->events.bind, &event);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_output *output = wl_container_of(listener, output, display_destroy);
	nx_output_destroy_global(output);
}

void nx_output_create_global(struct nx_output *output, struct wl_display *display) {
	if (output->global != NULL) {
		return;
	}

	output->global = wl_global_create(display,
		&wl_output_interface, OUTPUT_VERSION, output, output_bind);
	if (output->global == NULL) {
		nx_log(NX_ERROR, "Failed to allocate wl_output global");
		return;
	}

	wl_list_remove(&output->display_destroy.link);
	output->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &output->display_destroy);
}

void nx_output_destroy_global(struct nx_output *output) {
	if (output->global == NULL) {
		return;
	}

	// Make all resources inert.
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &output->resources) {
		wl_resource_set_user_data(resource, NULL);
		wl_list_remove(wl_resource_get_link(resource));
		wl_list_init(wl_resource_get_link(resource));
	}

	wl_list_remove(&output->display_destroy.link);
	wl_list_init(&output->display_destroy.link);

	nx_global_destroy_safe(output->global);
	output->global = NULL;
}

static void schedule_done_handle_idle_timer(void *data) {
	struct nx_output *output = data;
	output->idle_done = NULL;

	struct wl_resource *resource;
	wl_resource_for_each(resource, &output->resources) {
		send_done(resource);
	}
}

void nx_output_schedule_done(struct nx_output *output) {
	if (output->idle_done != NULL) {
		return;
	}
	output->idle_done = wl_event_loop_add_idle(output->event_loop,
		schedule_done_handle_idle_timer, output);
}

struct nx_output *nx_output_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_output_interface,
		&output_impl));
	return wl_resource_get_user_data(resource);
}

void nx_output_set_name(struct nx_output *output, const char *name) {
	assert(output->global == NULL);

	free(output->name);
	output->name = strdup(name);
}

void nx_output_set_description(struct nx_output *output, const char *desc) {
	if (output->description != NULL && desc != NULL &&
			strcmp(output->description, desc) == 0) {
		return;
	}

	free(output->description);
	output->description = desc != NULL ? strdup(desc) : NULL;

	struct wl_resource *resource;
	wl_resource_for_each(resource, &output->resources) {
		send_description(resource);
	}
	nx_output_schedule_done(output);

	wl_signal_emit_mutable(&output->events.description, output);
}

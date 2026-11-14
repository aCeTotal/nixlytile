#ifndef PLATFORM_OUTPUT_MANAGEMENT_CONFIGURATION_H
#define PLATFORM_OUTPUT_MANAGEMENT_CONFIGURATION_H

#include "platform/output/management.h"

struct nx_output_configuration_head_v1 *output_config_head_create(
	struct nx_output_configuration_v1 *config, struct nx_output *output);
void output_config_head_destroy(struct nx_output_configuration_head_v1 *config_head);
// Creates the client object of an enabled head.
void output_config_head_create_resource(
	struct nx_output_configuration_head_v1 *config_head,
	struct wl_resource *config_resource, uint32_t id);

// Configuration requested by a client.
struct nx_output_configuration_v1 *output_config_create_for_client(
	struct wl_resource *manager_resource, uint32_t id);

#endif

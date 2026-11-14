#ifndef PLATFORM_INPUT_TABLET_PAD_GROUP_H
#define PLATFORM_INPUT_TABLET_PAD_GROUP_H

#include <stddef.h>

struct nx_tablet_pad_client_v2;
struct nx_tablet_pad_group;

void input_tablet_pad_add_group(struct nx_tablet_pad_client_v2 *client,
	struct nx_tablet_pad_group *group, size_t index);
void input_tablet_pad_destroy_groups(struct nx_tablet_pad_client_v2 *client);

#endif

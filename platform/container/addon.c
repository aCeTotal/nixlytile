#include <assert.h>
#include <stdlib.h>
#include "platform/container/addon.h"
#include "platform/log/log.h"

void nx_addon_set_init(struct nx_addon_set *set) {
	*set = (struct nx_addon_set){0};
	wl_list_init(&set->addons);
}

void nx_addon_set_finish(struct nx_addon_set *set) {
	while (!wl_list_empty(&set->addons)) {
		struct wl_list *link = set->addons.next;
		struct nx_addon *addon = wl_container_of(link, addon, link);
		const struct nx_addon_interface *impl = addon->impl;
		addon->impl->destroy(addon);
		if (set->addons.next == link) {
			nx_log(NX_ERROR, "Dangling addon: %s", impl->name);
			abort();
		}
	}
}

void nx_addon_init(struct nx_addon *addon, struct nx_addon_set *set,
		const void *owner, const struct nx_addon_interface *impl) {
	assert(impl);
	assert(nx_addon_find(set, owner, impl) == NULL);
	*addon = (struct nx_addon){
		.impl = impl,
		.owner = owner,
	};
	wl_list_insert(&set->addons, &addon->link);
}

void nx_addon_finish(struct nx_addon *addon) {
	wl_list_remove(&addon->link);
}

struct nx_addon *nx_addon_find(struct nx_addon_set *set, const void *owner,
		const struct nx_addon_interface *impl) {
	struct nx_addon *addon;
	wl_list_for_each(addon, &set->addons, link) {
		if (addon->owner == owner && addon->impl == impl) {
			return addon;
		}
	}
	return NULL;
}

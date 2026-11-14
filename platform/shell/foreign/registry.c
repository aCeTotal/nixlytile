#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "platform/protocol/token.h"
#include "platform/shell/foreign/registry.h"

bool nx_xdg_foreign_exported_init(struct nx_xdg_foreign_exported *exported,
		struct nx_xdg_foreign_registry *registry) {
	do {
		if (!generate_token(exported->handle)) {
			return false;
		}
	} while (nx_xdg_foreign_registry_find_by_handle(registry, exported->handle) != NULL);

	wl_list_insert(&registry->exported_surfaces, &exported->link);

	wl_signal_init(&exported->events.destroy);

	return true;
}

struct nx_xdg_foreign_exported *nx_xdg_foreign_registry_find_by_handle(
		struct nx_xdg_foreign_registry *registry, const char *handle) {
	if (handle == NULL || strlen(handle) >= NX_XDG_FOREIGN_HANDLE_SIZE) {
		return NULL;
	}

	struct nx_xdg_foreign_exported *exported;
	wl_list_for_each(exported, &registry->exported_surfaces, link) {
		if (strcmp(handle, exported->handle) == 0) {
			return exported;
		}
	}

	return NULL;
}

void nx_xdg_foreign_exported_finish(struct nx_xdg_foreign_exported *exported) {
	wl_signal_emit_mutable(&exported->events.destroy, NULL);

	assert(wl_list_empty(&exported->events.destroy.listener_list));

	wl_list_remove(&exported->link);
	wl_list_init(&exported->link);
}

static void registry_handle_display_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_xdg_foreign_registry *registry =
		wl_container_of(listener, registry, display_destroy);

	wl_signal_emit_mutable(&registry->events.destroy, NULL);

	assert(wl_list_empty(&registry->events.destroy.listener_list));
	// Exporters remove their surfaces first.
	assert(wl_list_empty(&registry->exported_surfaces));
	free(registry);
}

struct nx_xdg_foreign_registry *nx_xdg_foreign_registry_create(
		struct wl_display *display) {
	struct nx_xdg_foreign_registry *registry = calloc(1, sizeof(*registry));
	if (!registry) {
		return NULL;
	}

	registry->display_destroy.notify = registry_handle_display_destroy;
	wl_display_add_destroy_listener(display, &registry->display_destroy);

	wl_list_init(&registry->exported_surfaces);

	wl_signal_init(&registry->events.destroy);

	return registry;
}

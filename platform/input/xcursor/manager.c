#include <stdlib.h>
#include <string.h>
#include "platform/input/xcursor/manager.h"

struct nx_xcursor_manager *nx_xcursor_manager_create(const char *name,
		uint32_t size) {
	struct nx_xcursor_manager *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	if (name != NULL) {
		manager->name = strdup(name);
	}
	manager->size = size;
	wl_list_init(&manager->scaled_themes);
	return manager;
}

void nx_xcursor_manager_destroy(struct nx_xcursor_manager *manager) {
	if (manager == NULL) {
		return;
	}
	struct nx_xcursor_manager_theme *theme, *tmp;
	wl_list_for_each_safe(theme, tmp, &manager->scaled_themes, link) {
		wl_list_remove(&theme->link);
		nx_xcursor_theme_destroy(theme->theme);
		free(theme);
	}
	free(manager->name);
	free(manager);
}

static struct nx_xcursor_manager_theme *find_theme(
		struct nx_xcursor_manager *manager, float scale) {
	struct nx_xcursor_manager_theme *theme;
	wl_list_for_each(theme, &manager->scaled_themes, link) {
		if (theme->scale == scale) {
			return theme;
		}
	}
	return NULL;
}

bool nx_xcursor_manager_load(struct nx_xcursor_manager *manager, float scale) {
	if (find_theme(manager, scale) != NULL) {
		return true;
	}

	struct nx_xcursor_manager_theme *theme = calloc(1, sizeof(*theme));
	if (theme == NULL) {
		return false;
	}
	theme->scale = scale;
	theme->theme = nx_xcursor_theme_load(manager->name,
		(int)(manager->size * scale));
	if (theme->theme == NULL) {
		free(theme);
		return false;
	}
	wl_list_insert(&manager->scaled_themes, &theme->link);
	return true;
}

struct nx_xcursor *nx_xcursor_manager_get_xcursor(
		struct nx_xcursor_manager *manager, const char *name, float scale) {
	struct nx_xcursor_manager_theme *theme = find_theme(manager, scale);
	if (theme == NULL) {
		return NULL;
	}
	return nx_xcursor_theme_get_cursor(theme->theme, name);
}

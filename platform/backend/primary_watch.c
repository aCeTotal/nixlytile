#include <stdlib.h>
#include "platform/backend/primary_watch.h"

struct primary_watch {
	struct nx_backend *multi;
	struct wl_listener multi_destroy;
	struct wl_listener primary_destroy;
};

static void handle_multi_destroy(struct wl_listener *listener, void *data) {
	struct primary_watch *watch = wl_container_of(listener, watch, multi_destroy);
	wl_list_remove(&watch->multi_destroy.link);
	wl_list_remove(&watch->primary_destroy.link);
	free(watch);
}

static void handle_primary_destroy(struct wl_listener *listener, void *data) {
	struct primary_watch *watch = wl_container_of(listener, watch, primary_destroy);
	nx_backend_destroy(watch->multi);
}

bool backend_watch_primary(struct nx_backend *multi, struct nx_backend *primary) {
	struct primary_watch *watch = calloc(1, sizeof(*watch));
	if (watch == NULL) {
		return false;
	}
	watch->multi = multi;
	watch->multi_destroy.notify = handle_multi_destroy;
	wl_signal_add(&multi->events.destroy, &watch->multi_destroy);
	watch->primary_destroy.notify = handle_primary_destroy;
	wl_signal_add(&primary->events.destroy, &watch->primary_destroy);
	return true;
}

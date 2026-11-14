#include <assert.h>
#include <stdlib.h>
#include "platform/shell/window/xdg_dialog.h"
#include "platform/shell/xdg/xdg_shell.h"
#include "xdg-dialog-v1-protocol.h"

#define XDG_WM_DIALOG_V1_VERSION 1

static const struct xdg_dialog_v1_interface dialog_impl;

static struct nx_xdg_dialog_v1 *dialog_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &xdg_dialog_v1_interface, &dialog_impl));
	return wl_resource_get_user_data(resource);
}

static void resource_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

// The toplevel always dies first.
static void surface_addon_destroy(struct nx_addon *addon) {
	abort();
}

static const struct nx_addon_interface surface_addon_impl = {
	.name = "xdg_wm_dialog_v1",
	.destroy = surface_addon_destroy,
};

static void dialog_handle_set_modal(struct wl_client *client, struct wl_resource *resource) {
}

static void dialog_handle_unset_modal(struct wl_client *client, struct wl_resource *resource) {
}

static const struct xdg_dialog_v1_interface dialog_impl = {
	.destroy = resource_destroy,
	.set_modal = dialog_handle_set_modal,
	.unset_modal = dialog_handle_unset_modal,
};

static void dialog_destroy(struct nx_xdg_dialog_v1 *dialog) {
	if (dialog == NULL) {
		return;
	}

	nx_addon_finish(&dialog->surface_addon);
	wl_list_remove(&dialog->xdg_toplevel_destroy.link);

	wl_resource_set_user_data(dialog->resource, NULL);
	free(dialog);
}

static void handle_xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct nx_xdg_dialog_v1 *dialog = wl_container_of(listener, dialog, xdg_toplevel_destroy);
	dialog_destroy(dialog);
}

static void handle_resource_destroy(struct wl_resource *resource) {
	dialog_destroy(dialog_from_resource(resource));
}

static void wm_get_xdg_dialog(struct wl_client *client, struct wl_resource *wm_resource,
		uint32_t id, struct wl_resource *toplevel_resource) {
	struct nx_xdg_toplevel *xdg_toplevel = nx_xdg_toplevel_from_resource(toplevel_resource);
	struct nx_addon_set *addon_set = &xdg_toplevel->base->surface->addons;

	if (nx_addon_find(addon_set, NULL, &surface_addon_impl) != NULL) {
		wl_resource_post_error(wm_resource, XDG_WM_DIALOG_V1_ERROR_ALREADY_USED,
			"the xdg_toplevel object has already been used to create a xdg_dialog_v1");
		return;
	}

	struct nx_xdg_dialog_v1 *dialog = calloc(1, sizeof(*dialog));
	if (dialog == NULL) {
		wl_resource_post_no_memory(wm_resource);
		return;
	}

	dialog->resource = wl_resource_create(client, &xdg_dialog_v1_interface,
		wl_resource_get_version(wm_resource), id);
	if (dialog->resource == NULL) {
		free(dialog);
		wl_resource_post_no_memory(wm_resource);
		return;
	}
	wl_resource_set_implementation(dialog->resource, &dialog_impl,
		dialog, handle_resource_destroy);

	nx_addon_init(&dialog->surface_addon, addon_set, NULL, &surface_addon_impl);

	dialog->xdg_toplevel_destroy.notify = handle_xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &dialog->xdg_toplevel_destroy);
}

static const struct xdg_wm_dialog_v1_interface wm_impl = {
	.destroy = resource_destroy,
	.get_xdg_dialog = wm_get_xdg_dialog,
};

static void wm_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &xdg_wm_dialog_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &wm_impl, NULL, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_xdg_wm_dialog_v1 *wm = wl_container_of(listener, wm, display_destroy);
	wl_list_remove(&wm->display_destroy.link);
	wl_global_destroy(wm->global);
	free(wm);
}

struct nx_xdg_dialog_v1 *nx_xdg_dialog_v1_try_from_nx_xdg_toplevel(
		struct nx_xdg_toplevel *xdg_toplevel) {
	struct nx_addon *addon =
		nx_addon_find(&xdg_toplevel->base->surface->addons, NULL, &surface_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct nx_xdg_dialog_v1 *dialog = wl_container_of(addon, dialog, surface_addon);
	return dialog;
}

struct nx_xdg_wm_dialog_v1 *nx_xdg_wm_dialog_v1_create(struct wl_display *display,
		uint32_t version) {
	assert(version <= XDG_WM_DIALOG_V1_VERSION);

	struct nx_xdg_wm_dialog_v1 *wm = calloc(1, sizeof(*wm));
	if (wm == NULL) {
		return NULL;
	}

	wm->global = wl_global_create(display, &xdg_wm_dialog_v1_interface, version, wm, wm_bind);
	if (wm->global == NULL) {
		free(wm);
		return NULL;
	}

	wm->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &wm->display_destroy);

	return wm;
}

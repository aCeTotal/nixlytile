#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/clipboard/primary/primary_selection.h"
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

#define MAX_TARGETS_LENGTH 4096 // in 32-bit units

struct x11_data_source {
	struct nx_data_source base;
	struct nx_xwm_selection *selection;
	struct wl_array mime_types_atoms;
};

struct x11_primary_selection_source {
	struct nx_primary_selection_source base;
	struct nx_xwm_selection *selection;
	struct wl_array mime_types_atoms;
};

// atoms runs parallel to mime_types.
static void source_send(struct nx_xwm_selection *selection,
		struct wl_array *mime_types, struct wl_array *atoms,
		const char *requested_mime_type, int fd) {
	const xcb_atom_t *targets = atoms->data;
	size_t i = 0;
	char **mime_type;
	wl_array_for_each(mime_type, mime_types) {
		if (strcmp(*mime_type, requested_mime_type) == 0) {
			xwm_selection_receive(selection, targets[i], fd);
			return;
		}
		i++;
	}

	nx_log(NX_DEBUG, "Cannot send X11 selection to Wayland: "
		"unsupported MIME type");
	close(fd);
}

static const struct nx_data_source_impl data_source_impl;

bool xwm_data_source_is_xwayland(struct nx_data_source *source) {
	return source->impl == &data_source_impl;
}

static struct x11_data_source *data_source_from_nx(struct nx_data_source *base) {
	assert(xwm_data_source_is_xwayland(base));
	struct x11_data_source *source = wl_container_of(base, source, base);
	return source;
}

static void data_source_send(struct nx_data_source *base,
		const char *mime_type, int32_t fd) {
	struct x11_data_source *source = data_source_from_nx(base);
	source_send(source->selection, &base->mime_types,
		&source->mime_types_atoms, mime_type, fd);
}

static void data_source_destroy(struct nx_data_source *base) {
	struct x11_data_source *source = data_source_from_nx(base);
	wl_array_release(&source->mime_types_atoms);
	free(source);
}

static const struct nx_data_source_impl data_source_impl = {
	.send = data_source_send,
	.destroy = data_source_destroy,
};

static const struct nx_primary_selection_source_impl primary_source_impl;

bool xwm_primary_selection_source_is_xwayland(
		struct nx_primary_selection_source *source) {
	return source->impl == &primary_source_impl;
}

static void primary_source_send(struct nx_primary_selection_source *base,
		const char *mime_type, int fd) {
	struct x11_primary_selection_source *source =
		wl_container_of(base, source, base);
	source_send(source->selection, &base->mime_types,
		&source->mime_types_atoms, mime_type, fd);
}

static void primary_source_destroy(struct nx_primary_selection_source *base) {
	struct x11_primary_selection_source *source =
		wl_container_of(base, source, base);
	wl_array_release(&source->mime_types_atoms);
	free(source);
}

static const struct nx_primary_selection_source_impl primary_source_impl = {
	.send = primary_source_send,
	.destroy = primary_source_destroy,
};

// Keeps only MIME-like atom names.
static char *target_mime_type(struct nx_xwm *xwm, xcb_atom_t target) {
	if (target == xwm->atoms[TARGETS] || target == xwm->atoms[TIMESTAMP]) {
		return NULL;
	}
	char *mime_type = xwm_mime_type_from_atom(xwm, target);
	if (mime_type != NULL && strchr(mime_type, '/') == NULL) {
		free(mime_type);
		return NULL;
	}
	return mime_type;
}

static bool add_target(struct wl_array *mime_types, struct wl_array *atoms,
		char *mime_type, xcb_atom_t target) {
	char **mime_type_ptr = wl_array_add(mime_types, sizeof(*mime_type_ptr));
	if (mime_type_ptr == NULL) {
		return false;
	}
	xcb_atom_t *atom_ptr = wl_array_add(atoms, sizeof(*atom_ptr));
	if (atom_ptr == NULL) {
		mime_types->size -= sizeof(*mime_type_ptr);
		return false;
	}
	*mime_type_ptr = mime_type;
	*atom_ptr = target;
	return true;
}

// TARGETS reply into parallel arrays.
static bool read_targets(struct nx_xwm_selection *selection,
		struct wl_array *mime_types, struct wl_array *atoms) {
	struct nx_xwm *xwm = selection->xwm;
	xcb_get_property_cookie_t cookie = xcb_get_property(xwm->xcb_conn, 1,
		selection->window, xwm->atoms[WL_SELECTION],
		XCB_GET_PROPERTY_TYPE_ANY, 0, MAX_TARGETS_LENGTH);
	xcb_get_property_reply_t *reply =
		xcb_get_property_reply(xwm->xcb_conn, cookie, NULL);
	if (reply == NULL) {
		return false;
	}
	if (reply->type != XCB_ATOM_ATOM) {
		free(reply);
		return false;
	}

	const xcb_atom_t *targets = xcb_get_property_value(reply);
	size_t count = xcb_get_property_value_length(reply) / sizeof(targets[0]);
	for (size_t i = 0; i < count; i++) {
		char *mime_type = target_mime_type(xwm, targets[i]);
		if (mime_type == NULL) {
			continue;
		}
		if (!add_target(mime_types, atoms, mime_type, targets[i])) {
			free(mime_type);
			break;
		}
	}

	free(reply);
	return true;
}

static void import_clipboard(struct nx_xwm_selection *selection) {
	struct nx_xwm *xwm = selection->xwm;
	struct x11_data_source *source = calloc(1, sizeof(*source));
	if (source == NULL) {
		return;
	}
	nx_data_source_init(&source->base, &data_source_impl);
	source->selection = selection;
	wl_array_init(&source->mime_types_atoms);

	if (!read_targets(selection, &source->base.mime_types,
			&source->mime_types_atoms)) {
		nx_data_source_destroy(&source->base);
		return;
	}
	nx_seat_request_set_selection(xwm->seat, NULL, &source->base,
		wl_display_next_serial(xwm->xwayland->wl_display));
}

static void import_primary(struct nx_xwm_selection *selection) {
	struct nx_xwm *xwm = selection->xwm;
	struct x11_primary_selection_source *source = calloc(1, sizeof(*source));
	if (source == NULL) {
		return;
	}
	nx_primary_selection_source_init(&source->base, &primary_source_impl);
	source->selection = selection;
	wl_array_init(&source->mime_types_atoms);

	if (!read_targets(selection, &source->base.mime_types,
			&source->mime_types_atoms)) {
		nx_primary_selection_source_destroy(&source->base);
		return;
	}
	nx_seat_set_primary_selection(xwm->seat, &source->base,
		wl_display_next_serial(xwm->xwayland->wl_display));
}

// Offers X11 TARGETS to Wayland.
void xwm_selection_get_targets(struct nx_xwm_selection *selection) {
	struct nx_xwm *xwm = selection->xwm;
	if (selection == &xwm->clipboard_selection) {
		import_clipboard(selection);
	} else if (selection == &xwm->primary_selection) {
		import_primary(selection);
	}
}

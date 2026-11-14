#include <assert.h>
#include <stdlib.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/log/log.h"
#include "platform/xwayland/wm/xwm.h"

#define INLINE_TYPES 3 // types fitting in XdndEnter

static xcb_atom_t action_to_atom(struct nx_xwm *xwm,
		enum wl_data_device_manager_dnd_action action) {
	if (action & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) {
		return xwm->atoms[DND_ACTION_COPY];
	}
	if (action & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE) {
		return xwm->atoms[DND_ACTION_MOVE];
	}
	if (action & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) {
		return xwm->atoms[DND_ACTION_ASK];
	}
	return XCB_ATOM_NONE;
}

static void send_event(struct nx_xwm *xwm, xcb_atom_t type,
		const xcb_client_message_data_t *data) {
	struct nx_xwayland_surface *dest = xwm->drag_focus;
	assert(dest != NULL);

	xcb_client_message_event_t event = {
		.response_type = XCB_CLIENT_MESSAGE,
		.format = 32,
		.window = dest->window_id,
		.type = type,
		.data = *data,
	};
	xwm_send_event(xwm, dest->window_id, XCB_EVENT_MASK_NO_EVENT,
		&event, sizeof(event));
	xwm_schedule_flush(xwm);
}

static bool publish_type_list(struct nx_xwm *xwm, struct wl_array *mime_types,
		size_t count) {
	xcb_atom_t *targets = malloc(count * sizeof(targets[0]));
	if (targets == NULL) {
		nx_log(NX_ERROR, "Allocation failed");
		return false;
	}

	size_t i = 0;
	char **mime_type;
	wl_array_for_each(mime_type, mime_types) {
		targets[i++] = xwm_mime_type_to_atom(xwm, *mime_type);
	}

	xcb_change_property(xwm->xcb_conn, XCB_PROP_MODE_REPLACE,
		xwm->dnd_selection.window, xwm->atoms[DND_TYPE_LIST],
		XCB_ATOM_ATOM, 32, count, targets);
	free(targets);
	return true;
}

// Inline types, else XdndTypeList.
static bool describe_types(struct nx_xwm *xwm, struct wl_array *mime_types,
		xcb_client_message_data_t *data) {
	size_t count = mime_types->size / sizeof(char *);
	if (count > INLINE_TYPES) {
		data->data32[1] |= 1;
		return publish_type_list(xwm, mime_types, count);
	}

	size_t i = 2;
	char **mime_type;
	wl_array_for_each(mime_type, mime_types) {
		data->data32[i++] = xwm_mime_type_to_atom(xwm, *mime_type);
	}
	return true;
}

void xwm_dnd_send_enter(struct nx_xwm *xwm) {
	struct nx_drag *drag = xwm->drag;
	assert(drag != NULL);

	xcb_client_message_data_t data = {
		.data32 = { xwm->dnd_selection.window, XDND_VERSION << 24 },
	};
	if (!describe_types(xwm, &drag->source->mime_types, &data)) {
		return;
	}
	send_event(xwm, xwm->atoms[DND_ENTER], &data);
}

void xwm_dnd_send_position(struct nx_xwm *xwm, uint32_t time, int16_t x,
		int16_t y) {
	struct nx_drag *drag = xwm->drag;
	assert(drag != NULL);

	xcb_client_message_data_t data = { 0 };
	data.data32[0] = xwm->dnd_selection.window;
	data.data32[2] = (x << 16) | y;
	data.data32[3] = time;
	data.data32[4] = action_to_atom(xwm, drag->source->actions);
	send_event(xwm, xwm->atoms[DND_POSITION], &data);
}

void xwm_dnd_send_drop(struct nx_xwm *xwm, uint32_t time) {
	assert(xwm->drag != NULL);
	assert(xwm->drag_focus != NULL);

	xcb_client_message_data_t data = {
		.data32 = { xwm->dnd_selection.window, 0, time },
	};
	send_event(xwm, xwm->atoms[DND_DROP], &data);
}

void xwm_dnd_send_leave(struct nx_xwm *xwm) {
	assert(xwm->drag != NULL);
	assert(xwm->drag_focus != NULL);

	xcb_client_message_data_t data = {
		.data32 = { xwm->dnd_selection.window },
	};
	send_event(xwm, xwm->atoms[DND_LEAVE], &data);
}

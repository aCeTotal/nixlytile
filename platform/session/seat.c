#include <libseat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/log/log.h"
#include "platform/session/seat.h"
#include "platform/session/session.h"

static void handle_enable_seat(struct libseat *seat, void *data) {
	struct nx_session *session = data;
	session->active = true;
	wl_signal_emit_mutable(&session->events.active, NULL);
}

static void handle_disable_seat(struct libseat *seat, void *data) {
	struct nx_session *session = data;
	session->active = false;
	wl_signal_emit_mutable(&session->events.active, NULL);
	libseat_disable_seat(session->seat_handle);
}

static int libseat_event(int fd, uint32_t mask, void *data) {
	struct nx_session *session = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		nx_log(mask & WL_EVENT_ERROR ? NX_ERROR : NX_INFO,
			"Failed to wait for libseat event");
		nx_session_destroy(session);
		return 0;
	}
	if (libseat_dispatch(session->seat_handle, 0) == -1) {
		nx_log_errno(NX_ERROR, "Failed to dispatch libseat");
		nx_session_destroy(session);
	}
	return 1;
}

static struct libseat_seat_listener seat_listener = {
	.enable_seat = handle_enable_seat,
	.disable_seat = handle_disable_seat,
};

static enum nx_log_importance libseat_log_level_to_nx(
		enum libseat_log_level level) {
	switch (level) {
	case LIBSEAT_LOG_LEVEL_ERROR:
		return NX_ERROR;
	case LIBSEAT_LOG_LEVEL_INFO:
		return NX_INFO;
	default:
		return NX_DEBUG;
	}
}

static void log_libseat(enum libseat_log_level level,
		const char *fmt, va_list args) {
	enum nx_log_importance importance = libseat_log_level_to_nx(level);

	static char nx_fmt[1024];
	snprintf(nx_fmt, sizeof(nx_fmt), "[libseat] %s", fmt);

	_nx_vlog(importance, nx_fmt, args);
}

bool session_seat_init(struct nx_session *session) {
	libseat_set_log_handler(log_libseat);
	libseat_set_log_level(LIBSEAT_LOG_LEVEL_INFO);

	// libseat updates logind from this
	setenv("XDG_SESSION_TYPE", "wayland", 1);

	session->seat_handle = libseat_open_seat(&seat_listener, session);
	if (session->seat_handle == NULL) {
		nx_log_errno(NX_ERROR, "Unable to create seat");
		return false;
	}

	const char *seat_name = libseat_seat_name(session->seat_handle);
	if (seat_name == NULL) {
		nx_log_errno(NX_ERROR, "Unable to get seat info");
		goto error;
	}
	snprintf(session->seat, sizeof(session->seat), "%s", seat_name);

	session->libseat_event = wl_event_loop_add_fd(session->event_loop,
		libseat_get_fd(session->seat_handle), WL_EVENT_READABLE,
		libseat_event, session);
	if (session->libseat_event == NULL) {
		nx_log(NX_ERROR, "Failed to create libseat event source");
		goto error;
	}

	// enable_seat may already be queued
	if (libseat_dispatch(session->seat_handle, 0) == -1) {
		nx_log_errno(NX_ERROR, "libseat dispatch failed");
		goto error_dispatch;
	}

	nx_log(NX_INFO, "Successfully loaded libseat session");
	return true;

error_dispatch:
	wl_event_source_remove(session->libseat_event);
	session->libseat_event = NULL;
error:
	libseat_close_seat(session->seat_handle);
	session->seat_handle = NULL;
	return false;
}

void session_seat_finish(struct nx_session *session) {
	libseat_close_seat(session->seat_handle);
	wl_event_source_remove(session->libseat_event);
	session->seat_handle = NULL;
	session->libseat_event = NULL;
}

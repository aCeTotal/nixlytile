#include <stdlib.h>
#include <string.h>
#include "platform/backend/headless/headless.h"
#include "platform/backend/multi.h"
#include "platform/backend/primary_watch.h"
#include "platform/drm/drm.h"
#include "platform/drm/monitor.h"
#include "platform/input/libinput/libinput_backend.h"
#include "platform/log/log.h"
#include "platform/nested/nested.h"
#include "platform/os/clock.h"
#include "platform/os/env.h"
#include "platform/session/session.h"

static const int64_t WAIT_SESSION_TIMEOUT_MS = 10000;
#define MAX_GPUS 8
static const unsigned int HEADLESS_WIDTH = 1280;
static const unsigned int HEADLESS_HEIGHT = 720;

struct autocreate {
	struct wl_event_loop *loop;
	struct nx_backend *multi;
	struct nx_session *session;
};

static struct nx_session *session_create_and_wait(struct wl_event_loop *loop) {
	struct nx_session *session = nx_session_create(loop);
	if (session == NULL) {
		nx_log(NX_ERROR, "Failed to start a session");
		return NULL;
	}
	if (session->active) {
		return session;
	}

	nx_log(NX_INFO, "Waiting for a session to become active");
	int64_t deadline = get_current_time_msec() + WAIT_SESSION_TIMEOUT_MS;
	int64_t timeout = WAIT_SESSION_TIMEOUT_MS;
	while (!session->active) {
		if (wl_event_loop_dispatch(loop, (int)timeout) < 0) {
			nx_log_errno(NX_ERROR, "Failed to wait for session active: "
				"wl_event_loop_dispatch failed");
			return NULL;
		}
		int64_t now = get_current_time_msec();
		if (now >= deadline) {
			break;
		}
		timeout = deadline - now;
	}

	if (!session->active) {
		nx_log(NX_ERROR, "Timeout waiting session to become active");
		return NULL;
	}
	return session;
}

static bool ensure_session(struct autocreate *ctx) {
	if (ctx->session != NULL) {
		return true;
	}
	ctx->session = session_create_and_wait(ctx->loop);
	if (ctx->session == NULL) {
		nx_log(NX_ERROR, "failed to start a session");
		return false;
	}
	return true;
}

static size_t parse_outputs_env(const char *name) {
	const char *outputs_str = getenv(name);
	if (outputs_str == NULL) {
		return 1;
	}

	char *end;
	int outputs = (int)strtol(outputs_str, &end, 10);
	if (*end || outputs < 0) {
		nx_log(NX_ERROR, "%s specified with invalid integer, ignoring", name);
		return 1;
	}
	return outputs;
}

static struct nx_backend *create_nested(struct wl_event_loop *loop) {
	struct nx_backend *backend = nx_wl_backend_create(loop);
	if (backend == NULL) {
		return NULL;
	}
	size_t outputs = parse_outputs_env("NIXLY_WL_OUTPUTS");
	for (size_t i = 0; i < outputs; ++i) {
		nx_wl_output_create(backend);
	}
	return backend;
}

// Returns the primary GPU's backend.
static struct nx_backend *add_drm(struct autocreate *ctx) {
	struct nx_device *gpus[MAX_GPUS];
	ssize_t num_gpus = nx_session_find_gpus(ctx->session, MAX_GPUS, gpus);
	if (num_gpus < 0) {
		nx_log(NX_ERROR, "Failed to find GPUs");
		return NULL;
	}
	if (num_gpus == 0) {
		nx_log(NX_ERROR, "Found 0 GPUs, cannot create backend");
		return NULL;
	}
	nx_log(NX_INFO, "Found %zd GPUs", num_gpus);

	struct nx_backend *primary_drm = NULL;
	for (ssize_t i = 0; i < num_gpus; ++i) {
		struct nx_backend *drm =
			nx_drm_backend_create(ctx->session, gpus[i], primary_drm);
		if (drm == NULL) {
			nx_log(NX_ERROR, "Failed to create DRM backend");
			continue;
		}
		if (primary_drm == NULL) {
			primary_drm = drm;
		}
		nx_multi_backend_add(ctx->multi, drm);
	}
	if (primary_drm == NULL) {
		nx_log(NX_ERROR, "Could not successfully create backend on any GPU");
		return NULL;
	}

	if (getenv("NIXLY_DRM_DEVICES") == NULL) {
		drm_backend_monitor_create(ctx->multi, primary_drm, ctx->session);
	}
	return primary_drm;
}

static bool add_child(struct autocreate *ctx, struct nx_backend *backend) {
	return backend != NULL && nx_multi_backend_add(ctx->multi, backend);
}

static bool add_named_nested(struct autocreate *ctx) {
	return add_child(ctx, create_nested(ctx->loop));
}

static bool add_named_headless(struct autocreate *ctx) {
	struct nx_backend *backend = nx_headless_backend_create(ctx->loop);
	if (backend == NULL) {
		return false;
	}
	size_t outputs = parse_outputs_env("NIXLY_HEADLESS_OUTPUTS");
	for (size_t i = 0; i < outputs; ++i) {
		nx_headless_add_output(backend, HEADLESS_WIDTH, HEADLESS_HEIGHT);
	}
	return add_child(ctx, backend);
}

static bool add_named_drm(struct autocreate *ctx) {
	return ensure_session(ctx) && add_drm(ctx) != NULL;
}

static bool add_named_libinput(struct autocreate *ctx) {
	return ensure_session(ctx) &&
		add_child(ctx, nx_libinput_backend_create(ctx->session));
}

static const struct {
	const char *name;
	bool (*add)(struct autocreate *ctx);
} named_backends[] = {
	{ "wayland", add_named_nested },
	{ "headless", add_named_headless },
	{ "drm", add_named_drm },
	{ "libinput", add_named_libinput },
};

static bool add_named(struct autocreate *ctx, const char *name) {
	for (size_t i = 0; i < sizeof(named_backends) / sizeof(named_backends[0]); i++) {
		if (strcmp(name, named_backends[i].name) == 0) {
			return named_backends[i].add(ctx);
		}
	}
	nx_log(NX_ERROR, "unrecognized backend '%s'", name);
	return false;
}

static bool add_listed(struct autocreate *ctx, const char *list) {
	nx_log(NX_INFO, "Loading user-specified backends due to NIXLY_BACKENDS: %s",
		list);

	char *names = strdup(list);
	if (names == NULL) {
		nx_log(NX_ERROR, "allocation failed");
		return false;
	}

	bool ok = true;
	char *saveptr;
	char *name = strtok_r(names, ",", &saveptr);
	while (ok && name != NULL) {
		ok = add_named(ctx, name);
		if (!ok) {
			nx_log(NX_ERROR, "failed to add backend '%s'", name);
		}
		name = strtok_r(NULL, ",", &saveptr);
	}
	free(names);
	return ok;
}

static bool add_host_nested(struct autocreate *ctx) {
	struct nx_backend *nested = create_nested(ctx->loop);
	if (nested == NULL) {
		return false;
	}
	nx_multi_backend_add(ctx->multi, nested);
	return backend_watch_primary(ctx->multi, nested);
}

static bool add_libinput(struct autocreate *ctx) {
	struct nx_backend *libinput = nx_libinput_backend_create(ctx->session);
	if (libinput != NULL) {
		nx_multi_backend_add(ctx->multi, libinput);
		return backend_watch_primary(ctx->multi, libinput);
	}
	if (env_parse_bool("NIXLY_LIBINPUT_NO_DEVICES")) {
		nx_log(NX_INFO, "NIXLY_LIBINPUT_NO_DEVICES is set, "
			"starting without libinput backend");
		return true;
	}
	nx_log(NX_ERROR, "Failed to start libinput backend");
	nx_log(NX_ERROR, "Set NIXLY_LIBINPUT_NO_DEVICES=1 to skip libinput");
	return false;
}

static bool add_session_backends(struct autocreate *ctx) {
	ctx->session = session_create_and_wait(ctx->loop);
	if (ctx->session == NULL) {
		nx_log(NX_ERROR, "Failed to start a DRM session");
		return false;
	}
	if (!add_libinput(ctx)) {
		return false;
	}

	struct nx_backend *primary_drm = add_drm(ctx);
	if (primary_drm == NULL) {
		nx_log(NX_ERROR, "Failed to open any DRM device");
		return false;
	}
	return backend_watch_primary(ctx->multi, primary_drm);
}

static bool add_backends(struct autocreate *ctx) {
	const char *list = getenv("NIXLY_BACKENDS");
	if (list != NULL) {
		return add_listed(ctx, list);
	}
	if (getenv("WAYLAND_DISPLAY") || getenv("WAYLAND_SOCKET")) {
		return add_host_nested(ctx);
	}
	return add_session_backends(ctx);
}

struct nx_backend *nx_backend_autocreate(struct wl_event_loop *loop,
		struct nx_session **session_ptr) {
	*session_ptr = NULL;

	struct autocreate ctx = {
		.loop = loop,
		.multi = nx_multi_backend_create(loop),
	};
	if (ctx.multi == NULL) {
		nx_log(NX_ERROR, "could not allocate multibackend");
		return NULL;
	}

	if (!add_backends(&ctx)) {
		nx_backend_destroy(ctx.multi);
		nx_session_destroy(ctx.session);
		return NULL;
	}
	*session_ptr = ctx.session;
	return ctx.multi;
}

#include "nixlytile.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define FC_OBJ_PATH  "/org/freedesktop/portal/desktop"
#define FC_IFACE     "org.freedesktop.impl.portal.FileChooser"
#define FC_REQ_IFACE "org.freedesktop.impl.portal.Request"
#define FC_BACKEND   "org.freedesktop.impl.portal.desktop.gtk"
#define FC_WAYLAND   "wayland:"
#define FC_REQ_MAX   65536

enum { FC_SUCCESS, FC_CANCELLED, FC_FAILED };

typedef struct {
	struct wl_list link;
	sd_bus_message *call;
	sd_bus_message *picked;
	sd_bus_slot *fwd;
	sd_bus_slot *obj;
	char *handle;
	pid_t pid;
	int grant_fd;
	struct wl_event_source *grant_src;
} FCRequest;

static sd_bus_slot *fc_slot;
static struct wl_list fc_requests;

static void
fc_free(FCRequest *req)
{
	if (req->grant_src)
		wl_event_source_remove(req->grant_src);
	if (req->grant_fd >= 0)
		close(req->grant_fd);
	wl_list_remove(&req->link);
	sd_bus_slot_unref(req->fwd);
	sd_bus_slot_unref(req->obj);
	sd_bus_message_unref(req->picked);
	sd_bus_message_unref(req->call);
	free(req->handle);
	free(req);
}

static void
fc_reply_code(FCRequest *req, uint32_t code)
{
	sd_bus_reply_method_return(req->call, "ua{sv}", code, 0);
	fc_free(req);
}

static void
fc_reply_picked(FCRequest *req)
{
	sd_bus_message *out = NULL;

	sd_bus_message_rewind(req->picked, 1);
	if (sd_bus_message_new_method_return(req->call, &out) < 0 ||
			sd_bus_message_copy(out, req->picked, 1) < 0) {
		sd_bus_message_unref(out);
		fc_reply_code(req, FC_FAILED);
		return;
	}
	sd_bus_send(NULL, out, NULL);
	sd_bus_message_unref(out);
	fc_free(req);
}

static void
fc_deny(FCRequest *req)
{
	if (selmon)
		osd_show(selmon, "Filtilgang avvist");
	fc_reply_code(req, FC_CANCELLED);
}

static int
fc_hex(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int
fc_uri_path(const char *uri, char *out, size_t cap)
{
	const char *s;
	size_t n = 0;

	if (strncmp(uri, "file://", 7) != 0)
		return 0;
	for (s = uri + 7; *s && n + 1 < cap; s++) {
		int hi = *s == '%' ? fc_hex(s[1]) : -1;
		int lo = hi >= 0 ? fc_hex(s[2]) : -1;

		if (lo < 0) {
			out[n++] = *s;
			continue;
		}
		if ((hi << 4 | lo) == 0)
			return 0;
		out[n++] = (char)(hi << 4 | lo);
		s += 2;
	}
	out[n] = '\0';
	return *s == '\0' && out[0] == '/';
}

/* Leaves the message inside uris. */
static int
fc_enter_uris(sd_bus_message *m)
{
	const char *key;
	uint32_t response;

	if (sd_bus_message_read(m, "u", &response) < 0 || response != FC_SUCCESS)
		return 0;
	if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0)
		return 0;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		if (sd_bus_message_read(m, "s", &key) < 0)
			return 0;
		if (strcmp(key, "uris") == 0)
			return sd_bus_message_enter_container(m, 'v', "as") > 0 &&
				sd_bus_message_enter_container(m, 'a', "s") > 0;
		sd_bus_message_skip(m, "v");
		sd_bus_message_exit_container(m);
	}
	return 0;
}

/* Builds "pid\0path\0..." of hidden picks. */
static ssize_t
fc_hidden_request(FCRequest *req, char *buf, size_t cap)
{
	char path[PATH_MAX];
	const char *uri;
	size_t head = (size_t)snprintf(buf, cap, "%d", (int)req->pid) + 1;
	size_t len = head;

	sd_bus_message_rewind(req->picked, 1);
	if (!fc_enter_uris(req->picked))
		return 0;
	while (sd_bus_message_read(req->picked, "s", &uri) > 0) {
		size_t plen;

		if (!fc_uri_path(uri, path, sizeof(path)) ||
				!jail_hides(req->pid, path))
			continue;
		plen = strlen(path) + 1;
		if (len + plen > cap)
			return -1;
		memcpy(buf + len, path, plen);
		len += plen;
	}
	return len > head ? (ssize_t)len : 0;
}

static int
fc_grant_done(int fd, uint32_t mask, void *data)
{
	FCRequest *req = data;

	if (!jail_grant_ok(fd)) {
		fc_deny(req);
		return 0;
	}
	fc_reply_picked(req);
	return 0;
}

static int
fc_grant_start(FCRequest *req, const char *buf, size_t len)
{
	req->grant_fd = jail_grant_send(buf, len);
	if (req->grant_fd < 0)
		return -1;
	req->grant_src = wl_event_loop_add_fd(event_loop, req->grant_fd,
		WL_EVENT_READABLE, fc_grant_done, req);
	return req->grant_src ? 0 : -1;
}

static int
fc_backend_reply(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	static char buf[FC_REQ_MAX];
	FCRequest *req = userdata;
	ssize_t len;

	req->fwd = sd_bus_slot_unref(req->fwd);
	if (sd_bus_message_is_method_error(m, NULL)) {
		fc_reply_code(req, FC_FAILED);
		return 0;
	}
	req->picked = sd_bus_message_ref(m);
	len = req->pid > 0 ? fc_hidden_request(req, buf, sizeof(buf)) : 0;
	if (len == 0) {
		fc_reply_picked(req);
		return 0;
	}
	if (len < 0 || fc_grant_start(req, buf, (size_t)len) < 0)
		fc_deny(req);
	return 0;
}

static int
fc_close(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	FCRequest *req = userdata;

	sd_bus_reply_method_return(m, "");
	if (!req->fwd) {
		fc_reply_code(req, FC_CANCELLED);
		return 0;
	}
	sd_bus_call_method_async(sd_bus_message_get_bus(m), NULL, FC_BACKEND,
		req->handle, FC_REQ_IFACE, "Close", NULL, NULL, "");
	return 0;
}

static const sd_bus_vtable fc_req_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", fc_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

static pid_t
fc_parent_pid(const char *parent)
{
	struct wlr_xdg_foreign_exported *exported = NULL;
	pid_t pid = 0;
	uid_t uid;
	gid_t gid;

	if (strncmp(parent, FC_WAYLAND, strlen(FC_WAYLAND)) == 0)
		exported = wlr_xdg_foreign_registry_find_by_handle(
			foreign_registry, parent + strlen(FC_WAYLAND));
	if (!exported)
		return client_get_pid(focustop(selmon));
	wl_client_get_credentials(wl_resource_get_client(
		exported->toplevel->base->surface->resource), &pid, &uid, &gid);
	return pid;
}

static int
fc_forward(sd_bus_message *m, const char *handle, pid_t pid)
{
	sd_bus *bus = sd_bus_message_get_bus(m);
	sd_bus_message *call = NULL;
	FCRequest *req = calloc(1, sizeof(*req));
	int r;

	if (!req)
		return -ENOMEM;
	wl_list_insert(&fc_requests, &req->link);
	req->grant_fd = -1;
	req->pid = pid;
	req->call = sd_bus_message_ref(m);
	req->handle = strdup(handle);
	r = req->handle ? sd_bus_message_new_method_call(bus, &call,
		FC_BACKEND, FC_OBJ_PATH, FC_IFACE,
		sd_bus_message_get_member(m)) : -ENOMEM;
	if (r >= 0)
		r = sd_bus_message_rewind(m, 1);
	if (r >= 0)
		r = sd_bus_message_copy(call, m, 1);
	if (r >= 0)
		r = sd_bus_call_async(bus, &req->fwd, call, fc_backend_reply,
			req, UINT64_MAX);
	if (r >= 0)
		r = sd_bus_add_object_vtable(bus, &req->obj, req->handle,
			FC_REQ_IFACE, fc_req_vtable, req);
	sd_bus_message_unref(call);
	if (r < 0) {
		fc_free(req);
		return r;
	}
	return 1;
}

static int
fc_method(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *handle, *app_id, *parent;
	int r = sd_bus_message_read(m, "oss", &handle, &app_id, &parent);

	if (r < 0)
		return r;
	if (strcmp(sd_bus_message_get_member(m), "OpenFile") != 0)
		return fc_forward(m, handle, 0);
	return fc_forward(m, handle, fc_parent_pid(parent));
}

static const sd_bus_vtable fc_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("OpenFile", "osssa{sv}", "ua{sv}", fc_method,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SaveFile", "osssa{sv}", "ua{sv}", fc_method,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SaveFiles", "osssa{sv}", "ua{sv}", fc_method,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

void
filechooser_init(void)
{
	sd_bus *bus = portal_bus();

	wl_list_init(&fc_requests);
	if (!bus)
		return;
	if (sd_bus_add_object_vtable(bus, &fc_slot, FC_OBJ_PATH, FC_IFACE,
			fc_vtable, NULL) < 0)
		wlr_log(WLR_ERROR, "filechooser: cannot export %s", FC_IFACE);
}

void
filechooser_cleanup(void)
{
	FCRequest *req, *tmp;

	wl_list_for_each_safe(req, tmp, &fc_requests, link)
		fc_reply_code(req, FC_FAILED);
	fc_slot = sd_bus_slot_unref(fc_slot);
}

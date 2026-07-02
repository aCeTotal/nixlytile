#include "nixlytile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * Minimal Niri-IPC server, scope B (workspaces only).
 *
 * Wire protocol (newline-delimited JSON over a Unix stream socket):
 *   client → "EventStream"\n
 *   server → {"Ok":"Handled"}\n
 *   server → {"WorkspacesChanged":{"workspaces":[...]}}\n
 *   server → {"WorkspaceActivated":{"id":N,"focused":true}}\n   (on switch)
 *
 *   click on workspace button:
 *   client → {"Action":{"FocusWorkspace":{"reference":{"Id":N}}}}\n
 *   server → {"Ok":"Handled"}\n
 *
 * Socket path is exported via the NIRI_SOCKET env var so waybar's
 * niri/workspaces module connects without any extra config.
 */

#define NIRI_IN_BUF_MAX  (16 * 1024)
#define NIRI_OUT_BUF_MAX (256 * 1024)

typedef struct NiriIpcClient {
	struct wl_list link;
	int fd;
	struct wl_event_source *src;     /* readable + writable combined */
	char in_buf[NIRI_IN_BUF_MAX];
	size_t in_len;
	char *out_buf;                   /* heap; grows up to NIRI_OUT_BUF_MAX */
	size_t out_cap;
	size_t out_len;
	int subscribed;                  /* sent EventStream request */
	int writable_armed;              /* mask currently includes WL_EVENT_WRITABLE */
} NiriIpcClient;

static int listen_fd = -1;
static struct wl_event_source *listen_src = NULL;
static struct wl_list ipc_clients;   /* NiriIpcClient.link */
static char socket_path[256];

/* ── helpers ──────────────────────────────────────────────────────── */

static void
set_cloexec_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(fd, F_GETFD, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static void
client_arm_write(NiriIpcClient *cl, int on)
{
	if (!cl->src)
		return;
	if (on == cl->writable_armed)
		return;
	uint32_t mask = WL_EVENT_READABLE;
	if (on)
		mask |= WL_EVENT_WRITABLE;
	wl_event_source_fd_update(cl->src, mask);
	cl->writable_armed = on;
}

static int
client_grow_out(NiriIpcClient *cl, size_t need)
{
	if (cl->out_cap >= need)
		return 1;
	size_t cap = cl->out_cap ? cl->out_cap : 1024;
	while (cap < need) {
		if (cap >= NIRI_OUT_BUF_MAX)
			return 0;
		cap *= 2;
		if (cap > NIRI_OUT_BUF_MAX)
			cap = NIRI_OUT_BUF_MAX;
	}
	char *p = realloc(cl->out_buf, cap);
	if (!p)
		return 0;
	cl->out_buf = p;
	cl->out_cap = cap;
	return 1;
}

static void
client_enqueue(NiriIpcClient *cl, const char *data, size_t len)
{
	if (!len)
		return;
	if (cl->out_len + len > NIRI_OUT_BUF_MAX) {
		/* Slow consumer — drop and close to avoid OOM. */
		cl->out_len = 0;
		shutdown(cl->fd, SHUT_RDWR);
		return;
	}
	if (!client_grow_out(cl, cl->out_len + len)) {
		shutdown(cl->fd, SHUT_RDWR);
		return;
	}
	memcpy(cl->out_buf + cl->out_len, data, len);
	cl->out_len += len;
	client_arm_write(cl, 1);
}

static void
client_destroy(NiriIpcClient *cl)
{
	if (cl->src) {
		wl_event_source_remove(cl->src);
		cl->src = NULL;
	}
	if (cl->fd >= 0) {
		close(cl->fd);
		cl->fd = -1;
	}
	wl_list_remove(&cl->link);
	free(cl->out_buf);
	free(cl);
}

/* ── JSON build ───────────────────────────────────────────────────── */

static size_t
json_str_escape(char *dst, size_t cap, const char *src)
{
	size_t i = 0;
	if (cap < 3)
		return 0;
	dst[i++] = '"';
	while (*src && i + 7 < cap) {
		unsigned char c = (unsigned char)*src++;
		if (c == '"' || c == '\\') {
			dst[i++] = '\\';
			dst[i++] = (char)c;
		} else if (c == '\n') { dst[i++] = '\\'; dst[i++] = 'n';
		} else if (c == '\r') { dst[i++] = '\\'; dst[i++] = 'r';
		} else if (c == '\t') { dst[i++] = '\\'; dst[i++] = 't';
		} else if (c < 0x20) {
			i += snprintf(dst + i, cap - i, "\\u%04x", c);
		} else {
			dst[i++] = (char)c;
		}
	}
	dst[i++] = '"';
	return i;
}

static void
build_workspaces_changed(char **out_buf, size_t *out_len)
{
	/* Grow as we go.  Use a local buffer then strdup. */
	size_t cap = 4096;
	char *buf = malloc(cap);
	if (!buf) { *out_buf = NULL; *out_len = 0; return; }
	size_t p = 0;

	#define APPEND(fmt, ...) do { \
		int _need = snprintf(NULL, 0, fmt, ##__VA_ARGS__) + 1; \
		while (p + _need > cap) { \
			size_t ncap = cap * 2; \
			char *np = realloc(buf, ncap); \
			if (!np) { free(buf); *out_buf = NULL; *out_len = 0; return; } \
			buf = np; cap = ncap; \
		} \
		p += snprintf(buf + p, cap - p, fmt, ##__VA_ARGS__); \
	} while (0)

	APPEND("{\"WorkspacesChanged\":{\"workspaces\":[");

	int first = 1;
	Monitor *m;
	Workspace *ws;
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output)
			continue;
		const char *output_name = m->wlr_output->name ? m->wlr_output->name : "";
		char out_esc[128];
		size_t out_esc_len = json_str_escape(out_esc, sizeof out_esc, output_name);
		out_esc[out_esc_len] = 0;

		/* Find max existing idx + whether the active ws has any client. */
		int max_idx = -1;
		int active_has_clients = 0;
		wl_list_for_each(ws, &m->workspaces, link) {
			if (ws->idx > max_idx)
				max_idx = ws->idx;
			if (m->active_ws == ws) {
				Column *col;
				wl_list_for_each(col, &ws->columns, link) {
					if (col->n_clients > 0) {
						active_has_clients = 1;
						break;
					}
				}
			}
		}

		/* Emit existing workspaces in idx order.  We track whether a
		 * trailing empty already exists; if not, synthesize one after
		 * the loop so Niri's "one empty after last occupied" rule holds. */
		int last_has_clients = 0;
		int emitted_any = 0;
		for (int idx = 0; idx <= max_idx; idx++) {
			Workspace *match = NULL;
			wl_list_for_each(ws, &m->workspaces, link) {
				if (ws->idx == idx) { match = ws; break; }
			}
			if (!match)
				continue;

			int has_clients = 0;
			Column *col;
			wl_list_for_each(col, &match->columns, link) {
				if (col->n_clients > 0) { has_clients = 1; break; }
			}
			int is_active = (m->active_ws == match) ? 1 : 0;
			int is_focused = (selmon == m && is_active) ? 1 : 0;

			/* Hide truly-empty non-active middle workspaces so the bar
			 * only lists occupied + active + trailing-empty.  Niri does
			 * this server-side. */
			if (!has_clients && !is_active && idx < max_idx)
				continue;

			if (!first)
				APPEND(",");
			APPEND("{\"id\":%llu,\"idx\":%d,\"name\":null,\"output\":%s,"
				"\"is_urgent\":false,\"is_active\":%s,\"is_focused\":%s,"
				"\"active_window_id\":%s}",
				(unsigned long long)match->window_id,
				match->idx + 1,   /* Niri idx is 1-based */
				out_esc,
				is_active ? "true" : "false",
				is_focused ? "true" : "false",
				has_clients ? "1" : "null");
			first = 0;
			emitted_any = 1;
			last_has_clients = has_clients;
		}

		/* If the last visible workspace has clients (or none emitted),
		 * synthesize one trailing empty workspace for this output. */
		(void)active_has_clients;
		if (!emitted_any || last_has_clients) {
			if (!first)
				APPEND(",");
			/* Synthesized trailing empty has a unique id derived
			 * from an FNV-1a hash of the output name so successive
			 * runs stay stable for waybar's "is this the same
			 * workspace?" — a heap pointer here both leaked ASLR
			 * layout to IPC clients and changed across restarts. */
			uint32_t name_hash = 2166136261u;
			for (const char *p = m->wlr_output->name; *p; p++)
				name_hash = (name_hash ^ (unsigned char)*p) * 16777619u;
			uint64_t synth_id = (uint64_t)0xFFFFFFFF00000000ULL
				| (uint64_t)name_hash;
			APPEND("{\"id\":%llu,\"idx\":%d,\"name\":null,\"output\":%s,"
				"\"is_urgent\":false,\"is_active\":false,\"is_focused\":false,"
				"\"active_window_id\":null}",
				(unsigned long long)synth_id,
				max_idx + 2,    /* idx after last (1-based) */
				out_esc);
			first = 0;
		}
	}

	APPEND("]}}\n");

	#undef APPEND

	*out_buf = buf;
	*out_len = p;
}

/* ── request handling ─────────────────────────────────────────────── */

static void
send_handled(NiriIpcClient *cl)
{
	static const char ok[] = "{\"Ok\":\"Handled\"}\n";
	client_enqueue(cl, ok, sizeof ok - 1);
}

static void
send_err(NiriIpcClient *cl, const char *msg)
{
	char buf[256];
	int n = snprintf(buf, sizeof buf, "{\"Err\":\"%s\"}\n", msg);
	if (n > 0)
		client_enqueue(cl, buf, (size_t)n);
}

static void
focus_workspace_by_window_id(uint64_t id)
{
	Monitor *m;
	Workspace *ws;
	wl_list_for_each(m, &mons, link) {
		wl_list_for_each(ws, &m->workspaces, link) {
			if (ws->window_id == id) {
				workspace_switch(m, ws);
				arrange(m);
				printstatus();
				return;
			}
		}
	}
}

static void
focus_workspace_by_idx_1based(unsigned int idx1)
{
	if (idx1 == 0)
		return;
	int target = (int)idx1 - 1;
	Monitor *m = selmon;
	if (!m)
		return;
	Workspace *ws;
	wl_list_for_each(ws, &m->workspaces, link) {
		if (ws->idx == target) {
			workspace_switch(m, ws);
			arrange(m);
			printstatus();
			return;
		}
	}
}

static void
send_initial_state(NiriIpcClient *cl)
{
	char *buf = NULL;
	size_t len = 0;
	build_workspaces_changed(&buf, &len);
	if (buf) {
		client_enqueue(cl, buf, len);
		free(buf);
	}
}

static void
handle_request_line(NiriIpcClient *cl, char *line)
{
	/* Trim trailing whitespace. */
	size_t n = strlen(line);
	while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' ||
			line[n-1] == ' ' || line[n-1] == '\t'))
		line[--n] = 0;
	while (*line == ' ' || *line == '\t') line++;

	if (n == 0)
		return;

	/* EventStream — switch to streaming mode. */
	if (strcmp(line, "\"EventStream\"") == 0) {
		send_handled(cl);
		cl->subscribed = 1;
		send_initial_state(cl);
		return;
	}

	/* Action::VideoPlaying / Action::VideoStopped — nixlymedia tells us
	 * what output its fullscreen video is on and what container-fps the
	 * stream has. We delegate to apply_best_video_mode (which picks VRR
	 * if capable, else exact mode, else integer multiple). On stop we
	 * call restore_max_refresh_rate. Surface-scoped: payload carries the
	 * wl_output name so we never guess a monitor. */
	if (strstr(line, "\"VideoPlaying\"") || strstr(line, "\"VideoStopped\"")) {
		int is_stop = strstr(line, "\"VideoStopped\"") != NULL;
		const char *p = strstr(line, "\"output\":");
		if (!p) { send_err(cl, "missing output"); return; }
		p = strchr(p + 9, '"');
		if (!p) { send_err(cl, "bad output"); return; }
		const char *s = p + 1;
		const char *e = strchr(s, '"');
		if (!e || e == s) { send_err(cl, "bad output"); return; }
		char name[64];
		size_t nlen = (size_t)(e - s);
		if (nlen >= sizeof name) nlen = sizeof name - 1;
		memcpy(name, s, nlen);
		name[nlen] = 0;

		Monitor *m = NULL, *it;
		wl_list_for_each(it, &mons, link) {
			if (it->wlr_output && it->wlr_output->name &&
			    strcmp(it->wlr_output->name, name) == 0) {
				m = it;
				break;
			}
		}
		if (!m) { send_err(cl, "output not found"); return; }

		if (is_stop) {
			restore_max_refresh_rate(m);
		} else {
			const char *fp = strstr(line, "\"fps\":");
			if (!fp) { send_err(cl, "missing fps"); return; }
			float fps = strtof(fp + 6, NULL);
			if (fps <= 0.0f) { send_err(cl, "bad fps"); return; }
			apply_best_video_mode(m, fps);
		}
		send_handled(cl);
		return;
	}

	/* Action::FocusWorkspace — accept by Id or Index. */
	if (strstr(line, "\"FocusWorkspace\"")) {
		const char *p;
		if ((p = strstr(line, "\"Id\":"))) {
			uint64_t id = strtoull(p + 5, NULL, 10);
			focus_workspace_by_window_id(id);
			send_handled(cl);
			return;
		}
		if ((p = strstr(line, "\"Index\":"))) {
			unsigned int idx1 = (unsigned int)strtoul(p + 8, NULL, 10);
			focus_workspace_by_idx_1based(idx1);
			send_handled(cl);
			return;
		}
		send_err(cl, "Unsupported FocusWorkspace reference");
		return;
	}

	/* Anything else: politely reject. */
	send_err(cl, "Not implemented");
}

static void
client_drain_in(NiriIpcClient *cl)
{
	/* Process one or more newline-delimited requests. */
	for (;;) {
		char *nl = memchr(cl->in_buf, '\n', cl->in_len);
		if (!nl)
			break;
		size_t line_len = (size_t)(nl - cl->in_buf);
		char saved = cl->in_buf[line_len];
		cl->in_buf[line_len] = 0;
		handle_request_line(cl, cl->in_buf);
		cl->in_buf[line_len] = saved;
		size_t consumed = line_len + 1;
		size_t rest = cl->in_len - consumed;
		if (rest)
			memmove(cl->in_buf, cl->in_buf + consumed, rest);
		cl->in_len = rest;
	}
}

static int
client_event(int fd, uint32_t mask, void *data)
{
	NiriIpcClient *cl = data;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		client_destroy(cl);
		return 0;
	}

	if (mask & WL_EVENT_READABLE) {
		while (cl->in_len < sizeof cl->in_buf) {
			ssize_t r = read(fd, cl->in_buf + cl->in_len,
					sizeof cl->in_buf - cl->in_len);
			if (r > 0) {
				cl->in_len += (size_t)r;
				continue;
			}
			if (r == 0) {
				client_destroy(cl);
				return 0;
			}
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			client_destroy(cl);
			return 0;
		}
		client_drain_in(cl);
		if (cl->in_len == sizeof cl->in_buf) {
			/* Buffer full, no newline — broken client. */
			client_destroy(cl);
			return 0;
		}
	}

	if (mask & WL_EVENT_WRITABLE) {
		while (cl->out_len > 0) {
			ssize_t w = write(fd, cl->out_buf, cl->out_len);
			if (w > 0) {
				size_t rest = cl->out_len - (size_t)w;
				if (rest)
					memmove(cl->out_buf, cl->out_buf + w, rest);
				cl->out_len = rest;
				continue;
			}
			if (w < 0 && errno == EINTR)
				continue;
			if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			client_destroy(cl);
			return 0;
		}
		if (cl->out_len == 0)
			client_arm_write(cl, 0);
	}

	return 0;
}

static int
listen_event(int fd, uint32_t mask, void *data)
{
	(void)data;
	if (!(mask & WL_EVENT_READABLE))
		return 0;

	for (;;) {
		int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			/* Other errors — keep listening. */
			break;
		}
		NiriIpcClient *cl = calloc(1, sizeof *cl);
		if (!cl) {
			close(cfd);
			continue;
		}
		cl->fd = cfd;
		struct wl_event_loop *loop = wl_display_get_event_loop(dpy);
		cl->src = wl_event_loop_add_fd(loop, cfd, WL_EVENT_READABLE,
				client_event, cl);
		cl->writable_armed = 0;
		if (!cl->src) {
			close(cfd);
			free(cl);
			continue;
		}
		wl_list_insert(&ipc_clients, &cl->link);
	}
	return 0;
}

/* ── public api ───────────────────────────────────────────────────── */

void
window_ipc_init(struct wl_event_loop *loop)
{
	wl_list_init(&ipc_clients);

	const char *xdg_rt = getenv("XDG_RUNTIME_DIR");
	if (!xdg_rt) {
		wlr_log(WLR_ERROR, "window_ipc: XDG_RUNTIME_DIR not set");
		return;
	}
	snprintf(socket_path, sizeof socket_path,
		"%s/nixlytile-niri-%d.sock", xdg_rt, (int)getpid());

	/* Best-effort: remove a stale socket if any (we own the path
	 * via getpid suffix, so collision should not normally happen). */
	unlink(socket_path);

	listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0) {
		wlr_log(WLR_ERROR, "window_ipc: socket: %s", strerror(errno));
		return;
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	if (strlen(socket_path) >= sizeof addr.sun_path) {
		wlr_log(WLR_ERROR, "window_ipc: socket path too long");
		close(listen_fd);
		listen_fd = -1;
		return;
	}
	strcpy(addr.sun_path, socket_path);

	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		wlr_log(WLR_ERROR, "window_ipc: bind %s: %s",
			socket_path, strerror(errno));
		close(listen_fd);
		listen_fd = -1;
		return;
	}
	if (listen(listen_fd, 8) < 0) {
		wlr_log(WLR_ERROR, "window_ipc: listen: %s", strerror(errno));
		close(listen_fd);
		unlink(socket_path);
		listen_fd = -1;
		return;
	}

	listen_src = wl_event_loop_add_fd(loop, listen_fd,
		WL_EVENT_READABLE, listen_event, NULL);
	if (!listen_src) {
		close(listen_fd);
		unlink(socket_path);
		listen_fd = -1;
		return;
	}

	/* Export NIRI_SOCKET so waybar's niri/workspaces module connects. */
	setenv("NIRI_SOCKET", socket_path, 1);
	wlr_log(WLR_INFO, "window_ipc: listening on %s", socket_path);
}

void
window_ipc_finish(void)
{
	NiriIpcClient *cl, *tmp;
	wl_list_for_each_safe(cl, tmp, &ipc_clients, link)
		client_destroy(cl);
	if (listen_src) {
		wl_event_source_remove(listen_src);
		listen_src = NULL;
	}
	if (listen_fd >= 0) {
		close(listen_fd);
		listen_fd = -1;
	}
	if (socket_path[0]) {
		unlink(socket_path);
		socket_path[0] = 0;
	}
}

void
window_ipc_publish_workspaces(void)
{
	/* Cache the last emitted doc — printstatus() calls this on every
	 * state change including title updates, which don't alter
	 * workspace state at all.  Skip both the O(n²) rebuild and the
	 * subscriber wakeups when nothing changed. */
	static char *last_doc;
	static size_t last_len;

	int have_subscriber = 0;
	NiriIpcClient *cl;
	wl_list_for_each(cl, &ipc_clients, link) {
		if (cl->subscribed) {
			have_subscriber = 1;
			break;
		}
	}
	if (!have_subscriber)
		return;

	char *buf = NULL;
	size_t len = 0;
	build_workspaces_changed(&buf, &len);
	if (!buf)
		return;

	if (last_doc && len == last_len && memcmp(last_doc, buf, len) == 0) {
		free(buf);
		return;
	}
	free(last_doc);
	last_doc = buf;
	last_len = len;

	wl_list_for_each(cl, &ipc_clients, link) {
		if (cl->subscribed)
			client_enqueue(cl, buf, len);
	}
}

void
window_ipc_publish_workspace_activated(void)
{
	static unsigned long long last_id;

	if (wl_list_empty(&ipc_clients))
		return;
	if (!selmon || !selmon->active_ws)
		return;

	/* Re-announcing the same workspace on every printstatus (e.g.
	 * per title change) makes subscribers re-render for nothing. */
	if (selmon->active_ws->window_id == last_id)
		return;
	last_id = selmon->active_ws->window_id;

	char buf[256];
	int n = snprintf(buf, sizeof buf,
		"{\"WorkspaceActivated\":{\"id\":%llu,\"focused\":true}}\n",
		(unsigned long long)selmon->active_ws->window_id);
	if (n <= 0)
		return;

	NiriIpcClient *cl;
	wl_list_for_each(cl, &ipc_clients, link) {
		if (cl->subscribed)
			client_enqueue(cl, buf, (size_t)n);
	}
}

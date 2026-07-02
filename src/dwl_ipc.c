#include "nixlytile.h"
#include "dwl-ipc-unstable-v2-protocol.h"

/*
 * dwl-ipc-unstable-v2 server-side implementation.
 *
 * Exposes nixlytile state (workspaces/tags, layout, focused title/appid)
 * to clients via the zdwl_ipc_manager_v2 global so waybar's dwl/tags
 * and dwl/window modules render identically to how niri/workspaces
 * looks in Niri.
 *
 * Mapping:
 *   - Niri workspaces (per-monitor) → dwl "tags".  Workspace idx → bit.
 *   - Current focused workspace → tag with state=active.
 *   - Workspace with urgent client → state=urgent.
 *   - Tag.clients = n_columns; tag.focused = 1 if active workspace.
 *
 * Click-to-switch:
 *   - waybar set_tags(tagmask) → find workspace whose idx-bit matches
 *     the LSB of tagmask and switch to it.
 *
 * Layout name + symbol:
 *   - We expose two layouts: "tile" and "max" (corresponds to
 *     column maximize_column toggle).  Layout symbol comes from
 *     active_ws.focused_col->fullscreen.
 *
 * Lifecycle:
 *   - One zdwl_ipc_manager_v2 global advertised in setup().
 *   - Per-(client, wl_output) pair: one zdwl_ipc_output_v2 resource,
 *     tracked in a wl_list rooted in DwlIpcOutput.link off the global.
 */

#define IPC_TAG_COUNT 10
static const char *const IPC_LAYOUT_NAMES[] = { "[]=", "[M]" };
#define IPC_LAYOUT_COUNT ((int)(sizeof(IPC_LAYOUT_NAMES)/sizeof(IPC_LAYOUT_NAMES[0])))

typedef struct DwlIpcOutput {
	struct wl_list link;          /* in dwl_ipc_outputs */
	struct wl_resource *resource; /* zdwl_ipc_output_v2 */
	struct wlr_output *output;    /* associated wl_output */
} DwlIpcOutput;

static struct wl_global *dwl_ipc_global;
static struct wl_list dwl_ipc_outputs; /* DwlIpcOutput.link */

/* ── output resource handlers ─────────────────────────────────────── */

static void
ipc_output_release(struct wl_client *client, struct wl_resource *res)
{
	(void)client;
	wl_resource_destroy(res);
}

static Monitor *
monitor_for_wlr_output(struct wlr_output *o)
{
	Monitor *m;
	if (!o)
		return NULL;
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output == o)
			return m;
	}
	return NULL;
}

static void
ipc_output_set_tags(struct wl_client *client, struct wl_resource *res,
		uint32_t tagmask, uint32_t toggle_tagset)
{
	DwlIpcOutput *out = wl_resource_get_user_data(res);
	Monitor *m;
	Workspace *ws;
	int target_idx = -1;
	int i;

	(void)client; (void)toggle_tagset;
	if (!out || !out->output)
		return;
	m = monitor_for_wlr_output(out->output);
	if (!m)
		return;

	/* Find LSB bit position. */
	for (i = 0; i < 31; i++) {
		if (tagmask & (1u << i)) {
			target_idx = i;
			break;
		}
	}
	if (target_idx < 0)
		return;

	/* Find or create workspace with that idx and switch. */
	wl_list_for_each(ws, &m->workspaces, link) {
		if (ws->idx == target_idx) {
			workspace_switch(m, ws);
			arrange(m);
			printstatus();
			return;
		}
	}
	/* Create workspaces up to idx N (Niri-style "fill in the gaps"). */
	while (m->n_workspaces <= target_idx) {
		ws = workspace_create(m);
		if (!ws)
			return;
		if (ws->idx == target_idx) {
			workspace_switch(m, ws);
			arrange(m);
			printstatus();
			return;
		}
	}
}

static void
ipc_output_set_client_tags(struct wl_client *client, struct wl_resource *res,
		uint32_t and_tags, uint32_t xor_tags)
{
	(void)client; (void)res; (void)and_tags; (void)xor_tags;
	/* Not implemented — Niri-style layout doesn't support per-client tag bits. */
}

static void
ipc_output_set_layout(struct wl_client *client, struct wl_resource *res,
		uint32_t index)
{
	DwlIpcOutput *out = wl_resource_get_user_data(res);
	Monitor *m;
	Column *col;
	(void)client;

	if (!out || !out->output)
		return;
	m = monitor_for_wlr_output(out->output);
	if (!m || !m->active_ws)
		return;
	col = m->active_ws->focused_col;
	if (!col)
		return;

	/* index 0 = "[]=" (tile, not fullscreen), 1 = "[M]" (column maximize). */
	if (index == 1)
		col->fullscreen = 1;
	else
		col->fullscreen = 0;
	arrange(m);
}

static const struct zdwl_ipc_output_v2_interface ipc_output_impl = {
	.release = ipc_output_release,
	.set_tags = ipc_output_set_tags,
	.set_client_tags = ipc_output_set_client_tags,
	.set_layout = ipc_output_set_layout,
};

static void
ipc_output_destroy(struct wl_resource *resource)
{
	DwlIpcOutput *out = wl_resource_get_user_data(resource);
	if (!out)
		return;
	wl_list_remove(&out->link);
	free(out);
}

/* ── manager resource handlers ────────────────────────────────────── */

static void
ipc_manager_release(struct wl_client *client, struct wl_resource *res)
{
	(void)client;
	wl_resource_destroy(res);
}

static void
ipc_manager_get_output(struct wl_client *client, struct wl_resource *mgr_res,
		uint32_t id, struct wl_resource *output_res)
{
	DwlIpcOutput *out;
	struct wlr_output *wlr_out;
	(void)mgr_res;

	/* plain calloc — ecalloc() die()s the whole compositor on OOM,
	 * turning a client-triggered request into a session kill and
	 * making the NULL branch below dead code. */
	out = calloc(1, sizeof(*out));
	if (!out) {
		wl_client_post_no_memory(client);
		return;
	}

	wlr_out = wlr_output_from_resource(output_res);
	out->output = wlr_out;

	out->resource = wl_resource_create(client,
			&zdwl_ipc_output_v2_interface, 1, id);
	if (!out->resource) {
		free(out);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(out->resource, &ipc_output_impl,
			out, ipc_output_destroy);
	wl_list_insert(&dwl_ipc_outputs, &out->link);

	/* Send initial state once the resource exists. */
	dwl_ipc_publish();
}

static const struct zdwl_ipc_manager_v2_interface ipc_manager_impl = {
	.release = ipc_manager_release,
	.get_output = ipc_manager_get_output,
};

static void
ipc_manager_bind(struct wl_client *client, void *data, uint32_t version,
		uint32_t id)
{
	struct wl_resource *resource;
	int i;
	(void)data;

	resource = wl_resource_create(client,
			&zdwl_ipc_manager_v2_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &ipc_manager_impl,
			NULL, NULL);

	/* Send tags-count + layout-name advertisements right after bind. */
	zdwl_ipc_manager_v2_send_tags(resource, IPC_TAG_COUNT);
	for (i = 0; i < IPC_LAYOUT_COUNT; i++)
		zdwl_ipc_manager_v2_send_layout(resource, IPC_LAYOUT_NAMES[i]);
}

/* ── per-frame publish ────────────────────────────────────────────── */

void
dwl_ipc_publish(void)
{
	DwlIpcOutput *out;
	Monitor *m;
	Workspace *ws;
	Client *c;
	int i;
	int active_layout;

	if (!dwl_ipc_global)
		return;

	wl_list_for_each(out, &dwl_ipc_outputs, link) {
		if (!out->resource || !out->output)
			continue;
		m = monitor_for_wlr_output(out->output);
		if (!m)
			continue;

		zdwl_ipc_output_v2_send_active(out->resource,
				m == selmon ? 1 : 0);

		/* Find the first empty tag idx — Niri shows occupied workspaces
		 * plus exactly one trailing empty slot.  We forge clients_count=1
		 * for that idx so waybar tags its button .occupied (visible);
		 * everything past it stays .empty and is hidden by CSS. */
		int next_empty_idx = -1;
		for (i = 0; i < IPC_TAG_COUNT; i++) {
			Workspace *probe = NULL;
			int has_clients = 0;
			wl_list_for_each(ws, &m->workspaces, link) {
				if (ws->idx == i) { probe = ws; break; }
			}
			if (probe) {
				Column *col;
				wl_list_for_each(col, &probe->columns, link) {
					if (col->n_clients > 0) { has_clients = 1; break; }
				}
			}
			if (!has_clients) { next_empty_idx = i; break; }
		}

		/* Build per-tag events.  For each tag idx 0..IPC_TAG_COUNT-1,
		 * compute state, client count and focused flag from the
		 * matching workspace on this monitor. */
		for (i = 0; i < IPC_TAG_COUNT; i++) {
			uint32_t state = 0;
			uint32_t clients_count = 0;
			uint32_t focused = 0;
			Workspace *match = NULL;

			wl_list_for_each(ws, &m->workspaces, link) {
				if (ws->idx == i) {
					match = ws;
					break;
				}
			}
			if (match) {
				Column *col;
				wl_list_for_each(col, &match->columns, link)
					clients_count += col->n_clients;

				if (m->active_ws == match)
					state |= ZDWL_IPC_OUTPUT_V2_TAG_STATE_ACTIVE;
				/* Check urgent flag on any client whose
				 * column lives in this workspace. */
				wl_list_for_each(c, &clients, link) {
					if (c->isurgent && c->column &&
							c->column->ws == match) {
						state |= ZDWL_IPC_OUTPUT_V2_TAG_STATE_URGENT;
						break;
					}
				}
				if (m->active_ws == match && match->focused_col &&
						!wl_list_empty(&match->focused_col->clients))
					focused = 1;
			}

			/* Mark the trailing "next-empty" tag as occupied so waybar
			 * keeps it visible; real-occupied tags keep their count.
			 * Only forge when the previous idx has clients — otherwise
			 * empty workspaces would chain visually without anchors. */
			if (i == next_empty_idx && clients_count == 0 && i > 0) {
				Workspace *prev_ws = NULL;
				int prev_has_clients = 0;
				wl_list_for_each(ws, &m->workspaces, link) {
					if (ws->idx == i - 1) { prev_ws = ws; break; }
				}
				if (prev_ws) {
					Column *col;
					wl_list_for_each(col, &prev_ws->columns, link) {
						if (col->n_clients > 0) { prev_has_clients = 1; break; }
					}
				}
				if (prev_has_clients)
					clients_count = 1;
			}

			zdwl_ipc_output_v2_send_tag(out->resource, i,
					state, clients_count, focused);
		}

		/* Layout: 1 if active workspace's focused column is
		 * fullscreen, else 0. */
		active_layout = 0;
		if (m->active_ws && m->active_ws->focused_col &&
				m->active_ws->focused_col->fullscreen)
			active_layout = 1;
		zdwl_ipc_output_v2_send_layout(out->resource, active_layout);
		zdwl_ipc_output_v2_send_layout_symbol(out->resource,
				IPC_LAYOUT_NAMES[active_layout]);

		/* Title + appid from focused client on this monitor. */
		c = focustop(m);
		zdwl_ipc_output_v2_send_title(out->resource,
				(c && client_get_title(c)) ? client_get_title(c) : "");
		zdwl_ipc_output_v2_send_appid(out->resource,
				(c && client_get_appid(c)) ? client_get_appid(c) : "");

		zdwl_ipc_output_v2_send_frame(out->resource);
	}
}

void
dwl_ipc_init(struct wl_display *display)
{
	wl_list_init(&dwl_ipc_outputs);
	dwl_ipc_global = wl_global_create(display,
			&zdwl_ipc_manager_v2_interface, 1,
			NULL, ipc_manager_bind);
}

void
dwl_ipc_finish(void)
{
	if (dwl_ipc_global) {
		wl_global_destroy(dwl_ipc_global);
		dwl_ipc_global = NULL;
	}
}

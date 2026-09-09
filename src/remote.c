/* remote.c — Sunshine/Moonlight remote desktop mode.
 *
 * A remote session claims a parked headless output, sizes it to the
 * client, moves the physical desktop onto it and blanks the screens.
 * Stopping puts everything back exactly where it was.
 */

#include <stdlib.h>
#include <string.h>

#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>

#include "nixlytile.h"
#include "client.h"
#include "remote.h"

int remote_outputs = 1;
int remote_mouse_speed = 1200;

static struct wlr_backend *headless;
static int active_sessions;

/* Physical monitors blanked by remote_start, in the order we took them */
static Monitor *parked_mons[MAX_MONITORS];
static int n_parked_mons;

/* Where selmon pointed before the session */
static char prev_selmon[32];

int
remote_is_active(void)
{
	return active_sessions > 0;
}

void
remote_backend_init(struct wl_display *display, int n_outputs)
{
	int i;

	if (!backend || !wlr_backend_is_multi(backend))
		return;
	/* `remote { outputs 0 }` means none: no headless backend, no parked
	 * output anywhere in the compositor.  Remote sessions are then
	 * unavailable until it is raised again. */
	if (n_outputs < 1) {
		wlr_log(WLR_INFO, "remote: disabled (outputs 0)");
		return;
	}
	if (n_outputs > REMOTE_MAX_OUTPUTS)
		n_outputs = REMOTE_MAX_OUTPUTS;

	if (!(headless = wlr_headless_backend_create(wl_display_get_event_loop(display)))) {
		wlr_log(WLR_ERROR, "remote: failed to create headless backend");
		return;
	}
	if (!wlr_multi_backend_add(backend, headless)) {
		wlr_log(WLR_ERROR, "remote: failed to add headless backend");
		wlr_backend_destroy(headless);
		headless = NULL;
		return;
	}

	for (i = 0; i < n_outputs; i++) {
		if (!wlr_headless_add_output(headless, REMOTE_PARK_W, REMOTE_PARK_H))
			wlr_log(WLR_ERROR, "remote: failed to add virtual output %d", i + 1);
	}
	wlr_log(WLR_INFO, "remote: %d virtual output(s) parked", n_outputs);
}

static Monitor *
virt_mon(int idx)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (m->is_virtual && m->virt_idx == idx)
			return m;
	}
	return NULL;
}

/* Physical monitors in layout order (left to right) */
static int
collect_physical(Monitor **out, int max)
{
	Monitor *m;
	int n = 0, i, j;

	wl_list_for_each(m, &mons, link) {
		if (m->is_virtual || m->is_mirror || !m->wlr_output->enabled)
			continue;
		if (n < max)
			out[n++] = m;
	}
	for (i = 1; i < n; i++) {
		Monitor *key = out[i];
		for (j = i - 1; j >= 0 && out[j]->m.x > key->m.x; j--)
			out[j + 1] = out[j];
		out[j + 1] = key;
	}
	return n;
}

/* Remember every client's slot so remote_stop can rebuild it exactly */
static void
record_clients(void)
{
	Monitor *m;
	Workspace *ws;
	Column *col;
	Client *c;
	int ws_i, col_i, row_i;

	wl_list_for_each(m, &mons, link) {
		if (m->is_virtual)
			continue;
		ws_i = 0;
		wl_list_for_each(ws, &m->workspaces, link) {
			col_i = 0;
			wl_list_for_each(col, &ws->columns, link) {
				row_i = 0;
				wl_list_for_each(c, &col->clients, column_link) {
					snprintf(c->rst_out, sizeof(c->rst_out), "%s",
						m->wlr_output->name);
					c->rst_ws = ws_i;
					c->rst_col = col_i;
					c->rst_row = row_i++;
				}
				col_i++;
			}
			ws_i++;
		}
	}
}

static void
clear_records(void)
{
	Client *c;

	wl_list_for_each(c, &clients, link) {
		c->rst_out[0] = '\0';
		c->rst_ws = c->rst_col = c->rst_row = 0;
	}
}

static void
configure_output(Monitor *m, const RemoteParams *p)
{
	struct wlr_output_state state;
	double scale = p->scale > 0.0 ? p->scale : 1.0;

	if (scale < 0.5) scale = 0.5;
	if (scale > 4.0) scale = 4.0;

	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, 1);
	/* mHz: the headless frame timer paces exactly at the client's fps */
	wlr_output_state_set_custom_mode(&state, p->width, p->height,
		p->fps > 0 ? p->fps * 1000 : REMOTE_PARK_HZ * 1000);
	wlr_output_state_set_scale(&state, (float)scale);

	if (!wlr_output_commit_state(m->wlr_output, &state))
		wlr_log(WLR_ERROR, "remote: commit failed for %s",
			m->wlr_output->name);
	wlr_output_state_finish(&state);

	m->asleep = 0;
	m->frame_scheduled = 0;
	m->hdr_force = p->hdr ? 1 : 0;
}

static void
park_output(Monitor *m)
{
	struct wlr_output_state state;

	m->hdr_force = 0;

	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, 1);
	wlr_output_state_set_custom_mode(&state, REMOTE_PARK_W, REMOTE_PARK_H,
		REMOTE_PARK_HZ * 1000);
	wlr_output_state_set_scale(&state, 1.0f);
	wlr_output_commit_state(m->wlr_output, &state);
	wlr_output_state_finish(&state);
}

/* Handheld: every physical monitor collapses onto one workspace */
static void
gather_to_single(Monitor *dst, Monitor **phys, int n_phys)
{
	Workspace *ws, *wstmp, *dest;
	Column *col, *coltmp;
	int i;

	for (i = 0; i < n_phys; i++) {
		if (!(dest = workspace_create(dst)))
			continue;
		wl_list_for_each_safe(ws, wstmp, &phys[i]->workspaces, link) {
			wl_list_for_each_safe(col, coltmp, &ws->columns, link)
				workspace_adopt_column(dest, col);
		}
	}
}

/* Desktop client: workspaces move 1:1 onto matching virtual outputs */
static void
spread_to_many(Monitor **phys, int n_phys, int n_virt)
{
	Workspace *ws, *wstmp;
	Monitor *dst;
	int i;

	for (i = 0; i < n_phys && i < n_virt; i++) {
		if (!(dst = virt_mon(i + 1)))
			continue;
		wl_list_for_each_safe(ws, wstmp, &phys[i]->workspaces, link) {
			if (workspace_has_clients(ws))
				workspace_move_to_monitor(ws, dst);
		}
	}
}

const char *
remote_start(const RemoteParams *p)
{
	Monitor *phys[MAX_MONITORS];
	Monitor *m;
	int n_phys, i;

	if (!p || p->width < 64 || p->height < 64)
		return "bad geometry";
	if (!(m = virt_mon(p->output)))
		return "no such virtual output";

	/* Later sessions only resize their own output */
	if (active_sessions > 0) {
		configure_output(m, p);
		arrange(m);
		active_sessions++;
		return NULL;
	}

	n_phys = collect_physical(phys, MAX_MONITORS);
	if (n_phys == 0)
		return "no physical monitors";

	record_clients();
	snprintf(prev_selmon, sizeof(prev_selmon), "%s",
		selmon && selmon->wlr_output ? selmon->wlr_output->name : "");

	active_sessions = 1;
	configure_output(m, p);

	if (p->outputs > 1)
		spread_to_many(phys, n_phys, p->outputs);
	else
		gather_to_single(m, phys, n_phys);

	/* Land on the first gathered workspace, not the bootstrap one */
	if (!wl_list_empty(&m->workspaces)) {
		Workspace *first = wl_container_of(m->workspaces.next, first, link);
		Workspace *ws;
		wl_list_for_each(ws, &m->workspaces, link) {
			if (workspace_has_clients(ws)) {
				first = ws;
				break;
			}
		}
		m->active_ws = first;
	}
	selmon = m;

	n_parked_mons = 0;
	for (i = 0; i < n_phys; i++) {
		parked_mons[n_parked_mons++] = phys[i];
		monitor_set_power(phys[i], 0);
	}

	arrange(m);
	focusclient(focustop(m), 1);
	printstatus();

	wlr_log(WLR_INFO, "remote: started %dx%d@%d hdr=%d scale=%.2f on %s",
		p->width, p->height, p->fps, p->hdr, p->scale, m->wlr_output->name);
	return NULL;
}

/* Put a client's column back on the monitor and workspace it came from */
static Workspace *
restore_target(Client *c)
{
	Monitor *m, *home = NULL;
	Workspace *ws;
	int i;

	wl_list_for_each(m, &mons, link) {
		if (!m->is_virtual && c->rst_out[0]
				&& !strcmp(m->wlr_output->name, c->rst_out)) {
			home = m;
			break;
		}
	}
	if (!home)
		home = parked_mons[0];
	if (!home)
		return NULL;

	i = 0;
	wl_list_for_each(ws, &home->workspaces, link) {
		if (i++ == c->rst_ws)
			return ws;
	}
	return workspace_create(home);
}

/* A column with the slot it must land in, keyed off its first client */
typedef struct {
	Column *col;
	Client *lead;
} ColSlot;

static int
slot_before(const ColSlot *a, const ColSlot *b)
{
	int d = strcmp(a->lead->rst_out, b->lead->rst_out);

	if (d != 0)
		return d < 0;
	if (a->lead->rst_ws != b->lead->rst_ws)
		return a->lead->rst_ws < b->lead->rst_ws;
	return a->lead->rst_col < b->lead->rst_col;
}

static void
restore_columns(void)
{
	ColSlot slots[MAX_MONITORS * 64];
	Monitor *m;
	Workspace *ws, *wstmp;
	Column *col, *coltmp;
	int n = 0, i, j;

	wl_list_for_each(m, &mons, link) {
		if (!m->is_virtual)
			continue;
		wl_list_for_each_safe(ws, wstmp, &m->workspaces, link) {
			wl_list_for_each_safe(col, coltmp, &ws->columns, link) {
				Client *lead;

				if (wl_list_empty(&col->clients)
						|| n == (int)LENGTH(slots))
					continue;
				lead = wl_container_of(col->clients.next, lead, column_link);
				slots[n].col = col;
				slots[n].lead = lead;
				n++;
			}
		}
	}

	/* adopt_column appends, so replay in original slot order */
	for (i = 1; i < n; i++) {
		ColSlot key = slots[i];
		for (j = i - 1; j >= 0 && slot_before(&key, &slots[j]); j--)
			slots[j + 1] = slots[j];
		slots[j + 1] = key;
	}

	for (i = 0; i < n; i++) {
		Workspace *dst = restore_target(slots[i].lead);
		if (dst)
			workspace_adopt_column(dst, slots[i].col);
	}
}

/* Drop the empty workspaces a session created on a virtual output */
static void
purge_virtual_workspaces(Monitor *m)
{
	Workspace *ws, *tmp;

	wl_list_for_each_safe(ws, tmp, &m->workspaces, link) {
		if (m->n_workspaces <= 1)
			break;
		if (!workspace_has_clients(ws)) {
			if (m->active_ws == ws)
				m->active_ws = NULL;
			if (m->prev_ws == ws)
				m->prev_ws = NULL;
			workspace_destroy(ws);
		}
	}
	if (!m->active_ws && !wl_list_empty(&m->workspaces))
		m->active_ws = wl_container_of(m->workspaces.next, m->active_ws, link);
	if (wl_list_empty(&m->workspaces))
		m->active_ws = workspace_create(m);
}

const char *
remote_stop(int output)
{
	Monitor *m, *home = NULL;
	int i;

	if (active_sessions == 0)
		return NULL;
	if (!(m = virt_mon(output)))
		return "no such virtual output";

	if (--active_sessions > 0) {
		park_output(m);
		return NULL;
	}

	/* Rebuild the layout while the screens are still dark */
	restore_columns();

	for (i = 0; i < n_parked_mons; i++)
		monitor_set_power(parked_mons[i], 1);
	n_parked_mons = 0;

	wl_list_for_each(m, &mons, link) {
		if (m->is_virtual) {
			purge_virtual_workspaces(m);
			park_output(m);
		}
	}

	wl_list_for_each(m, &mons, link) {
		if (!m->is_virtual && prev_selmon[0]
				&& !strcmp(m->wlr_output->name, prev_selmon)) {
			home = m;
			break;
		}
	}
	if (!home) {
		wl_list_for_each(m, &mons, link) {
			if (!m->is_virtual && m->wlr_output->enabled) {
				home = m;
				break;
			}
		}
	}
	selmon = home;

	clear_records();

	wl_list_for_each(m, &mons, link)
		arrange(m);
	focusclient(focustop(selmon), 1);
	printstatus();

	wlr_log(WLR_INFO, "remote: stopped, desktop restored");
	return NULL;
}

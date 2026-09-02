/* monitors_conf.c — ~/.local/nixlyos/monitors.conf hot-reload.
 *
 * The file is written by nixlycc (Monitors page).  Format, one line per
 * monitor:
 *
 *   monitor = DP-1 grid=0,0 2560x1440@144 [scale=1.25]
 *             [transform=rotate-90|rotate-180|rotate-270]
 *
 * Entries here take priority over `monitor` nodes in config.kdl (see
 * find_monitor_config in nixlytile.c).  An inotify watch on the
 * ~/.local/nixlyos directory reapplies mode/transform and grid layout
 * immediately whenever nixlycc rewrites the file.
 */

#include "nixlytile.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define MONCONF_FILENAME "monitors.conf"

static void
monconf_resolve_paths(char *dir, size_t dircap, char *file, size_t filecap)
{
	const char *home = getenv("HOME");
	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw)
			home = pw->pw_dir;
	}
	if (!home)
		home = "/";
	snprintf(dir, dircap, "%s/.local/nixlyos", home);
	snprintf(file, filecap, "%s/.local/nixlyos/" MONCONF_FILENAME, home);
}

/* Parse the file into monconf_monitors[].  Returns entry count, or -1 if
 * the file could not be read (previous entries are kept in that case). */
int
load_monitors_conf(void)
{
	char dir[PATH_MAX];
	char line[512];
	FILE *fp;

	if (!monconf_path_cached[0])
		monconf_resolve_paths(dir, sizeof(dir),
			monconf_path_cached, sizeof(monconf_path_cached));

	fp = fopen(monconf_path_cached, "r");
	if (!fp)
		return -1;

	monconf_monitor_count = 0;

	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		char *tok;
		RuntimeMonitorConfig *m;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;
		if (strncmp(p, "monitor", 7) != 0)
			continue;
		p += 7;
		while (*p == ' ' || *p == '\t' || *p == '=')
			p++;

		if (monconf_monitor_count >= MAX_MONITORS)
			break;
		m = &monconf_monitors[monconf_monitor_count];
		memset(m, 0, sizeof(*m));
		m->enabled = 1;
		m->scale = 1.0f;
		m->mfact = 0.55f;
		m->nmaster = 1;
		m->transform = WL_OUTPUT_TRANSFORM_NORMAL;
		m->position = MON_POS_AUTO;
		m->grid_col = -1;
		m->grid_row = -1;

		/* First token: connector name */
		tok = strtok(p, " \t\n");
		if (!tok)
			continue;
		snprintf(m->name, sizeof(m->name), "%s", tok);

		while ((tok = strtok(NULL, " \t\n"))) {
			int a, b;
			float hz;
			if (sscanf(tok, "grid=%d,%d", &a, &b) == 2) {
				m->grid_col = a;
				m->grid_row = b;
			} else if (sscanf(tok, "%dx%d@%f", &a, &b, &hz) == 3) {
				m->width = a;
				m->height = b;
				m->refresh = hz;
			} else if (sscanf(tok, "%dx%d", &a, &b) == 2) {
				m->width = a;
				m->height = b;
			} else if (strcmp(tok, "transform=rotate-90") == 0) {
				m->transform = WL_OUTPUT_TRANSFORM_90;
			} else if (strcmp(tok, "transform=rotate-180") == 0) {
				m->transform = WL_OUTPUT_TRANSFORM_180;
			} else if (strcmp(tok, "transform=rotate-270") == 0) {
				m->transform = WL_OUTPUT_TRANSFORM_270;
			} else if (sscanf(tok, "scale=%f", &hz) == 1) {
				if (hz >= 0.5f && hz <= 4.0f)
					m->scale = hz;
			} else if (strncmp(tok, "mirror=", 7) == 0) {
				snprintf(m->mirror, sizeof(m->mirror), "%s",
					tok + 7);
			}
		}

		monconf_monitor_count++;
	}
	fclose(fp);

	wlr_log(WLR_INFO, "monitors.conf: loaded %d entries from %s",
		monconf_monitor_count, monconf_path_cached);
	return monconf_monitor_count;
}

/* Exact-name lookup in the monitors.conf table (no wildcards — nixlycc
 * always writes full connector names). */
RuntimeMonitorConfig *
monconf_find(const char *name)
{
	int i;
	for (i = 0; i < monconf_monitor_count; i++)
		if (strcmp(name, monconf_monitors[i].name) == 0)
			return &monconf_monitors[i];
	return NULL;
}

/* One test+commit attempt with the cfg's transform/scale plus either a
 * custom modeline (custom_mhz > 0) or a listed mode. */
static int
monconf_commit_mode(Monitor *m, RuntimeMonitorConfig *cfg,
		struct wlr_output_mode *mode, int custom_mhz)
{
	struct wlr_output_state st;
	int ok;

	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, 1);
	wlr_output_state_set_transform(&st, cfg->transform);
	if (cfg->scale >= 0.5f && cfg->scale <= 4.0f)
		wlr_output_state_set_scale(&st, cfg->scale);
	if (custom_mhz > 0)
		wlr_output_state_set_custom_mode(&st, cfg->width, cfg->height,
			custom_mhz);
	else if (mode)
		wlr_output_state_set_mode(&st, mode);
	ok = wlr_output_test_state(m->wlr_output, &st) &&
		wlr_output_commit_state(m->wlr_output, &st);
	wlr_output_state_finish(&st);
	return ok;
}

/* Commit mode/transform for every monitor that has a monitors.conf entry. */
static void
monconf_apply_modes(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		RuntimeMonitorConfig *cfg;
		struct wlr_output_mode *mode;

		if (!m->wlr_output)
			continue;
		cfg = monconf_find(m->wlr_output->name);
		if (!cfg)
			continue;

		if (cfg->width > 0 && cfg->height > 0)
			mode = find_mode(m->wlr_output, cfg->width, cfg->height,
				cfg->refresh);
		else
			mode = bestmode(m->wlr_output);
		/* Requested rate isn't in the EDID list (120 Hz cap on a
		 * faster panel, or an overclock on a slow one): commit it as
		 * a custom modeline — clients see it like any other mode.
		 * If the test or commit fails, fall straight back to the
		 * highest standard rate at that resolution. */
		if (cfg->width > 0 && cfg->height > 0 && cfg->refresh > 0 &&
				(!mode || fabsf(mode->refresh / 1000.0f -
					cfg->refresh) > 1.0f)) {
			if (monconf_commit_mode(m, cfg, NULL,
					(int)(cfg->refresh * 1000.0f + 0.5f))) {
				wlr_log(WLR_INFO,
					"monitors.conf: %s custom mode %dx%d@%.3f",
					m->wlr_output->name, cfg->width,
					cfg->height, cfg->refresh);
				continue;
			}
			mode = find_mode(m->wlr_output, cfg->width,
					cfg->height, 0);
			wlr_log(WLR_INFO,
				"monitors.conf: %s custom %.3f Hz failed, "
				"falling back to %d mHz",
				m->wlr_output->name, cfg->refresh,
				mode ? mode->refresh : 0);
		}
		monconf_commit_mode(m, cfg, mode, 0);
	}
}

/* Position monitors from their grid=C,R cells.  Column widths / row
 * heights are the max effective size in that column/row; each monitor is
 * centered inside its cell.  Monitors without a grid entry are left
 * untouched.  Emits the output-layout change signal so updatemons()
 * rearranges clients. */
void
monconf_apply_layout(void)
{
	int col_w[MAX_MONITORS] = {0}, row_h[MAX_MONITORS] = {0};
	int col_x[MAX_MONITORS], row_y[MAX_MONITORS];
	int max_col = -1, max_row = -1;
	int placed = 0;
	Monitor *m;

	if (monconf_monitor_count == 0)
		return;

	wl_list_for_each(m, &mons, link) {
		RuntimeMonitorConfig *cfg;
		int w, h;
		if (!m->wlr_output || !m->wlr_output->enabled)
			continue;
		cfg = monconf_find(m->wlr_output->name);
		if (!cfg || cfg->grid_col < 0 || cfg->grid_row < 0 ||
		    cfg->grid_col >= MAX_MONITORS || cfg->grid_row >= MAX_MONITORS)
			continue;
		if (cfg->mirror[0])
			continue; /* sits on top of its source, owns no cell */
		monitor_effective_size(m, &w, &h);
		if (w > col_w[cfg->grid_col])
			col_w[cfg->grid_col] = w;
		if (h > row_h[cfg->grid_row])
			row_h[cfg->grid_row] = h;
		if (cfg->grid_col > max_col)
			max_col = cfg->grid_col;
		if (cfg->grid_row > max_row)
			max_row = cfg->grid_row;
	}
	if (max_col < 0)
		return;

	col_x[0] = 0;
	for (int c = 1; c <= max_col; c++)
		col_x[c] = col_x[c - 1] + col_w[c - 1];
	row_y[0] = 0;
	for (int r = 1; r <= max_row; r++)
		row_y[r] = row_y[r - 1] + row_h[r - 1];

	wl_list_for_each(m, &mons, link) {
		RuntimeMonitorConfig *cfg;
		int w, h, x, y;
		if (!m->wlr_output || !m->wlr_output->enabled)
			continue;
		cfg = monconf_find(m->wlr_output->name);
		if (!cfg || cfg->grid_col < 0 || cfg->grid_row < 0 ||
		    cfg->grid_col >= MAX_MONITORS || cfg->grid_row >= MAX_MONITORS)
			continue;
		if (cfg->mirror[0])
			continue;
		monitor_effective_size(m, &w, &h);
		x = col_x[cfg->grid_col] + (col_w[cfg->grid_col] - w) / 2;
		y = row_y[cfg->grid_row] + (row_h[cfg->grid_row] - h) / 2;
		wlr_output_layout_add(output_layout, m->wlr_output, x, y);
		wlr_log(WLR_INFO, "monitors.conf: %s grid=%d,%d → (%d,%d) [%dx%d]",
			m->wlr_output->name, cfg->grid_col, cfg->grid_row, x, y, w, h);
		placed++;
	}

	/* Mirrors go last: they take the position their source just got, and
	 * two outputs on the same layout box scan out the same picture. */
	wl_list_for_each(m, &mons, link) {
		RuntimeMonitorConfig *cfg;
		struct wlr_output_layout_output *lo;
		Monitor *src = NULL, *cand;

		if (!m->wlr_output || !m->wlr_output->enabled)
			continue;
		cfg = monconf_find(m->wlr_output->name);
		if (!cfg || !cfg->mirror[0])
			continue;

		wl_list_for_each(cand, &mons, link) {
			if (cand->wlr_output && cand->wlr_output->enabled &&
			    strcmp(cand->wlr_output->name, cfg->mirror) == 0) {
				src = cand;
				break;
			}
		}
		if (!src)
			continue;
		lo = wlr_output_layout_get(output_layout, src->wlr_output);
		if (!lo)
			continue;

		wlr_output_layout_add(output_layout, m->wlr_output, lo->x, lo->y);
		wlr_log(WLR_INFO, "monitors.conf: %s mirrors %s → (%d,%d)",
			m->wlr_output->name, cfg->mirror, lo->x, lo->y);
		placed++;
	}

	if (placed)
		wl_signal_emit_mutable(&output_layout->events.change, output_layout);
}

/* Full hot-reload: re-parse the file, recommit modes, reposition. */
void
reload_monitors_conf(void)
{
	if (load_monitors_conf() < 0)
		return; /* file missing/unreadable — keep current state */
	monconf_apply_modes();
	monconf_apply_layout();
	monitor_overlay_update(); /* screens moved — the ID boxes follow */
}

/* ── inotify watch ────────────────────────────────────────────────── */

static int
monconf_watch_cb(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len;
	int relevant = 0;
	(void)mask;
	(void)data;

	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		char *p = buf;
		while (p < buf + len) {
			struct inotify_event *ev = (struct inotify_event *)p;
			if (ev->len && strcmp(ev->name, MONCONF_FILENAME) == 0)
				relevant = 1;
			p += sizeof(*ev) + ev->len;
		}
	}

	if (relevant)
		reload_monitors_conf();
	return 0;
}

/* Watch the ~/.local/nixlyos directory (not the file itself: nixlycc may
 * replace the file via rename, and it may not exist yet at startup). */
void
setup_monitors_conf_watch(void)
{
	char dir[PATH_MAX];
	char file[PATH_MAX];

	monconf_resolve_paths(dir, sizeof(dir), file, sizeof(file));
	if (!monconf_path_cached[0])
		snprintf(monconf_path_cached, sizeof(monconf_path_cached), "%s", file);

	/* Ensure the directory exists so the watch can be placed. */
	mkdir(dir, 0755);

	monconf_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (monconf_inotify_fd < 0) {
		wlr_log(WLR_ERROR, "monitors.conf: inotify_init failed: %s",
			strerror(errno));
		return;
	}

	monconf_watch_wd = inotify_add_watch(monconf_inotify_fd, dir,
		IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
	if (monconf_watch_wd < 0) {
		wlr_log(WLR_ERROR, "monitors.conf: inotify_add_watch(%s) failed: %s",
			dir, strerror(errno));
		close(monconf_inotify_fd);
		monconf_inotify_fd = -1;
		return;
	}

	monconf_watch_source = wl_event_loop_add_fd(event_loop,
		monconf_inotify_fd, WL_EVENT_READABLE, monconf_watch_cb, NULL);
	wlr_log(WLR_INFO, "monitors.conf: watching %s", dir);
}

/* launcher.c - Auto-extracted from nixlytile.c */
#include "nixlytile.h"
#include "client.h"

void
modal_hide(Monitor *m)
{
	if (!m || !m->modal.tree)
		return;
	modal_file_search_stop(m);
	modal_git_search_stop(m);
	m->modal.visible = 0;
	wlr_scene_node_set_enabled(&m->modal.tree->node, 0);
}

void
modal_hide_all(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link)
		modal_hide(m);
}

void
modal_show(const Arg *arg)
{
	Monitor *m = selmon ? selmon : xytomon(cursor->x, cursor->y);
	Monitor *vm = modal_visible_monitor();
	ModalOverlay *mo;
	int mw, mh, w, h, x, y;
	int min_w = 300, min_h = 200;
	int max_w = 1400, max_h = 800;
	int cycling = 0;

	(void)arg;

	if (!m || !m->modal.tree)
		return;

	if (vm && vm != m)
		modal_hide_all();
	else if (vm == m)
		cycling = 1;

	mw = m->w.width > 0 ? m->w.width : m->m.width;
	mh = m->w.height > 0 ? m->w.height : m->m.height;
	w = mw > 0 ? (int)(mw * 0.8) : 1100;
	h = mh > 0 ? (int)(mh * 0.6) : 500;
	if (w < min_w)
		w = min_w;
	if (h < min_h)
		h = min_h;
	if (w > max_w)
		w = max_w;
	if (h > max_h)
		h = max_h;

	x = m->m.x + (m->m.width - w) / 2;
	y = m->m.y + (m->m.height - h) / 2;
	if (x < m->m.x)
		x = m->m.x;
	if (y < m->m.y)
		y = m->m.y;

	mo = &m->modal;
	mo->width = w;
	mo->height = h;
	mo->x = x;
	mo->y = y;

	if (!mo->bg)
		mo->bg = wlr_scene_tree_create(mo->tree);
	if (mo->bg) {
		struct wlr_scene_node *node, *tmp;
		wl_list_for_each_safe(node, tmp, &mo->bg->children, link)
			wlr_scene_node_destroy(node);
		drawrect(mo->bg, 0, 0, w, h, statusbar_popup_bg);
	}

	wlr_scene_node_set_position(&mo->tree->node, x, y);
	wlr_scene_node_set_enabled(&mo->tree->node, 1);
	if (cycling)
		mo->active_idx = (mo->active_idx + 1) % 3;
	else if (mo->active_idx < 0 || mo->active_idx > 2)
		mo->active_idx = 0;
	else
		mo->active_idx = 0;
	modal_file_search_stop(m);
	modal_file_search_clear_results(m);
	modal_git_search_stop(m);
	modal_git_search_clear_results(m);
	mo->file_search_last[0] = '\0';
	mo->git_search_last[0] = '\0';
	mo->git_search_done = 0;
	for (int i = 0; i < 3; i++) {
		mo->search[i][0] = '\0';
		mo->search_len[i] = 0;
		mo->search_rendered[i][0] = '\0';
		mo->selected[i] = -1;
		mo->scroll[i] = 0;
	}
	mo->search_field_tree = NULL; /* will be recreated on render */
	mo->render_pending = 0;
	if (mo->render_timer)
		wl_event_source_timer_update(mo->render_timer, 0);
	mo->file_search_fallback = 0;
	mo->visible = 1;
	modal_update_results(m);
	modal_render(m);
}

void
modal_show_files(const Arg *arg)
{
	Monitor *m = selmon ? selmon : xytomon(cursor->x, cursor->y);
	ModalOverlay *mo;
	int mw, mh, w, h, x, y;
	int min_w = 300, min_h = 200;
	int max_w = 1400, max_h = 800;

	(void)arg;

	if (!m || !m->modal.tree)
		return;

	modal_hide_all();

	mw = m->w.width > 0 ? m->w.width : m->m.width;
	mh = m->w.height > 0 ? m->w.height : m->m.height;
	w = mw > 0 ? (int)(mw * 0.8) : 1100;
	h = mh > 0 ? (int)(mh * 0.6) : 500;
	if (w < min_w) w = min_w;
	if (h < min_h) h = min_h;
	if (w > max_w) w = max_w;
	if (h > max_h) h = max_h;

	x = m->m.x + (m->m.width - w) / 2;
	y = m->m.y + (m->m.height - h) / 2;
	if (x < m->m.x) x = m->m.x;
	if (y < m->m.y) y = m->m.y;

	mo = &m->modal;
	mo->width = w;
	mo->height = h;
	mo->x = x;
	mo->y = y;

	if (!mo->bg)
		mo->bg = wlr_scene_tree_create(mo->tree);
	if (mo->bg) {
		struct wlr_scene_node *node, *tmp;
		wl_list_for_each_safe(node, tmp, &mo->bg->children, link)
			wlr_scene_node_destroy(node);
		drawrect(mo->bg, 0, 0, w, h, statusbar_popup_bg);
	}

	wlr_scene_node_set_position(&mo->tree->node, x, y);
	wlr_scene_node_set_enabled(&mo->tree->node, 1);
	mo->active_idx = 1; /* File search tab */
	modal_file_search_stop(m);
	modal_file_search_clear_results(m);
	modal_git_search_stop(m);
	modal_git_search_clear_results(m);
	mo->file_search_last[0] = '\0';
	mo->git_search_last[0] = '\0';
	mo->git_search_done = 0;
	for (int i = 0; i < 3; i++) {
		mo->search[i][0] = '\0';
		mo->search_len[i] = 0;
		mo->search_rendered[i][0] = '\0';
		mo->selected[i] = -1;
		mo->scroll[i] = 0;
	}
	mo->search_field_tree = NULL;
	mo->render_pending = 0;
	if (mo->render_timer)
		wl_event_source_timer_update(mo->render_timer, 0);
	mo->file_search_fallback = 0;
	mo->visible = 1;
	modal_update_results(m);
	modal_render(m);
}

void
modal_show_git(const Arg *arg)
{
	Monitor *m = selmon ? selmon : xytomon(cursor->x, cursor->y);
	ModalOverlay *mo;
	int mw, mh, w, h, x, y;
	int min_w = 300, min_h = 200;
	int max_w = 1400, max_h = 800;

	(void)arg;

	if (!m || !m->modal.tree)
		return;

	modal_hide_all();

	mw = m->w.width > 0 ? m->w.width : m->m.width;
	mh = m->w.height > 0 ? m->w.height : m->m.height;
	w = mw > 0 ? (int)(mw * 0.8) : 1100;
	h = mh > 0 ? (int)(mh * 0.6) : 500;
	if (w < min_w) w = min_w;
	if (h < min_h) h = min_h;
	if (w > max_w) w = max_w;
	if (h > max_h) h = max_h;

	x = m->m.x + (m->m.width - w) / 2;
	y = m->m.y + (m->m.height - h) / 2;
	if (x < m->m.x) x = m->m.x;
	if (y < m->m.y) y = m->m.y;

	mo = &m->modal;
	mo->width = w;
	mo->height = h;
	mo->x = x;
	mo->y = y;

	if (!mo->bg)
		mo->bg = wlr_scene_tree_create(mo->tree);
	if (mo->bg) {
		struct wlr_scene_node *node, *tmp;
		wl_list_for_each_safe(node, tmp, &mo->bg->children, link)
			wlr_scene_node_destroy(node);
		drawrect(mo->bg, 0, 0, w, h, statusbar_popup_bg);
	}

	wlr_scene_node_set_position(&mo->tree->node, x, y);
	wlr_scene_node_set_enabled(&mo->tree->node, 1);
	mo->active_idx = 2; /* Git projects tab */
	modal_file_search_stop(m);
	modal_file_search_clear_results(m);
	modal_git_search_stop(m);
	modal_git_search_clear_results(m);
	mo->file_search_last[0] = '\0';
	mo->git_search_last[0] = '\0';
	mo->git_search_done = 0;
	for (int i = 0; i < 3; i++) {
		mo->search[i][0] = '\0';
		mo->search_len[i] = 0;
		mo->search_rendered[i][0] = '\0';
		mo->selected[i] = -1;
		mo->scroll[i] = 0;
	}
	mo->search_field_tree = NULL;
	mo->render_pending = 0;
	if (mo->render_timer)
		wl_event_source_timer_update(mo->render_timer, 0);
	mo->file_search_fallback = 0;
	mo->visible = 1;
	modal_git_search_start(m); /* Start git search immediately */
	modal_render(m);
}

Monitor *
modal_visible_monitor(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (m->modal.visible)
			return m;
	}
	return NULL;
}

void
modal_layout_metrics(int *btn_h, int *field_h, int *line_h, int *pad)
{
	int bh = statusfont.height > 0 ? statusfont.height + 16 : 48;
	int fh;
	int lh;
	int pd = 12;

	if (bh < 40)
		bh = 40;
	fh = bh > 40 ? bh - 8 : 36;
	lh = statusfont.height > 0 ? statusfont.height + 6 : 22;

	if (btn_h)
		*btn_h = bh;
	if (field_h)
		*field_h = fh;
	if (line_h)
		*line_h = lh;
	if (pad)
		*pad = pd;
}

int
modal_max_visible_lines(Monitor *m)
{
	ModalOverlay *mo;
	int btn_h, field_h, line_h, pad;
	int line_y, avail;

	if (!m)
		return 0;
	mo = &m->modal;
	modal_layout_metrics(&btn_h, &field_h, &line_h, &pad);

	line_y = btn_h + pad + field_h + pad;
	avail = mo->height - line_y - pad;
	if (avail <= 0 || line_h <= 0)
		return 0;
	return avail / line_h;
}

void
modal_ensure_selection_visible(Monitor *m)
{
	ModalOverlay *mo;
	int active;
	int max_lines;
	int max_scroll;
	int sel;

	if (!m)
		return;
	mo = &m->modal;
	active = mo->active_idx;
	if (active < 0 || active > 2)
		return;

	max_lines = modal_max_visible_lines(m);
	if (max_lines <= 0) {
		mo->scroll[active] = 0;
		return;
	}
	if (mo->result_count[active] <= max_lines) {
		mo->scroll[active] = 0;
		return;
	}

	max_scroll = mo->result_count[active] - max_lines;
	sel = mo->selected[active];

	if (sel < 0) {
		if (mo->scroll[active] > max_scroll)
			mo->scroll[active] = max_scroll;
		if (mo->scroll[active] < 0)
			mo->scroll[active] = 0;
		return;
	}

	if (sel < mo->scroll[active])
		mo->scroll[active] = sel;
	else if (sel >= mo->scroll[active] + max_lines)
		mo->scroll[active] = sel - max_lines + 1;

	if (mo->scroll[active] > max_scroll)
		mo->scroll[active] = max_scroll;
	if (mo->scroll[active] < 0)
		mo->scroll[active] = 0;
}

void
modal_file_search_clear_results(Monitor *m)
{
	ModalOverlay *mo;

	if (!m)
		return;
	mo = &m->modal;
		mo->result_count[1] = 0;
		for (int i = 0; i < (int)LENGTH(mo->results[1]); i++) {
			mo->results[1][i][0] = '\0';
			mo->result_entry_idx[1][i] = -1;
			mo->file_results_name[i][0] = '\0';
			mo->file_results_path[i][0] = '\0';
			mo->file_results_mtime[i] = 0;
	}
	mo->selected[1] = -1;
	mo->scroll[1] = 0;
	mo->file_search_fallback = 0;
}

int
ci_contains(const char *hay, const char *needle)
{
	size_t hlen, nlen;

	if (!needle || !*needle)
		return 1;
	if (!hay || !*hay)
		return 0;
	hlen = strlen(hay);
	nlen = strlen(needle);
	if (nlen > hlen)
		return 0;
	for (size_t i = 0; i + nlen <= hlen; i++) {
		size_t j = 0;
		for (; j < nlen; j++) {
			char a = (char)tolower((unsigned char)hay[i + j]);
			char b = (char)tolower((unsigned char)needle[j]);
			if (a != b)
				break;
		}
		if (j == nlen)
			return 1;
	}
	return 0;
}

int
modal_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym)
{
	ModalOverlay *mo;

	if (!m)
		return 0;
	mo = &m->modal;

	if (sym == XKB_KEY_Escape) {
		modal_hide_all();
		return 1;
	}

	/* Mod+E: Open all file search results in Thunar via symlinks */
	if ((sym == XKB_KEY_e || sym == XKB_KEY_E) && (mods & WLR_MODIFIER_LOGO) &&
			mo->active_idx == 1 && mo->result_count[1] > 0) {
		const char *base = "/tmp/nixlytile-search-results";
		DIR *d;
		struct dirent *ent;
		char entpath[PATH_MAX];
		int i;

		/* Remove old temp dir contents (symlinks only) */
		d = opendir(base);
		if (d) {
			while ((ent = readdir(d)) != NULL) {
				if (ent->d_name[0] == '.')
					continue;
				snprintf(entpath, sizeof(entpath), "%s/%s", base, ent->d_name);
				unlink(entpath);
			}
			closedir(d);
			rmdir(base);
		}
		mkdir(base, 0755);
		{
			/* Create symlinks for all results */
			for (i = 0; i < mo->result_count[1]; i++) {
				const char *path = mo->file_results_path[i];
				const char *name = mo->file_results_name[i];
				char linkpath[PATH_MAX];
				char safename[256];
				int j, k;

				if (!path || !*path || !name || !*name)
					continue;

				/* Sanitize filename: replace / with _ */
				for (j = 0, k = 0; name[j] && k < (int)sizeof(safename) - 1; j++) {
					if (name[j] == '/')
						safename[k++] = '_';
					else
						safename[k++] = name[j];
				}
				safename[k] = '\0';

				/* Add index prefix to preserve order and handle duplicates */
				snprintf(linkpath, sizeof(linkpath), "%s/%03d_%s", base, i + 1, safename);
				symlink(path, linkpath);
			}

			/* Open in Thunar */
			char *cmd[] = { "thunar", (char *)base, NULL };
			Arg arg = { .v = cmd };
			spawn(&arg);
			modal_hide_all();
		}
		return 1;
	}

	if (sym == XKB_KEY_BackSpace && mo->search_len[mo->active_idx] > 0 && mods == 0) {
		mo->search_len[mo->active_idx]--;
		mo->search[mo->active_idx][mo->search_len[mo->active_idx]] = '\0';
		return 1;
	}
	if (sym == XKB_KEY_Down && mo->result_count[mo->active_idx] > 0) {
		int sel = mo->selected[mo->active_idx];
		if (sel < 0)
			sel = 0;
		else if (sel + 1 < mo->result_count[mo->active_idx])
			sel++;
		else
			sel = 0; /* wrap to top */
		mo->selected[mo->active_idx] = sel;
		modal_ensure_selection_visible(m);
		return 1;
	}
	if (sym == XKB_KEY_Up && mo->result_count[mo->active_idx] > 0) {
		int sel = mo->selected[mo->active_idx];
		if (sel < 0)
			sel = mo->result_count[mo->active_idx] - 1;
		else if (sel > 0)
			sel--;
		else
			sel = mo->result_count[mo->active_idx] - 1; /* wrap to bottom */
		mo->selected[mo->active_idx] = sel;
		modal_ensure_selection_visible(m);
		return 1;
	}
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		int sel = mo->selected[mo->active_idx];
		if (mo->active_idx == 0 && sel >= 0 && sel < mo->result_count[0]) {
			int idx = mo->result_entry_idx[0][sel];
			if (idx >= 0 && idx < desktop_entry_count) {
				char cmd_str[512];
				desktop_entries[idx].used++;
				snprintf(cmd_str, sizeof(cmd_str), "%s", desktop_entries[idx].exec[0]
						? desktop_entries[idx].exec : desktop_entries[idx].name);
				/* Launch with dGPU if desktop entry prefers it or command matches dgpu_programs */
				pid_t pid = fork();
				if (pid == 0) {
					setsid();
					if (desktop_entries[idx].prefers_dgpu || should_use_dgpu(cmd_str))
						set_dgpu_env();
					if (is_steam_cmd(cmd_str)) {
						set_steam_env();
						/* Launch Steam - Big Picture in HTPC mode, normal otherwise */
						if (htpc_mode_active) {
							execlp("steam", "steam", "-bigpicture", "-cef-force-gpu", "-cef-disable-sandbox", "steam://open/games", (char *)NULL);
						} else {
							execlp("steam", "steam", "-cef-force-gpu", "-cef-disable-sandbox", (char *)NULL);
						}
					}
					execl("/bin/sh", "sh", "-c", cmd_str, NULL);
					_exit(127);
				}
				modal_hide_all();
			}
		} else if (mo->active_idx == 1 && sel >= 0 && sel < mo->result_count[1]) {
			const char *path = mo->file_results_path[sel];
			if (path && *path) {
				char *cmd[] = { "xdg-open", (char *)path, NULL };
				Arg arg = { .v = cmd };
				spawn(&arg);
				modal_hide_all();
			}
		} else if (mo->active_idx == 2 && sel >= 0 && sel < mo->result_count[2]) {
			const char *path = mo->git_results_path[sel];
			if (path && *path) {
				char cmd_str[PATH_MAX + 64];
				char *cmd[4] = { "sh", "-c", cmd_str, NULL };
				Arg arg;
				/* Open terminal in the git project directory */
				snprintf(cmd_str, sizeof(cmd_str), "cd '%s' && alacritty", path);
				arg.v = cmd;
				spawn(&arg);
				modal_hide_all();
			}
		}
		return 1;
	}
	/* Allow Ctrl+Alt (AltGr) combinations - on many systems AltGr is Ctrl+Alt
	 * Only block Ctrl without Alt (real Ctrl shortcuts) and Logo key */
	{
		int is_altgr = (mods & WLR_MODIFIER_CTRL) && (mods & WLR_MODIFIER_ALT);
		int is_ctrl_only = (mods & WLR_MODIFIER_CTRL) && !is_altgr;
		if (sym >= 0x20 && sym <= 0x7e && !is_ctrl_only && !(mods & WLR_MODIFIER_LOGO)) {
			int len = mo->search_len[mo->active_idx];
			if (len + 1 < (int)sizeof(mo->search[0])) {
				mo->search[mo->active_idx][len] = (char)sym;
				mo->search[mo->active_idx][len + 1] = '\0';
				mo->search_len[mo->active_idx] = len + 1;
			}
			return 1;
		}
	}

	return 0;
}

void
modal_file_search_stop(Monitor *m)
{
	ModalOverlay *mo;

	if (!m)
		return;
	mo = &m->modal;

	if (mo->file_search_timer)
		wl_event_source_timer_update(mo->file_search_timer, 0);
	if (mo->file_search_event) {
		wl_event_source_remove(mo->file_search_event);
		mo->file_search_event = NULL;
	}
	if (mo->file_search_fd >= 0) {
		close(mo->file_search_fd);
		mo->file_search_fd = -1;
	}
	if (mo->file_search_pid > 0) {
		wlr_log(WLR_INFO, "modal file search stop: killing pid=%d", mo->file_search_pid);
		kill(mo->file_search_pid, SIGTERM);
		waitpid(mo->file_search_pid, NULL, WNOHANG);
		mo->file_search_pid = -1;
	}
	mo->file_search_len = 0;
	mo->file_search_buf[0] = '\0';
	if (mo->file_search_timer) {
		wl_event_source_remove(mo->file_search_timer);
		mo->file_search_timer = NULL;
	}
}

void
shorten_path_display(const char *full, char *out, size_t len)
{
	const char *home = getenv("HOME");
	const char *p = full;
	size_t flen;
	size_t keep;
	const char *tail;

	if (!out || len == 0)
		return;
	out[0] = '\0';
	if (!p)
		return;

	if (home && *home && strncmp(full, home, strlen(home)) == 0 && strlen(home) < strlen(full)) {
		static char tmp[PATH_MAX];
		size_t hlen = strlen(home);
		size_t rem = strlen(full) - hlen;
		if (hlen + 1 + rem < sizeof(tmp)) {
			tmp[0] = '~';
			memcpy(tmp + 1, full + hlen, rem + 1);
			p = tmp;
		}
	}

	flen = strlen(p);
	if (flen < len) {
		snprintf(out, len, "%s", p);
		return;
	}
	if (len < 5) {
		snprintf(out, len, "%s", p + (flen - (len - 1)));
		return;
	}

	keep = (len - 4) / 2;
	tail = p + flen - (len - keep - 4);
	if (tail < p)
		tail = p;
	snprintf(out, len, "%.*s...%s", (int)keep, p, tail);
}

void
modal_file_search_add_line(Monitor *m, const char *line)
{
	ModalOverlay *mo;
	int idx;
	const char *tab1;
	const char *tab2;
	size_t name_len, path_len;
	char name[128];
	char path[PATH_MAX];
	char dir[PATH_MAX];
	char when[32] = {0};
	char path_disp[128] = {0};
	double epoch = 0.0;
	time_t t;
	struct tm tm;
	char needle[256];

	if (!m || !line || !*line)
		return;
	mo = &m->modal;
	if (mo->result_count[1] >= (int)LENGTH(mo->results[1]))
		return;

	tab1 = strchr(line, '\t');
	if (!tab1)
		return;
	tab2 = strchr(tab1 + 1, '\t');
	if (!tab2)
		return;

	name_len = (size_t)(tab1 - line);
	path_len = (size_t)(tab2 - (tab1 + 1));
	if (name_len == 0 || path_len == 0)
		return;
	if (name_len >= sizeof(name))
		name_len = sizeof(name) - 1;
	if (path_len >= sizeof(path))
		path_len = sizeof(path) - 1;

	snprintf(name, sizeof(name), "%.*s", (int)name_len, line);
	snprintf(path, sizeof(path), "%.*s", (int)path_len, tab1 + 1);
	snprintf(dir, sizeof(dir), "%s", path);
	{
		char *slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = '\0';
		} else if (slash) {
			*(slash + 1) = '\0';
		} else {
			snprintf(dir, sizeof(dir), ".");
		}
	}

	epoch = strtod(tab2 + 1, NULL);
	t = (time_t)epoch;
	if (localtime_r(&t, &tm))
		strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
	shorten_path_display(dir, path_disp, sizeof(path_disp));

	/* Always filter locally to allow any substring/character combination to match */
	const char *q = mo->file_search_last;
	int qlen = q ? (int)strlen(q) : 0;
	int qstart = 0, qend = qlen;
	if (qlen >= (int)sizeof(needle))
		qlen = (int)sizeof(needle) - 1;
	while (qstart < qlen && isspace((unsigned char)q[qstart]))
		qstart++;
	while (qend > qstart && isspace((unsigned char)q[qend - 1]))
		qend--;
	qlen = qend - qstart;
	if (qlen <= 0)
		return;
	for (int i = 0; i < qlen; i++)
		needle[i] = (char)q[qstart + i];
	needle[qlen] = '\0';
	if (!ci_contains(name, needle) && !ci_contains(path, needle))
		return;

	wlr_log(WLR_INFO, "modal file search result: name='%s' dir='%s'", name, path_disp[0] ? path_disp : dir);

	idx = mo->result_count[1];
	snprintf(mo->results[1][idx], sizeof(mo->results[1][idx]),
			"%s  %s", name, path_disp[0] ? path_disp : dir);
	snprintf(mo->file_results_name[idx], sizeof(mo->file_results_name[idx]), "%s", name);
	snprintf(mo->file_results_path[idx], sizeof(mo->file_results_path[idx]), "%s", path);
	mo->file_results_mtime[idx] = t;
	mo->result_entry_idx[1][idx] = -1;
	mo->result_count[1]++;
	if (mo->selected[1] < 0)
		mo->selected[1] = idx;
	if (mo->result_count[1] >= (int)LENGTH(mo->results[1]))
		modal_file_search_stop(m);
}

void
modal_file_search_flush_buffer(Monitor *m)
{
	ModalOverlay *mo;
	size_t start = 0;

	if (!m)
		return;
	mo = &m->modal;
	for (size_t i = 0; i < mo->file_search_len; i++) {
		if (mo->file_search_buf[i] == '\n') {
			mo->file_search_buf[i] = '\0';
			modal_file_search_add_line(m, mo->file_search_buf + start);
			if (mo->result_count[1] >= (int)LENGTH(mo->results[1])) {
				modal_file_search_stop(m);
				start = mo->file_search_len;
				break;
			}
			start = i + 1;
		}
	}

	if (start > 0) {
		size_t remaining = mo->file_search_len - start;
		memmove(mo->file_search_buf, mo->file_search_buf + start, remaining);
		mo->file_search_len = remaining;
		mo->file_search_buf[mo->file_search_len] = '\0';
	} else if (mo->file_search_len >= sizeof(mo->file_search_buf) - 1) {
		/* avoid getting stuck with an overfull buffer if no newline arrives */
		mo->file_search_len = 0;
		mo->file_search_buf[0] = '\0';
	}
}

void
modal_file_search_start_mode(Monitor *m, int fallback)
{
	ModalOverlay *mo;
	int pipefd[2] = {-1, -1};
	pid_t pid;

	(void)fallback;

	if (!m)
		return;
	mo = &m->modal;

	modal_file_search_stop(m);
	modal_file_search_clear_results(m);

	if (mo->search_len[1] < modal_file_search_minlen) {
		mo->file_search_last[0] = '\0';
		return;
	}
	mo->file_search_fallback = 1; /* always filter locally */

	if (pipe(pipefd) != 0) {
		wlr_log(WLR_ERROR, "modal file search: pipe failed: %s", strerror(errno));
		mo->file_search_last[0] = '\0';
		return;
	}
	fcntl(pipefd[0], F_SETFD, fcntl(pipefd[0], F_GETFD) | FD_CLOEXEC);
	fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);

	pid = fork();
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		setsid();

		/* Read file search from cache (cache updated at nixlytile startup) */
		execlp("sh", "sh", "-c",
			"cat \"${XDG_CACHE_HOME:-$HOME/.cache}/nixlytile-file-search\" 2>/dev/null",
			(char *)NULL);
		_exit(127);
	} else if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		wlr_log(WLR_ERROR, "modal file search: fork failed: %s", strerror(errno));
		mo->file_search_last[0] = '\0';
		return;
	}

	close(pipefd[1]);
	mo->file_search_pid = pid;
	mo->file_search_fd = pipefd[0];
	fcntl(mo->file_search_fd, F_SETFL, fcntl(mo->file_search_fd, F_GETFL) | O_NONBLOCK);
	mo->file_search_len = 0;
	mo->file_search_buf[0] = '\0';
	mo->file_search_event = wl_event_loop_add_fd(event_loop, mo->file_search_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP, modal_file_search_event, m);
	if (!mo->file_search_event) {
		wlr_log(WLR_ERROR, "modal file search: failed to watch results fd");
		modal_file_search_stop(m);
		mo->file_search_last[0] = '\0';
		return;
	}

	snprintf(mo->file_search_last, sizeof(mo->file_search_last), "%s", mo->search[1]);
	wlr_log(WLR_INFO, "modal file search start: '%s' (len=%d, fd=%d pid=%d event=%p)",
			mo->file_search_last, mo->search_len[1], mo->file_search_fd, mo->file_search_pid, (void *)mo->file_search_event);
}

void
modal_file_search_start(Monitor *m)
{
	modal_file_search_start_mode(m, 1);
}

int
modal_file_search_debounce_cb(void *data)
{
	Monitor *m = data;
	ModalOverlay *mo;

	if (!m)
		return 0;
	mo = &m->modal;

	/* Only start if search string still differs from last search */
	if (mo->search_len[1] >= modal_file_search_minlen &&
	    strncmp(mo->search[1], mo->file_search_last, sizeof(mo->file_search_last)) != 0) {
		modal_file_search_start(m);
	}

	return 0;
}

void
modal_file_search_schedule(Monitor *m)
{
	ModalOverlay *mo;

	if (!m)
		return;
	mo = &m->modal;

	/* Create timer if it doesn't exist */
	if (!mo->file_search_timer) {
		mo->file_search_timer = wl_event_loop_add_timer(event_loop,
				modal_file_search_debounce_cb, m);
	}

	/* Reset timer to 150ms */
	if (mo->file_search_timer)
		wl_event_source_timer_update(mo->file_search_timer, 150);
}

int
modal_file_search_event(int fd, uint32_t mask, void *data)
{
	Monitor *m = data;
	ModalOverlay *mo;
	char buf[512];
	ssize_t n;

	if (!m)
		return 0;
	(void)mask;
	mo = &m->modal;
	if (fd != mo->file_search_fd)
		return 0;

	wlr_log(WLR_INFO, "modal file search event: mask=0x%x pid=%d fd=%d", mask, mo->file_search_pid, fd);

	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			wlr_log(WLR_INFO, "modal file search read %zd bytes (buf_len=%zu)", n, mo->file_search_len);
			if (mo->file_search_len >= sizeof(mo->file_search_buf) - 1)
				modal_file_search_flush_buffer(m);
			if (mo->file_search_len < sizeof(mo->file_search_buf) - 1) {
				size_t avail = sizeof(mo->file_search_buf) - 1 - mo->file_search_len;
				if ((size_t)n > avail)
					n = (ssize_t)avail;
				memcpy(mo->file_search_buf + mo->file_search_len, buf, (size_t)n);
				mo->file_search_len += (size_t)n;
				mo->file_search_buf[mo->file_search_len] = '\0';
			}
			modal_file_search_flush_buffer(m);
			if (mo->file_search_fd < 0 || mo->file_search_pid <= 0)
				break;
		} else if (n == 0) {
			modal_file_search_flush_buffer(m);
			if (mo->file_search_len > 0 && mo->file_search_len < sizeof(mo->file_search_buf)) {
				mo->file_search_buf[mo->file_search_len] = '\0';
				modal_file_search_add_line(m, mo->file_search_buf);
				mo->file_search_len = 0;
				mo->file_search_buf[0] = '\0';
			}
			wlr_log(WLR_INFO, "modal file search done: '%s' (%d results)", mo->file_search_last, mo->result_count[1]);
			modal_file_search_stop(m);
			break;
		} else {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			modal_file_search_stop(m);
			wlr_log(WLR_ERROR, "modal file search read error: %s", strerror(errno));
			break;
		}
	}

	if (mo->result_count[1] > 0 && mo->selected[1] < 0)
		mo->selected[1] = 0;
	if (mo->active_idx == 1 && mo->result_count[1] > 0)
		modal_ensure_selection_visible(m);
	/* Only render if user is not actively typing */
	if (m->modal.visible && !mo->render_pending)
		modal_render(m);
	return 0;
}

void
modal_git_search_clear_results(Monitor *m)
{
	ModalOverlay *mo;

	if (!m)
		return;
	mo = &m->modal;
	mo->git_result_count = 0;
	mo->result_count[2] = 0;
	for (int i = 0; i < MODAL_MAX_RESULTS; i++) {
		mo->git_results_name[i][0] = '\0';
		mo->git_results_path[i][0] = '\0';
		mo->git_results_mtime[i] = 0;
		mo->results[2][i][0] = '\0';
	}
	mo->selected[2] = -1;
	mo->scroll[2] = 0;
}

void
modal_git_search_stop(Monitor *m)
{
	ModalOverlay *mo;

	if (!m)
		return;
	mo = &m->modal;

	if (mo->git_search_event) {
		wl_event_source_remove(mo->git_search_event);
		mo->git_search_event = NULL;
	}
	if (mo->git_search_fd >= 0) {
		close(mo->git_search_fd);
		mo->git_search_fd = -1;
	}
	if (mo->git_search_pid > 0) {
		wlr_log(WLR_INFO, "modal git search stop: killing pid=%d", mo->git_search_pid);
		kill(mo->git_search_pid, SIGTERM);
		waitpid(mo->git_search_pid, NULL, WNOHANG);
		mo->git_search_pid = -1;
	}
	mo->git_search_len = 0;
	mo->git_search_buf[0] = '\0';
}

void
modal_git_search_add_line(Monitor *m, const char *line)
{
	ModalOverlay *mo;
	int idx;
	const char *tab;
	double epoch;
	time_t t;
	char *name;
	char path[PATH_MAX];
	char short_path[128];

	if (!m || !line || !*line)
		return;
	mo = &m->modal;
	if (mo->git_result_count >= MODAL_MAX_RESULTS)
		return;

	/* Format: "mtime_epoch\tpath" */
	tab = strchr(line, '\t');
	if (!tab)
		return;

	epoch = strtod(line, NULL);
	t = (time_t)epoch;
	snprintf(path, sizeof(path), "%s", tab + 1);

	/* Extract project name from path */
	name = strrchr(path, '/');
	if (name)
		name++;
	else
		name = path;

	/* Filter by search query if any */
	if (mo->search_len[2] > 0) {
		char needle[256];
		int nlen = mo->search_len[2];
		if (nlen >= (int)sizeof(needle))
			nlen = (int)sizeof(needle) - 1;
		for (int i = 0; i < nlen; i++)
			needle[i] = (char)tolower((unsigned char)mo->search[2][i]);
		needle[nlen] = '\0';
		if (!ci_contains(name, needle) && !ci_contains(path, needle))
			return;
	}

	idx = mo->git_result_count;
	snprintf(mo->git_results_name[idx], sizeof(mo->git_results_name[idx]), "%s", name);
	snprintf(mo->git_results_path[idx], sizeof(mo->git_results_path[idx]), "%s", path);
	mo->git_results_mtime[idx] = t;

	shorten_path_display(path, short_path, sizeof(short_path));
	snprintf(mo->results[2][idx], sizeof(mo->results[2][idx]), "%s", short_path);
	mo->result_count[2]++;
	mo->git_result_count++;

	if (mo->selected[2] < 0)
		mo->selected[2] = 0;
}

void
modal_git_search_flush_buffer(Monitor *m)
{
	ModalOverlay *mo;
	size_t start = 0;

	if (!m)
		return;
	mo = &m->modal;
	for (size_t i = 0; i < mo->git_search_len; i++) {
		if (mo->git_search_buf[i] == '\n') {
			mo->git_search_buf[i] = '\0';
			modal_git_search_add_line(m, mo->git_search_buf + start);
			if (mo->git_result_count >= MODAL_MAX_RESULTS) {
				modal_git_search_stop(m);
				start = mo->git_search_len;
				break;
			}
			start = i + 1;
		}
	}
	if (start > 0 && start < mo->git_search_len) {
		memmove(mo->git_search_buf, mo->git_search_buf + start, mo->git_search_len - start);
		mo->git_search_len -= start;
	} else if (start == mo->git_search_len) {
		mo->git_search_len = 0;
	}
	mo->git_search_buf[mo->git_search_len] = '\0';
}

int
modal_git_search_event(int fd, uint32_t mask, void *data)
{
	Monitor *m = data;
	ModalOverlay *mo;
	char buf[512];
	ssize_t n;

	if (!m)
		return 0;
	(void)mask;
	mo = &m->modal;
	if (fd != mo->git_search_fd)
		return 0;

	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		size_t space = sizeof(mo->git_search_buf) - mo->git_search_len - 1;
		if ((size_t)n > space)
			n = (ssize_t)space;
		if (n > 0) {
			memcpy(mo->git_search_buf + mo->git_search_len, buf, (size_t)n);
			mo->git_search_len += (size_t)n;
			mo->git_search_buf[mo->git_search_len] = '\0';
		}
		modal_git_search_flush_buffer(m);
		if (mo->git_result_count >= MODAL_MAX_RESULTS)
			break;
	}

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
		mo->git_search_done = 1;
		modal_git_search_stop(m);
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			wlr_log(WLR_ERROR, "modal git search read error: %s", strerror(errno));
	}

	if (mo->result_count[2] > 0 && mo->selected[2] < 0)
		mo->selected[2] = 0;
	if (mo->active_idx == 2 && mo->result_count[2] > 0)
		modal_ensure_selection_visible(m);
	/* Render after receiving git search results */
	if (mo->active_idx == 2)
		modal_render(m);
	return 0;
}

void
modal_git_search_start(Monitor *m)
{
	ModalOverlay *mo;
	pid_t pid;
	int pipefd[2];
	const char *home;

	if (!m)
		return;
	mo = &m->modal;

	modal_git_search_stop(m);
	modal_git_search_clear_results(m);
	mo->git_search_done = 0;

	home = getenv("HOME");
	if (!home || !*home)
		home = "/";

	if (pipe(pipefd) != 0) {
		wlr_log(WLR_ERROR, "modal git search: pipe failed: %s", strerror(errno));
		return;
	}
	fcntl(pipefd[0], F_SETFD, fcntl(pipefd[0], F_GETFD) | FD_CLOEXEC);
	fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);

	pid = fork();
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		setsid();

		/* Read git projects from cache only (cache updated at nixlytile startup) */
		execlp("sh", "sh", "-c",
			"cat \"${XDG_CACHE_HOME:-$HOME/.cache}/nixlytile-git-projects\" 2>/dev/null",
			(char *)NULL);
		_exit(127);
	} else if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		wlr_log(WLR_ERROR, "modal git search: fork failed: %s", strerror(errno));
		return;
	}

	close(pipefd[1]);
	mo->git_search_pid = pid;
	mo->git_search_fd = pipefd[0];
	fcntl(mo->git_search_fd, F_SETFL, fcntl(mo->git_search_fd, F_GETFL) | O_NONBLOCK);
	mo->git_search_len = 0;
	mo->git_search_buf[0] = '\0';
	mo->git_search_event = wl_event_loop_add_fd(event_loop, mo->git_search_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP, modal_git_search_event, m);
	if (!mo->git_search_event) {
		wlr_log(WLR_ERROR, "modal git search: failed to watch results fd");
		modal_git_search_stop(m);
		return;
	}

	/* Save current search query */
	{
		int qlen = mo->search_len[2];
		if (qlen > (int)sizeof(mo->git_search_last) - 1)
			qlen = (int)sizeof(mo->git_search_last) - 1;
		if (qlen > 0)
			memcpy(mo->git_search_last, mo->search[2], (size_t)qlen);
		mo->git_search_last[qlen] = '\0';
	}
	wlr_log(WLR_INFO, "modal git search start: fd=%d pid=%d query='%s'", mo->git_search_fd, mo->git_search_pid, mo->git_search_last);
}

void
git_cache_update_start(void)
{
	pid_t pid;

	if (game_mode_active || htpc_mode_active)
		return;

	pid = fork();
	if (pid == 0) {
		setsid();
		/* Update git projects cache in background with low priority */
		execlp("nice", "nice", "-n", "19", "ionice", "-c", "3", "sh", "-c",
			"cache=\"${XDG_CACHE_HOME:-$HOME/.cache}/nixlytile-git-projects\"; "
			"fd -H -t d '^\\.git$' -E '.local' -E '.config' -E '.cache' -E '.npm' -E '.cargo' -E 'node_modules' -E '.Trash*' \"$HOME\" /mnt /media /run/media 2>/dev/null | "
			"sed 's|/\\.git/$||' | while IFS= read -r d; do "
			"  mtime=$(stat -c %Y \"$d\" 2>/dev/null || echo 0); "
			"  printf '%s\\t%s\\n' \"$mtime\" \"$d\"; "
			"done | sort -rn | head -500 > \"$cache.tmp\" && mv \"$cache.tmp\" \"$cache\"",
			(char *)NULL);
		_exit(127);
	} else if (pid > 0) {
		wlr_log(WLR_INFO, "git cache update started: pid=%d", pid);
	}
}

void
file_cache_update_start(void)
{
	pid_t pid;

	if (game_mode_active || htpc_mode_active)
		return;

	pid = fork();
	if (pid == 0) {
		setsid();
		/* Update file search cache in background with low priority
		 * No -H flag = ignores hidden files/directories by default */
		execlp("nice", "nice", "-n", "19", "ionice", "-c", "3", "sh", "-c",
			"cache=\"${XDG_CACHE_HOME:-$HOME/.cache}/nixlytile-file-search\"; "
			"fd -t f . -E 'node_modules' -E '__pycache__' \"$HOME\" /mnt /media /run/media 2>/dev/null | "
			"awk -F/ '{print $NF\"\\t\"$0\"\\t0\"}' > \"$cache.tmp\" && mv \"$cache.tmp\" \"$cache\"",
			(char *)NULL);
		_exit(127);
	} else if (pid > 0) {
		wlr_log(WLR_INFO, "file cache update started: pid=%d", pid);
	}
}

void
modal_truncate_to_width(const char *src, char *dst, size_t len, int max_px)
{
	size_t n;

	if (!dst || len == 0) {
		return;
	}
	dst[0] = '\0';
	if (!src || !*src || max_px <= 0) {
		return;
	}

	snprintf(dst, len, "%s", src);
	if (status_text_width(dst) <= max_px)
		return;

	n = strlen(dst);
	if (len < 4) {
		dst[0] = '\0';
		return;
	}

	while (n > 0 && status_text_width(dst) > max_px) {
		n--;
		if (n < 3) {
			dst[0] = '\0';
			return;
		}
		dst[n] = '\0';
	}

	if (strlen(dst) + 3 < len) {
		strncat(dst, "...", len - strlen(dst) - 1);
	}
}

int
desktop_entry_cmp_used(const void *a, const void *b)
{
	int ia = *(const int *)a;
	int ib = *(const int *)b;
	int ua = desktop_entries[ia].used;
	int ub = desktop_entries[ib].used;
	int namecmp;

	if (ua != ub)
		return (ua < ub) ? 1 : -1; /* more used first */

	namecmp = strcasecmp(desktop_entries[ia].name, desktop_entries[ib].name);
	if (namecmp != 0)
		return namecmp;

	return ia - ib;
}

int
modal_match_name(const char *haystack, const char *needle)
{
	if (!needle || !*needle)
		return 1;
	if (!haystack || !*haystack)
		return 0;

	return strstr(haystack, needle) != NULL;
}

void
strip_trailing_space(char *s)
{
	size_t len;

	if (!s)
		return;
	len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
				s[len - 1] == ' ' || s[len - 1] == '\t')) {
		s[len - 1] = '\0';
		len--;
	}
}

int
desktop_entry_exists(const char *name)
{
	if (!name || !*name)
		return 0;
	for (int i = 0; i < desktop_entry_count; i++) {
		if (!strcasecmp(desktop_entries[i].name, name))
			return 1;
	}
	return 0;
}

int
appdir_exists(char appdirs[][PATH_MAX], int count, const char *path)
{
	for (int i = 0; i < count; i++) {
		if (!strcmp(appdirs[i], path))
			return 1;
	}
	return 0;
}

void
load_desktop_dir_rec(const char *dir, int depth)
{
	struct dirent *ent;
	DIR *d;

	if (!dir || !*dir || depth > 5 || desktop_entry_count >= (int)LENGTH(desktop_entries))
		return;

	d = opendir(dir);
	if (!d)
		return;

	while ((ent = readdir(d))) {
		char path[PATH_MAX];
		FILE *fp;
		char line[512];
		char name[256] = {0};
		char exec[512] = {0};
		int nodisplay = 0;
		int hidden = 0;
		int isdir = 0;
		int in_main_section = 0;
		int prefers_dgpu = 0;

		if (desktop_entry_count >= (int)LENGTH(desktop_entries))
			break;

		if (ent->d_type == DT_DIR) {
			isdir = 1;
		} else if (ent->d_type == DT_UNKNOWN || ent->d_type == DT_LNK) {
			struct stat st = {0};
			if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int)sizeof(path))
				continue;
			if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
				isdir = 1;
		}

		if (isdir) {
			if (ent->d_name[0] == '.' || depth >= 5)
				continue;
			if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int)sizeof(path))
				continue;
			load_desktop_dir_rec(path, depth + 1);
			continue;
		}

		if (!strstr(ent->d_name, ".desktop"))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int)sizeof(path))
			continue;
		fp = fopen(path, "r");
		if (!fp)
			continue;

		while (fgets(line, sizeof(line), fp)) {
			if (line[0] == '[') {
				if (!strncmp(line, "[Desktop Entry]", 15)) {
					in_main_section = 1;
					continue;
				}
				if (in_main_section)
					break; /* stop at first other section */
				continue;
			}

			if (!in_main_section)
				continue;

			if (!strncmp(line, "Name=", 5)) {
				snprintf(name, sizeof(name), "%s", line + 5);
			} else if (!strncmp(line, "Name[", 5) && !name[0]) {
				char *eq = strchr(line, '=');
				if (eq && eq[1])
					snprintf(name, sizeof(name), "%s", eq + 1);
			} else if (!strncmp(line, "Exec=", 5)) {
				snprintf(exec, sizeof(exec), "%s", line + 5);
			} else if (!strncmp(line, "NoDisplay=", 10)) {
				const char *val = line + 10;
				if (!strncasecmp(val, "true", 4))
					nodisplay = 1;
			} else if (!strncmp(line, "Hidden=", 7)) {
				const char *val = line + 7;
				if (!strncasecmp(val, "true", 4))
					hidden = 1;
			} else if (!strncmp(line, "PrefersNonDefaultGPU=", 21)) {
				const char *val = line + 21;
				if (!strncasecmp(val, "true", 4))
					prefers_dgpu = 1;
			} else if (!strncmp(line, "X-KDE-RunOnDiscreteGpu=", 23)) {
				const char *val = line + 23;
				if (!strncasecmp(val, "true", 4))
					prefers_dgpu = 1;
			}
		}
		fclose(fp);

		strip_trailing_space(name);
		strip_trailing_space(exec);

		for (size_t k = 0; exec[k]; k++) {
			if (exec[k] == '%') {
				exec[k] = '\0';
				break;
			}
		}
		strip_trailing_space(exec);

		if (nodisplay || hidden || !name[0] || !exec[0])
			continue;
		if (desktop_entry_exists(name))
			continue;

		snprintf(desktop_entries[desktop_entry_count].name,
				sizeof(desktop_entries[desktop_entry_count].name),
				"%s", name);
		snprintf(desktop_entries[desktop_entry_count].exec,
				sizeof(desktop_entries[desktop_entry_count].exec),
				"%s", exec);
		memset(desktop_entries[desktop_entry_count].name_lower, 0,
				sizeof(desktop_entries[desktop_entry_count].name_lower));
		for (size_t j = 0; j < strlen(desktop_entries[desktop_entry_count].name) &&
				j < sizeof(desktop_entries[desktop_entry_count].name_lower) - 1; j++) {
			desktop_entries[desktop_entry_count].name_lower[j] =
				(char)tolower((unsigned char)desktop_entries[desktop_entry_count].name[j]);
		}
		desktop_entries[desktop_entry_count].prefers_dgpu = prefers_dgpu;
		desktop_entry_count++;
	}
	closedir(d);
}

void
load_desktop_dir(const char *dir)
{
	load_desktop_dir_rec(dir, 0);
}

void
ensure_desktop_entries_loaded(void)
{
	char appdirs[128][PATH_MAX];
	int appdir_count = 0;
	const char *home = getenv("HOME");
	const char *user = getenv("USER");
	const char *xdg_data_home = getenv("XDG_DATA_HOME");
	const char *defaults[] = {
		"/usr/share/applications",
		"/usr/local/share/applications",
		"/var/lib/flatpak/exports/share/applications",
		"/opt/share/applications",
		"/run/current-system/sw/share/applications",
		"/nix/var/nix/profiles/system/sw/share/applications",
		"/nix/profile/share/applications",
		"/nix/var/nix/profiles/default/share/applications",
		NULL
	};
	char buf[PATH_MAX];
	char xdg_buf[4096] = {0};
	char *saveptr = NULL;
	const char *xdg_data_dirs;

	if (desktop_entries_loaded)
		return;

	memset(appdirs, 0, sizeof(appdirs));

	for (size_t i = 0; defaults[i]; i++) {
		if (appdir_count >= (int)LENGTH(appdirs))
			break;
		if (appdir_exists(appdirs, appdir_count, defaults[i]))
			continue;
		snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", defaults[i]);
	}

	if (xdg_data_home && *xdg_data_home) {
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/applications", xdg_data_home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
	}

	if (home) {
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.local/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.local/share/flatpak/exports/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.nix-profile/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.local/state/nix/profiles/profile/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.local/state/nix/profiles/default/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.local/state/nix/profiles/home-manager/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
	}

	if (user && appdir_count < (int)LENGTH(appdirs)) {
		if (snprintf(buf, sizeof(buf), "/etc/profiles/per-user/%s/share/applications", user) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "/nix/var/nix/profiles/per-user/%s/profile/share/applications", user) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "/nix/var/nix/profiles/per-user/%s/home-manager/share/applications", user) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "/nix/var/nix/profiles/per-user/%s/sw/share/applications", user) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
	}

	xdg_data_dirs = getenv("XDG_DATA_DIRS");
	if (!xdg_data_dirs || !*xdg_data_dirs)
		xdg_data_dirs = "/usr/local/share:/usr/share";
	snprintf(xdg_buf, sizeof(xdg_buf), "%s", xdg_data_dirs);
	for (char *tok = strtok_r(xdg_buf, ":", &saveptr); tok; tok = strtok_r(NULL, ":", &saveptr)) {
		if (!*tok || appdir_count >= (int)LENGTH(appdirs))
			continue;
		if (snprintf(buf, sizeof(buf), "%s/applications", tok) >= (int)sizeof(buf))
			continue;
		if (appdir_exists(appdirs, appdir_count, buf))
			continue;
		snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count >= (int)LENGTH(appdirs))
			break;
	}

	for (int i = 0; i < appdir_count && desktop_entry_count < (int)LENGTH(desktop_entries); i++)
		load_desktop_dir(appdirs[i]);

	desktop_entries_loaded = 1;
}

void
modal_update_results(Monitor *m)
{
	ModalOverlay *mo;
	int active;

	if (!m)
		return;
	mo = &m->modal;
	if (mo->active_idx < 0 || mo->active_idx > 2)
		mo->active_idx = 0;
	active = mo->active_idx;

	if (active != 1 && mo->file_search_pid > 0)
		modal_file_search_stop(m);
	if (active != 2 && mo->git_search_pid > 0)
		modal_git_search_stop(m);

	if (active == 1) {
		if (mo->search_len[1] < modal_file_search_minlen) {
			modal_file_search_stop(m);
			modal_file_search_clear_results(m);
			mo->file_search_last[0] = '\0';
			/* Cancel any pending debounce timer */
			if (mo->file_search_timer)
				wl_event_source_timer_update(mo->file_search_timer, 0);
			return;
		}
		/* Search is started by render_timer_cb, not here */
		if (mo->file_search_pid <= 0 && mo->result_count[1] == 0 &&
		    mo->search_len[1] >= modal_file_search_minlen &&
		    strncmp(mo->search[1], mo->file_search_last, sizeof(mo->file_search_last)) != 0) {
			/* Start search if not running and query changed */
			modal_file_search_stop(m);
			modal_file_search_start(m);
		}
		if (mo->result_count[1] > 0) {
			if (mo->selected[1] < 0)
				mo->selected[1] = 0;
			modal_ensure_selection_visible(m);
		} else {
			mo->selected[1] = -1;
			mo->scroll[1] = 0;
		}
		return;
	}

	if (active == 2) {
		/* Git-projects: Check if search query changed */
		char current_query[256] = {0};
		int qlen = mo->search_len[2];
		if (qlen > (int)sizeof(current_query) - 1)
			qlen = (int)sizeof(current_query) - 1;
		if (qlen > 0)
			memcpy(current_query, mo->search[2], (size_t)qlen);
		current_query[qlen] = '\0';

		/* Restart search if query changed or not started yet */
		if (mo->git_search_pid <= 0 &&
		    (!mo->git_search_done || strcmp(current_query, mo->git_search_last) != 0)) {
			mo->git_search_done = 0;
			modal_git_search_start(m);
		}
		if (mo->result_count[2] > 0) {
			if (mo->selected[2] < 0)
				mo->selected[2] = 0;
			modal_ensure_selection_visible(m);
		} else {
			mo->selected[2] = -1;
			mo->scroll[2] = 0;
		}
		return;
	}

	if (active != 0) {
		mo->selected[active] = -1;
		mo->scroll[active] = 0;
		return;
	}

	{
		int prev_entry = -1;
		int order[LENGTH(desktop_entries)];
		int order_count;

		if (mo->selected[0] >= 0 && mo->selected[0] < mo->result_count[0])
			prev_entry = mo->result_entry_idx[0][mo->selected[0]];

		mo->result_count[0] = 0;
		for (int i = 0; i < (int)LENGTH(mo->results[0]); i++)
			mo->results[0][i][0] = '\0';
		for (int i = 0; i < (int)LENGTH(mo->result_entry_idx[0]); i++)
			mo->result_entry_idx[0][i] = -1;

		ensure_desktop_entries_loaded();
		order_count = desktop_entry_count;
		if (order_count > (int)LENGTH(order))
			order_count = (int)LENGTH(order);
		for (int i = 0; i < order_count; i++)
			order[i] = i;
		qsort(order, order_count, sizeof(order[0]), desktop_entry_cmp_used);

		if (mo->search_len[0] <= 0) {
			for (int i = 0; i < order_count && mo->result_count[0] < (int)LENGTH(mo->results[0]); i++) {
				int idx = order[i];
				snprintf(mo->results[0][mo->result_count[0]],
						sizeof(mo->results[0][mo->result_count[0]]),
						"%s", desktop_entries[idx].name);
				mo->result_entry_idx[0][mo->result_count[0]] = idx;
				mo->result_count[0]++;
			}
		} else {
			char needle[256] = {0};
			int nlen = mo->search_len[0];
			if (nlen >= (int)sizeof(needle))
				nlen = (int)sizeof(needle) - 1;
			for (int i = 0; i < nlen; i++)
				needle[i] = (char)tolower((unsigned char)mo->search[0][i]);
			needle[nlen] = '\0';

			/* exact matches first */
			for (int i = 0; i < order_count && mo->result_count[0] < (int)LENGTH(mo->results[0]); i++) {
				int idx = order[i];
				if (strcmp(desktop_entries[idx].name_lower, needle) == 0) {
					snprintf(mo->results[0][mo->result_count[0]],
							sizeof(mo->results[0][mo->result_count[0]]),
							"%s", desktop_entries[idx].name);
					mo->result_entry_idx[0][mo->result_count[0]] = idx;
					mo->result_count[0]++;
				}
			}
			/* then prefix matches */
			for (int i = 0; i < order_count && mo->result_count[0] < (int)LENGTH(mo->results[0]); i++) {
				int idx = order[i];
				if (strncmp(desktop_entries[idx].name_lower, needle, (size_t)nlen) == 0 &&
						strcmp(desktop_entries[idx].name_lower, needle) != 0) {
					snprintf(mo->results[0][mo->result_count[0]],
							sizeof(mo->results[0][mo->result_count[0]]),
							"%s", desktop_entries[idx].name);
					mo->result_entry_idx[0][mo->result_count[0]] = idx;
					mo->result_count[0]++;
				}
			}
			/* finally substring matches */
			for (int i = 0; i < order_count && mo->result_count[0] < (int)LENGTH(mo->results[0]); i++) {
				int idx = order[i];
				if (strstr(desktop_entries[idx].name_lower, needle) &&
						strncmp(desktop_entries[idx].name_lower, needle, (size_t)nlen) != 0) {
					snprintf(mo->results[0][mo->result_count[0]],
							sizeof(mo->results[0][mo->result_count[0]]),
							"%s", desktop_entries[idx].name);
					mo->result_entry_idx[0][mo->result_count[0]] = idx;
					mo->result_count[0]++;
				}
			}
		}

		mo->selected[0] = -1;
		if (mo->result_count[0] > 0) {
			int found_prev = 0;
			if (prev_entry >= 0) {
				for (int i = 0; i < mo->result_count[0]; i++) {
					if (mo->result_entry_idx[0][i] == prev_entry) {
						mo->selected[0] = i;
						found_prev = 1;
						break;
					}
				}
			}
			if (!found_prev) {
				mo->selected[0] = 0;
				mo->scroll[0] = 0;
			}
		}
	}

	if (mo->result_count[0] > 0)
		modal_ensure_selection_visible(m);
	else {
		mo->selected[0] = -1;
		mo->scroll[0] = 0;
	}
}

void
modal_prewarm(Monitor *m)
{
	int prev_idx;

	if (!m || !m->modal.tree || m->modal.visible)
		return;

	ensure_desktop_entries_loaded();

	prev_idx = m->modal.active_idx;
	m->modal.active_idx = 0;
	m->modal.search[0][0] = '\0';
	m->modal.search_len[0] = 0;
	modal_update_results(m);
	m->modal.active_idx = prev_idx;
	m->modal.selected[0] = -1;
	m->modal.scroll[0] = 0;
}

void
modal_render_search_field(Monitor *m)
{
	ModalOverlay *mo;
	int btn_h, field_h, line_h, pad;
	struct wlr_scene_node *node, *tmp;
	const char *text;
	const char *to_draw;

	if (!m || !m->modal.tree)
		return;
	mo = &m->modal;
	if (!mo->visible || mo->width <= 0 || mo->height <= 0)
		return;

	/* Check if search text actually changed */
	if (strcmp(mo->search[mo->active_idx], mo->search_rendered[mo->active_idx]) == 0)
		return;

	/* Destroy and recreate only the search field tree */
	if (mo->search_field_tree) {
		wl_list_for_each_safe(node, tmp, &mo->search_field_tree->children, link)
			wlr_scene_node_destroy(node);
	} else {
		mo->search_field_tree = wlr_scene_tree_create(mo->tree);
		if (!mo->search_field_tree)
			return;
	}

	modal_layout_metrics(&btn_h, &field_h, &line_h, &pad);

	text = mo->search[mo->active_idx];
	to_draw = (text && *text) ? text : "Type to search...";

	{
		int field_x = pad;
		int field_y = btn_h + pad;
		int field_w = mo->width - pad * 2;
		int field_h_use = field_h > 20 ? field_h : 20;
		StatusModule mod = {0};
		int text_w, text_x;

		if (field_w < 20) field_w = 20;
		wlr_scene_node_set_position(&mo->search_field_tree->node, 0, field_y);
		mod.tree = mo->search_field_tree;
		/* Center text */
		text_w = status_text_width(to_draw);
		text_x = field_x + (field_w - text_w) / 2;
		if (text_x < field_x + 6)
			text_x = field_x + 6;
		tray_render_label(&mod, to_draw, text_x, field_h_use, statusbar_fg);
	}

	snprintf(mo->search_rendered[mo->active_idx], sizeof(mo->search_rendered[0]),
			"%s", mo->search[mo->active_idx]);
}

int
modal_render_timer_cb(void *data)
{
	Monitor *m = data;
	ModalOverlay *mo;
	if (!m || !m->modal.visible)
		return 0;
	mo = &m->modal;

	if (mo->render_pending) {
		mo->render_pending = 0;
		/* For file search: start search directly with current text */
		if (mo->active_idx == 1 && mo->search_len[1] >= modal_file_search_minlen) {
			modal_file_search_stop(m);
			modal_file_search_start(m);
			/* Results will trigger render via modal_file_search_event */
		} else {
			/* Non-file-search: render immediately */
			modal_update_results(m);
			modal_render(m);
		}
	}
	return 0;
}

void
modal_schedule_render(Monitor *m)
{
	ModalOverlay *mo;

	if (!m)
		return;
	mo = &m->modal;

	mo->render_pending = 1;

	/* Create timer if needed */
	if (!mo->render_timer) {
		mo->render_timer = wl_event_loop_add_timer(event_loop, modal_render_timer_cb, m);
	}

	/* Reset timer to 300ms - only fires after typing stops */
	if (mo->render_timer)
		wl_event_source_timer_update(mo->render_timer, 300);
}

void
modal_render(Monitor *m)
{
	static const char *labels[] = {
		"App Launcher", "File Search", "Git-projects"
	};
	ModalOverlay *mo;
	int btn_h, btn_w, last_w, field_h, line_h, pad;
	struct wlr_scene_node *node, *tmp;

	if (!m || !m->modal.tree)
		return;

	mo = &m->modal;
	if (!mo->visible || mo->width <= 0 || mo->height <= 0)
		return;

	modal_layout_metrics(&btn_h, &field_h, &line_h, &pad);
	btn_w = mo->width / 3;
	if (btn_w <= 0)
		btn_w = mo->width;
	last_w = mo->width - btn_w * 2;
	if (last_w < btn_w)
		last_w = btn_w;
	if (mo->active_idx < 0 || mo->active_idx > 2)
		mo->active_idx = 0;
	/* NOTE: modal_update_results is called by keypress, not here */

	/* clear existing non-bg nodes */
	wl_list_for_each_safe(node, tmp, &mo->tree->children, link) {
		if (mo->bg && node == &mo->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}
	/* Reset cached trees since they were destroyed above */
	mo->search_field_tree = NULL;
	mo->results_tree = NULL;
	mo->row_highlight_count = 0;
	for (int i = 0; i < 3; i++)
		mo->search_rendered[i][0] = '\0';

	/* redraw background */
	if (mo->bg) {
		wl_list_for_each_safe(node, tmp, &mo->bg->children, link)
			wlr_scene_node_destroy(node);
		drawrect(mo->bg, 0, 0, mo->width, mo->height, statusbar_popup_bg);
		/* 1px black border */
		if (mo->width > 0 && mo->height > 0) {
			const float border[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			drawrect(mo->bg, 0, 0, mo->width, 1, border); /* top */
			drawrect(mo->bg, 0, mo->height - 1, mo->width, 1, border); /* bottom */
			drawrect(mo->bg, 0, 0, 1, mo->height, border); /* left */
			if (mo->width > 1)
				drawrect(mo->bg, mo->width - 1, 0, 1, mo->height, border); /* right */
		}
	}

	for (int i = 0; i < 3; i++) {
		int x = i * btn_w;
		int w = (i == 2) ? last_w : btn_w;
		const float *col = (mo->active_idx == i) ? statusbar_tag_active_bg : statusbar_tag_bg;
		struct wlr_scene_tree *btn = wlr_scene_tree_create(mo->tree);
		StatusModule mod = {0};
		int text_w;
		int text_x;

		if (btn) {
			wlr_scene_node_set_position(&btn->node, x, 0);
			drawrect(btn, 0, 0, w, btn_h, col);
			mod.tree = btn;
			text_w = status_text_width(labels[i]);
			text_x = (w - text_w) / 2;
			if (text_x < 4)
				text_x = 4;
			tray_render_label(&mod, labels[i], text_x, btn_h, statusbar_fg);
		}
	}

	/* search field */
	{
		int field_x = pad;
		int field_y = btn_h + pad;
		int field_w = mo->width - pad * 2;
		const float field_bg[4] = {0.1f, 0.1f, 0.1f, 0.5f};
		const float border[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		const char *text = mo->search[mo->active_idx];
		const char *to_draw = (text && *text) ? text : "Type to search...";
		struct wlr_scene_tree *row;
		StatusModule mod = {0};
		int text_x;

		if (field_w < 20)
			field_w = 20;
		if (field_h < 20)
			field_h = 20;

		if (mo->bg) {
			drawrect(mo->bg, field_x, field_y, field_w, field_h, field_bg);
			drawrect(mo->bg, field_x, field_y, field_w, 1, border);
			drawrect(mo->bg, field_x, field_y + field_h - 1, field_w, 1, border);
			drawrect(mo->bg, field_x, field_y, 1, field_h, border);
			drawrect(mo->bg, field_x + field_w - 1, field_y, 1, field_h, border);
		}

		/* Use search_field_tree so it's consistent with modal_render_search_field */
		if (!mo->search_field_tree)
			mo->search_field_tree = wlr_scene_tree_create(mo->tree);
		if (mo->search_field_tree) {
			int text_w = status_text_width(to_draw);
			wlr_scene_node_set_position(&mo->search_field_tree->node, 0, field_y);
			mod.tree = mo->search_field_tree;
			/* Center text */
			text_x = field_x + (field_w - text_w) / 2;
			if (text_x < field_x + 6)
				text_x = field_x + 6;
			tray_render_label(&mod, to_draw, text_x, field_h, statusbar_fg);
			/* Mark as rendered */
			snprintf(mo->search_rendered[mo->active_idx], sizeof(mo->search_rendered[0]),
					"%s", mo->search[mo->active_idx]);
		}
	}

	/* results list - use the dedicated function */
	modal_render_results(m);

	/* Hint text at bottom for file search */
	if (mo->active_idx == 1) {
		struct wlr_scene_tree *hint = wlr_scene_tree_create(mo->tree);
		if (hint) {
			StatusModule mod = {0};
			const char *hint_text = "Super + E to open all in Thunar";
			int hint_h = 20;
			int hint_y = mo->height - hint_h - 4;
			int text_w = status_text_width(hint_text);
			int text_x = (mo->width - text_w) / 2;
			const float hint_fg[4] = {0.6f, 0.6f, 0.6f, 1.0f};

			wlr_scene_node_set_position(&hint->node, 0, hint_y);
			mod.tree = hint;
			/* Small background bar */
			drawrect(hint, 0, 0, mo->width, hint_h, statusbar_popup_bg);
			tray_render_label(&mod, hint_text, text_x, hint_h, hint_fg);
		}
	}
}

void
modal_render_results(Monitor *m)
{
	ModalOverlay *mo;
	int btn_h, field_h, line_h, pad;
	int max_lines, start, max_start;
	int col_name_w = 0, col_path_w = 0, col_gap = 12;
	int line_y;
	int rendered_count = 0;

	if (!m || !m->modal.tree)
		return;
	mo = &m->modal;
	if (!mo->visible || mo->width <= 0 || mo->height <= 0)
		return;
	if (mo->active_idx < 0 || mo->active_idx > 2)
		return;
	if (mo->result_count[mo->active_idx] <= 0)
		return;

	modal_layout_metrics(&btn_h, &field_h, &line_h, &pad);
	line_y = btn_h + pad + field_h + pad;

	/* Destroy old results tree if it exists */
	if (mo->results_tree) {
		wlr_scene_node_destroy(&mo->results_tree->node);
		mo->results_tree = NULL;
	}
	/* Clear cached highlights */
	mo->row_highlight_count = 0;
	for (int i = 0; i < MODAL_MAX_RESULTS; i++)
		mo->row_highlights[i] = NULL;

	/* Create new results tree */
	mo->results_tree = wlr_scene_tree_create(mo->tree);
	if (!mo->results_tree)
		return;
	wlr_scene_node_set_position(&mo->results_tree->node, 0, line_y);

	max_lines = modal_max_visible_lines(m);
	if (max_lines <= 0)
		return;

	start = mo->scroll[mo->active_idx];
	max_start = mo->result_count[mo->active_idx] - max_lines;
	if (max_start < 0)
		max_start = 0;
	if (start < 0)
		start = 0;
	if (start > max_start)
		start = max_start;
	mo->scroll[mo->active_idx] = start;
	mo->last_scroll = start;
	mo->last_selected = mo->selected[mo->active_idx];

	if (mo->active_idx == 1) {
		int total_w = mo->width - pad * 2 - 4;
		int name_min = 80;
		int path_min = 120;
		col_name_w = total_w / 2;
		if (col_name_w < name_min)
			col_name_w = name_min;
		col_path_w = total_w - col_name_w - col_gap;
		if (col_path_w < path_min) {
			col_path_w = path_min;
			col_name_w = total_w - col_gap - col_path_w;
		}
		if (col_name_w < name_min)
			col_name_w = name_min;
	}

	for (int i = 0; i < max_lines && (start + i) < mo->result_count[mo->active_idx]; i++) {
		int idx = start + i;
		struct wlr_scene_tree *row = wlr_scene_tree_create(mo->results_tree);
		struct wlr_scene_rect *highlight;
		StatusModule mod = {0};
		char text_buf[MODAL_RESULT_LEN];
		int max_px = mo->width - pad * 2 - 8;
		char name_buf[128];
		char path_buf[128];
		char short_path[128];
		const char *name = mo->file_results_name[idx];
		const char *path = mo->file_results_path[idx];
		char path_dir[PATH_MAX];
		int path_w = 0;
		int path_x = 0;
		char *slash = NULL;
		int is_selected = (mo->selected[mo->active_idx] == idx);

		if (!row)
			continue;
		wlr_scene_node_set_position(&row->node, 0, i * line_h);
		mod.tree = row;

		/* Create highlight rect for this row - always create, toggle visibility */
		highlight = wlr_scene_rect_create(row, mo->width - pad * 2 + 4, line_h, statusbar_tag_active_bg);
		if (highlight) {
			wlr_scene_node_set_position(&highlight->node, pad - 2, 0);
			wlr_scene_node_set_enabled(&highlight->node, is_selected);
			if (rendered_count < MODAL_MAX_RESULTS)
				mo->row_highlights[rendered_count] = highlight;
		}

		if (mo->active_idx == 1) {
			if (path) {
				snprintf(path_dir, sizeof(path_dir), "%s", path);
				slash = strrchr(path_dir, '/');
				if (slash && slash != path_dir)
					*slash = '\0';
				else if (slash)
					*(slash + 1) = '\0';
			} else {
				snprintf(path_dir, sizeof(path_dir), ".");
			}
			shorten_path_display(path_dir, short_path, sizeof(short_path));
			modal_truncate_to_width(name ? name : "", name_buf, sizeof(name_buf), col_name_w);
			modal_truncate_to_width(short_path, path_buf, sizeof(path_buf), col_path_w);
			path_w = status_text_width(path_buf);
			path_x = mo->width - pad - path_w;
			if (path_x < pad + 10)
				path_x = pad + 10;
			tray_render_label(&mod, name_buf[0] ? name_buf : "--", pad, line_h, statusbar_fg);
			tray_render_label(&mod, path_buf[0] ? path_buf : "--", path_x, line_h, statusbar_fg);
		} else {
			if (max_px < 40)
				max_px = 40;
			modal_truncate_to_width(mo->results[mo->active_idx][idx],
					text_buf, sizeof(text_buf), max_px);
			if (!text_buf[0])
				snprintf(text_buf, sizeof(text_buf), "%s", mo->results[mo->active_idx][idx]);
			tray_render_label(&mod, text_buf, pad, line_h, statusbar_fg);
		}
		rendered_count++;
	}
	mo->row_highlight_count = rendered_count;
}

void
modal_update_selection(Monitor *m)
{
	ModalOverlay *mo;
	int start, sel, old_row, new_row;

	if (!m)
		return;
	mo = &m->modal;
	if (!mo->visible || !mo->results_tree)
		return;
	if (mo->active_idx < 0 || mo->active_idx > 2)
		return;
	if (mo->row_highlight_count <= 0)
		return;

	start = mo->scroll[mo->active_idx];
	sel = mo->selected[mo->active_idx];

	/* If scroll changed, we need full re-render */
	if (start != mo->last_scroll) {
		modal_render_results(m);
		return;
	}

	/* Calculate old and new row indices within visible area */
	old_row = mo->last_selected - start;
	new_row = sel - start;

	/* Hide old highlight */
	if (old_row >= 0 && old_row < mo->row_highlight_count && mo->row_highlights[old_row])
		wlr_scene_node_set_enabled(&mo->row_highlights[old_row]->node, 0);

	/* Show new highlight */
	if (new_row >= 0 && new_row < mo->row_highlight_count && mo->row_highlights[new_row])
		wlr_scene_node_set_enabled(&mo->row_highlights[new_row]->node, 1);

	mo->last_selected = sel;
}


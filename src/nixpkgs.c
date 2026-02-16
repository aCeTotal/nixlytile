#include "nixlytile.h"
#include "client.h"

int
cache_update_timer_cb(void *data)
{
	(void)data;
	if (!game_mode_active && !htpc_mode_active) {
		/* Run only one cache update at a time, cycling through phases */
		switch (cache_update_phase) {
		case 0:
			git_cache_update_start();
			break;
		case 1:
			file_cache_update_start();
			break;
		case 2:
			pc_gaming_cache_update_start();
			break;
		}
		cache_update_phase = (cache_update_phase + 1) % 3;
	}
	schedule_cache_update_timer();
	return 0;
}

void
schedule_cache_update_timer(void)
{
	if (!cache_update_timer || game_mode_active || htpc_mode_active)
		return;
	/* 30 minutes between each cache type = 90 min full cycle
	 * 30 minutes = 30 * 60 * 1000 ms = 1800000 ms */
	wl_event_source_timer_update(cache_update_timer, 1800000);
}

void
nixpkgs_cache_update_start(void)
{
	pid_t pid;
	char *home;
	char cache_path[PATH_MAX];

	if (game_mode_active || htpc_mode_active)
		return;

	home = getenv("HOME");
	if (!home)
		home = "/tmp";
	snprintf(cache_path, sizeof(cache_path), "%s/.cache/nixlytile-nixpkgs-stable.txt", home);

	pid = fork();
	if (pid == 0) {
		setsid();
		/* Update nixpkgs cache from nixpkgs-stable flake input with low priority */
		char cmd[2048];
		snprintf(cmd, sizeof(cmd),
			"nice -n 19 ionice -c 3 sh -c \""
			"nix eval --raw ~/.nixlyos#nixpkgs-stable.outPath 2>/dev/null | "
			"xargs -I{} nix-env -qaP -f {} 2>/dev/null | "
			"awk 'index(\\$1, \\\".\\\") == 0 {print \\$1\\\"\\\\t\\\"\\$2}' | "
			"sed 's/\\\\t.*-\\([0-9]\\)/\\\\t\\1/' > '%s'\"",
			cache_path);
		execl("/bin/sh", "sh", "-c", cmd, NULL);
		_exit(127);
	} else if (pid > 0) {
		wlr_log(WLR_INFO, "nixpkgs cache update started: pid=%d", pid);
	}
}

int
nixpkgs_cache_timer_cb(void *data)
{
	(void)data;
	if (!game_mode_active && !htpc_mode_active) {
		nixpkgs_cache_update_start();
	}
	schedule_nixpkgs_cache_timer();
	return 0;
}

void
schedule_nixpkgs_cache_timer(void)
{
	if (!nixpkgs_cache_timer)
		return;
	/* 1 week = 7 * 24 * 60 * 60 * 1000 ms = 604800000 ms */
	wl_event_source_timer_update(nixpkgs_cache_timer, 604800000);
}

void
add_nixpkg_entry(const char *name, const char *version)
{
	int i;
	if (nixpkg_entry_count >= NIXPKGS_MAX_ENTRIES)
		return;

	snprintf(nixpkg_entries[nixpkg_entry_count].name, sizeof(nixpkg_entries[0].name),
		"%s", name);
	snprintf(nixpkg_entries[nixpkg_entry_count].version, sizeof(nixpkg_entries[0].version),
		"%s", version ? version : "");
	nixpkg_entries[nixpkg_entry_count].installed = 0; /* Will be set by load_installed_packages */

	/* Create lowercase version for case-insensitive search */
	for (i = 0; nixpkg_entries[nixpkg_entry_count].name[i] && i < (int)sizeof(nixpkg_entries[0].name_lower) - 1; i++)
		nixpkg_entries[nixpkg_entry_count].name_lower[i] = tolower((unsigned char)nixpkg_entries[nixpkg_entry_count].name[i]);
	nixpkg_entries[nixpkg_entry_count].name_lower[i] = '\0';
	nixpkg_entry_count++;
}

void
load_nixlypkgs_cache(void)
{
	/* Load nixlypkgs packages from ~/dev_nixly/nixlypkgs/pkgs/ */
	char pkgs_dir[PATH_MAX];
	char *home;
	DIR *dir;
	struct dirent *entry;

	home = getenv("HOME");
	if (!home)
		return;

	snprintf(pkgs_dir, sizeof(pkgs_dir), "%s/dev_nixly/nixlypkgs/pkgs", home);
	dir = opendir(pkgs_dir);
	if (!dir)
		return;

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		if (entry->d_type == DT_DIR) {
			char name[140];
			snprintf(name, sizeof(name), "nixlypkgs.%s", entry->d_name);
			add_nixpkg_entry(name, "local");
		}
	}
	closedir(dir);
	wlr_log(WLR_INFO, "Added nixlypkgs entries, total now %d", nixpkg_entry_count);
}

void
load_nixpkgs_cache(void)
{
	FILE *f;
	char line[512];
	char *home;
	char *tab;

	if (nixpkg_entries_loaded)
		return;

	/* Build cache path from flake.nix nixpkgs-stable input */
	home = getenv("HOME");
	if (!home)
		home = "/tmp";
	snprintf(nixpkgs_cache_path, sizeof(nixpkgs_cache_path),
		"%s/.cache/nixlytile-nixpkgs-stable.txt", home);

	f = fopen(nixpkgs_cache_path, "r");
	if (!f) {
		wlr_log(WLR_INFO, "Nixpkgs cache not found at %s, generating...", nixpkgs_cache_path);
		/* Generate cache in background using nix-env -qa with version */
		pid_t pid = fork();
		if (pid == 0) {
			/* Child: generate cache with versions (format: name<TAB>version)
			 * Use nixpkgs-stable from ~/.nixlyos flake
			 * Filter to only show top-level packages (first column has no dots)
			 * This filters out internal packages like pythonPackages.*, haskellPackages.*, etc. */
			char cmd[2048];
			snprintf(cmd, sizeof(cmd),
				"nix eval --raw ~/.nixlyos#nixpkgs-stable.outPath 2>/dev/null | "
				"xargs -I{} nix-env -qaP -f {} 2>/dev/null | "
				"awk 'index($1, \".\") == 0 {print $1\"\\t\"$2}' | "
				"sed 's/\\t.*-\\([0-9]\\)/\\t\\1/' > '%s'",
				nixpkgs_cache_path);
			execl("/bin/sh", "sh", "-c", cmd, NULL);
			_exit(1);
		}
		nixpkg_entries_loaded = 1; /* Mark as loaded (empty) to avoid re-triggering */
		/* Still load nixlypkgs even if nixpkgs cache doesn't exist */
		load_nixlypkgs_cache();
		return;
	}

	nixpkg_entry_count = 0;
	while (fgets(line, sizeof(line), f) && nixpkg_entry_count < NIXPKGS_MAX_ENTRIES) {
		/* Remove newline */
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';
		if (line[0] == '\0')
			continue;

		/* Parse tab-separated format: name<TAB>version */
		tab = strchr(line, '\t');
		if (tab) {
			*tab = '\0';
			add_nixpkg_entry(line, tab + 1);
		} else {
			/* Old format without version */
			add_nixpkg_entry(line, NULL);
		}
	}
	fclose(f);

	/* Also load nixlypkgs packages */
	load_nixlypkgs_cache();

	nixpkg_entries_loaded = 1;
	wlr_log(WLR_INFO, "Loaded %d nixpkgs entries from cache", nixpkg_entry_count);

	/* Load installed packages status */
	load_installed_packages();
}

void
load_installed_packages(void)
{
	char packages_path[PATH_MAX];
	char *home;
	FILE *f;
	char *content = NULL;
	long file_size;
	int i, count = 0;

	home = getenv("HOME");
	if (!home)
		return;

	snprintf(packages_path, sizeof(packages_path),
		"%s/.nixlyos/modules/core/packages.nix", home);

	f = fopen(packages_path, "r");
	if (!f)
		return;

	fseek(f, 0, SEEK_END);
	file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	content = malloc(file_size + 1);
	if (!content) {
		fclose(f);
		return;
	}

	if (fread(content, 1, file_size, f) != (size_t)file_size) {
		fclose(f);
		free(content);
		return;
	}
	content[file_size] = '\0';
	fclose(f);

	/* Mark packages as installed if they appear in packages.nix */
	for (i = 0; i < nixpkg_entry_count; i++) {
		const char *name = nixpkg_entries[i].name;
		/* For nixlypkgs.foo entries, check for "foo" */
		if (strncmp(name, "nixlypkgs.", 10) == 0)
			name = name + 10;

		/* Simple check: look for the package name in the file */
		if (strstr(content, name)) {
			nixpkg_entries[i].installed = 1;
			count++;
		} else {
			nixpkg_entries[i].installed = 0;
		}
	}

	free(content);
	wlr_log(WLR_INFO, "Marked %d packages as installed", count);
}

int
ensure_nixpkg_ok_icon(int height)
{
	GdkPixbuf *pixbuf = NULL;
	char icon_path[PATH_MAX];

	if (nixpkg_ok_icon_buf && nixpkg_ok_icon_height == height)
		return 0;

	/* Drop old buffer */
	if (nixpkg_ok_icon_buf) {
		wlr_buffer_drop(nixpkg_ok_icon_buf);
		nixpkg_ok_icon_buf = NULL;
		nixpkg_ok_icon_height = 0;
	}

	if (resolve_asset_path("images/svg/ok.svg", icon_path, sizeof(icon_path)) != 0)
		return -1;

	if (tray_load_svg_pixbuf(icon_path, height, &pixbuf) != 0)
		return -1;

	{
		int w = gdk_pixbuf_get_width(pixbuf);
		int h = gdk_pixbuf_get_height(pixbuf);
		int stride = gdk_pixbuf_get_rowstride(pixbuf);
		int n_chan = gdk_pixbuf_get_n_channels(pixbuf);
		guchar *pix = gdk_pixbuf_get_pixels(pixbuf);
		uint32_t *argb = malloc(w * h * 4);

		if (!argb) {
			g_object_unref(pixbuf);
			return -1;
		}

		for (int y = 0; y < h; y++) {
			guchar *row = pix + y * stride;
			for (int x = 0; x < w; x++) {
				guchar r = row[x * n_chan + 0];
				guchar g = row[x * n_chan + 1];
				guchar b = row[x * n_chan + 2];
				guchar a = (n_chan == 4) ? row[x * n_chan + 3] : 255;
				argb[y * w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
				                  ((uint32_t)g << 8) | (uint32_t)b;
			}
		}

		nixpkg_ok_icon_buf = statusbar_buffer_from_argb32_raw(argb, w, h);
		free(argb);
		g_object_unref(pixbuf);

		if (!nixpkg_ok_icon_buf)
			return -1;

		nixpkg_ok_icon_height = height;
	}

	return 0;
}

void
ensure_nixpkgs_cache_loaded(void)
{
	if (!nixpkg_entries_loaded)
		load_nixpkgs_cache();
}

Monitor *
nixpkgs_visible_monitor(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->nixpkgs.visible)
			return m;
	}
	return NULL;
}

void
nixpkgs_hide(Monitor *m)
{
	if (!m || !m->nixpkgs.tree)
		return;
	m->nixpkgs.visible = 0;
	wlr_scene_node_set_enabled(&m->nixpkgs.tree->node, 0);
}

void
nixpkgs_hide_all(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		nixpkgs_hide(m);
}

void
nixpkgs_update_results(Monitor *m)
{
	NixpkgsOverlay *no;
	char needle[256];
	int i, k, count;

	if (!m)
		return;
	no = &m->nixpkgs;

	ensure_nixpkgs_cache_loaded();

	/* Convert search to lowercase */
	for (i = 0; i < no->search_len && i < (int)sizeof(needle) - 1; i++)
		needle[i] = tolower((unsigned char)no->search[i]);
	needle[i] = '\0';

	count = 0;
	if (needle[0] == '\0') {
		/* Empty search: show first N packages */
		for (i = 0; i < nixpkg_entry_count && count < MODAL_MAX_RESULTS; i++)
			no->result_indices[count++] = i;
	} else {
		/* Search: prefix match first, then substring */
		/* Pass 1: prefix matches */
		for (i = 0; i < nixpkg_entry_count && count < MODAL_MAX_RESULTS; i++) {
			if (strncmp(nixpkg_entries[i].name_lower, needle, strlen(needle)) == 0)
				no->result_indices[count++] = i;
		}
		/* Pass 2: substring matches (not prefix) */
		for (i = 0; i < nixpkg_entry_count && count < MODAL_MAX_RESULTS; i++) {
			if (strncmp(nixpkg_entries[i].name_lower, needle, strlen(needle)) != 0 &&
			    strstr(nixpkg_entries[i].name_lower, needle))
				no->result_indices[count++] = i;
		}
	}
	no->result_count = count;

	/* Reset selection if out of range */
	if (no->selected >= count)
		no->selected = count > 0 ? count - 1 : -1;
	if (no->selected < 0 && count > 0)
		no->selected = 0;
}

void
nixpkgs_layout_metrics(int *field_h, int *line_h, int *pad)
{
	*field_h = 36;
	*line_h = 24;
	*pad = 10;
}

int
nixpkgs_max_visible_lines(Monitor *m)
{
	NixpkgsOverlay *no;
	int field_h, line_h, pad;
	int usable;
	int hint_h = 20 + 4; /* hint height + margin */
	int title_h = 32;

	if (!m)
		return 0;
	no = &m->nixpkgs;

	nixpkgs_layout_metrics(&field_h, &line_h, &pad);
	/* title + pad + field + pad + results + hint area */
	usable = no->height - title_h - pad - field_h - pad - hint_h - pad;
	if (usable <= 0)
		return 0;
	return usable / line_h;
}

void
nixpkgs_ensure_selection_visible(Monitor *m)
{
	NixpkgsOverlay *no;
	int max_lines, sel;

	if (!m)
		return;
	no = &m->nixpkgs;
	sel = no->selected;
	if (sel < 0)
		return;

	max_lines = nixpkgs_max_visible_lines(m);
	if (max_lines <= 0)
		return;

	if (sel < no->scroll)
		no->scroll = sel;
	else if (sel >= no->scroll + max_lines)
		no->scroll = sel - max_lines + 1;
}

void
nixpkgs_render(Monitor *m)
{
	NixpkgsOverlay *no;
	int field_h, line_h, pad;
	struct wlr_scene_node *node, *tmp;

	if (!m || !m->nixpkgs.tree)
		return;

	no = &m->nixpkgs;
	if (!no->visible || no->width <= 0 || no->height <= 0)
		return;

	nixpkgs_layout_metrics(&field_h, &line_h, &pad);

	/* Clear existing non-bg nodes */
	wl_list_for_each_safe(node, tmp, &no->tree->children, link) {
		if (no->bg && node == &no->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}
	/* Reset cached trees */
	no->search_field_tree = NULL;
	no->results_tree = NULL;
	no->row_highlight_count = 0;
	no->search_rendered[0] = '\0';

	/* Redraw background */
	if (no->bg) {
		wl_list_for_each_safe(node, tmp, &no->bg->children, link)
			wlr_scene_node_destroy(node);
		drawrect(no->bg, 0, 0, no->width, no->height, statusbar_popup_bg);
		if (no->width > 0 && no->height > 0) {
			const float border[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			draw_border(no->bg, 0, 0, no->width, no->height, 1, border);
		}
	}

	/* Title */
	{
		struct wlr_scene_tree *title = wlr_scene_tree_create(no->tree);
		if (title) {
			StatusModule mod = {0};
			const char *title_text = "Nixpkgs Install";
			int title_h = 32;
			int text_w = status_text_width(title_text);
			int text_x = (no->width - text_w) / 2;

			wlr_scene_node_set_position(&title->node, 0, 4);
			mod.tree = title;
			drawrect(title, 0, 0, no->width, title_h, statusbar_tag_active_bg);
			tray_render_label(&mod, title_text, text_x, title_h, statusbar_fg);
		}
	}

	/* Search field */
	{
		int field_x = pad;
		int field_y = 32 + pad;
		int field_w = no->width - pad * 2;
		const float field_bg[4] = {0.1f, 0.1f, 0.1f, 0.5f};
		const float border[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		const char *text = no->search;
		const char *to_draw = (text && *text) ? text : "Type to search packages...";
		StatusModule mod = {0};
		int text_w, text_x;

		if (field_w < 20)
			field_w = 20;
		if (field_h < 20)
			field_h = 20;

		if (no->bg) {
			drawrect(no->bg, field_x, field_y, field_w, field_h, field_bg);
			draw_border(no->bg, field_x, field_y, field_w, field_h, 1, border);
		}

		if (!no->search_field_tree)
			no->search_field_tree = wlr_scene_tree_create(no->tree);
		if (no->search_field_tree) {
			wlr_scene_node_set_position(&no->search_field_tree->node, 0, field_y);
			mod.tree = no->search_field_tree;
			text_w = status_text_width(to_draw);
			text_x = field_x + (field_w - text_w) / 2;
			if (text_x < field_x + 6)
				text_x = field_x + 6;
			tray_render_label(&mod, to_draw, text_x, field_h, statusbar_fg);
			snprintf(no->search_rendered, sizeof(no->search_rendered), "%s", no->search);
		}
	}

	/* Results list */
	nixpkgs_render_results(m);

	/* Hint at bottom */
	{
		struct wlr_scene_tree *hint = wlr_scene_tree_create(no->tree);
		if (hint) {
			StatusModule mod = {0};
			const char *hint_text = "Hit enter to install package";
			int hint_h = 20;
			int hint_y = no->height - hint_h - 4;
			int text_w = status_text_width(hint_text);
			int text_x = (no->width - text_w) / 2;
			const float hint_fg[4] = {0.6f, 0.6f, 0.6f, 1.0f};

			wlr_scene_node_set_position(&hint->node, 0, hint_y);
			mod.tree = hint;
			drawrect(hint, 0, 0, no->width, hint_h, statusbar_popup_bg);
			tray_render_label(&mod, hint_text, text_x, hint_h, hint_fg);
		}
	}
}

static void
nixpkgs_render_row(NixpkgsOverlay *no, int idx, int y, int line_h, int pad,
	int selected, int row_idx)
{
	static const float sel_bg[4] = {0.2f, 0.4f, 0.6f, 0.8f};
	static const float version_fg[4] = {0.5f, 0.5f, 0.5f, 1.0f};
	const char *name = nixpkg_entries[idx].name;
	const char *version = nixpkg_entries[idx].version;
	struct wlr_scene_tree *row;
	struct wlr_scene_rect *highlight;
	StatusModule mod = {0};
	int name_w;

	row = wlr_scene_tree_create(no->results_tree);
	if (!row)
		return;
	wlr_scene_node_set_position(&row->node, pad, y);

	highlight = wlr_scene_rect_create(row, no->width - pad * 2, line_h, sel_bg);
	if (highlight) {
		wlr_scene_node_set_position(&highlight->node, 0, 0);
		wlr_scene_node_set_enabled(&highlight->node, selected);
		no->row_highlights[row_idx] = highlight;
	}

	mod.tree = row;
	tray_render_label(&mod, name, 4, line_h, statusbar_fg);

	name_w = status_text_width(name);
	if (version && version[0]) {
		char ver_str[80];
		snprintf(ver_str, sizeof(ver_str), "(%s)", version);
		tray_render_label(&mod, ver_str, 4 + name_w + 8, line_h, version_fg);

		if (nixpkg_entries[idx].installed) {
			int icon_h = line_h - 4;
			if (ensure_nixpkg_ok_icon(icon_h) == 0 && nixpkg_ok_icon_buf) {
				int ver_w = status_text_width(ver_str);
				int icon_x = 4 + name_w + 8 + ver_w + 6;
				int icon_y = (line_h - icon_h) / 2;
				struct wlr_scene_buffer *icon_node =
					wlr_scene_buffer_create(row, nixpkg_ok_icon_buf);
				if (icon_node)
					wlr_scene_node_set_position(&icon_node->node, icon_x, icon_y);
			}
		}
	} else if (nixpkg_entries[idx].installed) {
		int icon_h = line_h - 4;
		if (ensure_nixpkg_ok_icon(icon_h) == 0 && nixpkg_ok_icon_buf) {
			int icon_x = 4 + name_w + 8;
			int icon_y = (line_h - icon_h) / 2;
			struct wlr_scene_buffer *icon_node =
				wlr_scene_buffer_create(row, nixpkg_ok_icon_buf);
			if (icon_node)
				wlr_scene_node_set_position(&icon_node->node, icon_x, icon_y);
		}
	}

	{
		const char *source_text = "Source (Mod+w)";
		int source_w = status_text_width(source_text);
		int source_x = no->width - pad * 2 - source_w - 8;
		tray_render_label(&mod, source_text, source_x, line_h, version_fg);
	}
}

void
nixpkgs_render_results(Monitor *m)
{
	NixpkgsOverlay *no;
	int field_h, line_h, pad;
	int max_lines, start, max_start;
	int line_y;
	int rendered_count = 0;
	int i;

	if (!m || !m->nixpkgs.tree)
		return;
	no = &m->nixpkgs;
	if (!no->visible || no->width <= 0 || no->height <= 0)
		return;
	if (no->result_count <= 0)
		return;

	nixpkgs_layout_metrics(&field_h, &line_h, &pad);
	line_y = 32 + pad + field_h + pad;

	if (no->results_tree) {
		wlr_scene_node_destroy(&no->results_tree->node);
		no->results_tree = NULL;
	}
	no->row_highlight_count = 0;
	for (i = 0; i < MODAL_MAX_RESULTS; i++)
		no->row_highlights[i] = NULL;

	no->results_tree = wlr_scene_tree_create(no->tree);
	if (!no->results_tree)
		return;
	wlr_scene_node_set_position(&no->results_tree->node, 0, line_y);

	max_lines = nixpkgs_max_visible_lines(m);
	if (max_lines <= 0)
		return;

	start = no->scroll;
	max_start = no->result_count - max_lines;
	if (max_start < 0)
		max_start = 0;
	if (start < 0)
		start = 0;
	if (start > max_start)
		start = max_start;
	no->scroll = start;

	for (i = start; i < no->result_count && rendered_count < max_lines; i++) {
		nixpkgs_render_row(no, no->result_indices[i], rendered_count * line_h,
			line_h, pad, i == no->selected, rendered_count);
		rendered_count++;
	}
	no->row_highlight_count = rendered_count;
	no->last_scroll = start;
	no->last_selected = no->selected;
}

void
nixpkgs_update_selection(Monitor *m)
{
	NixpkgsOverlay *no;
	int start, sel, old_row, new_row;

	if (!m)
		return;
	no = &m->nixpkgs;
	if (!no->visible || !no->results_tree)
		return;
	if (no->row_highlight_count <= 0)
		return;

	start = no->scroll;
	sel = no->selected;

	/* If scroll changed, need full re-render */
	if (start != no->last_scroll) {
		nixpkgs_render_results(m);
		return;
	}

	/* Calculate old and new row indices */
	old_row = no->last_selected - start;
	new_row = sel - start;

	/* Hide old highlight */
	if (old_row >= 0 && old_row < no->row_highlight_count && no->row_highlights[old_row])
		wlr_scene_node_set_enabled(&no->row_highlights[old_row]->node, 0);

	/* Show new highlight */
	if (new_row >= 0 && new_row < no->row_highlight_count && no->row_highlights[new_row])
		wlr_scene_node_set_enabled(&no->row_highlights[new_row]->node, 1);

	no->last_selected = sel;
}

void
nixpkgs_open_source(Monitor *m)
{
	NixpkgsOverlay *no;
	int idx;
	const char *name;
	char url[512];
	char cmd_str[600];
	char *cmd[4] = { "sh", "-c", cmd_str, NULL };
	Arg arg;

	if (!m)
		return;
	no = &m->nixpkgs;
	if (no->selected < 0 || no->selected >= no->result_count)
		return;

	idx = no->result_indices[no->selected];
	name = nixpkg_entries[idx].name;

	/* Check if it's a nixlypkgs package */
	if (strncmp(name, "nixlypkgs.", 10) == 0) {
		/* Open local nixlypkgs package in file manager or editor */
		const char *pkg = name + 10; /* skip "nixlypkgs." prefix */
		snprintf(url, sizeof(url),
			"https://github.com/totalygeek/nixlypkgs/tree/main/pkgs/%s", pkg);
	} else {
		/* Open on GitHub nixpkgs (nixos-25.11 stable branch) */
		snprintf(url, sizeof(url),
			"https://github.com/NixOS/nixpkgs/blob/nixos-25.11/pkgs/by-name/%c%c/%s/package.nix",
			name[0], name[1] && name[1] != '\0' ? name[1] : name[0], name);
	}

	snprintf(cmd_str, sizeof(cmd_str), "xdg-open '%s'", url);
	arg.v = cmd;
	spawn(&arg);
}

void
nixpkgs_install_selected(Monitor *m)
{
	NixpkgsOverlay *no;
	int idx;
	const char *name;
	char packages_path[PATH_MAX];
	char *home;
	FILE *f;
	char *content = NULL;
	long file_size;
	char *insert_pos;
	char *new_content;
	size_t new_size;
	const char *marker = "];"; /* End of hmPackages list */
	char pkg_name[140];

	if (!m)
		return;
	no = &m->nixpkgs;
	if (no->selected < 0 || no->selected >= no->result_count)
		return;

	idx = no->result_indices[no->selected];
	name = nixpkg_entries[idx].name;

	/* Get package name without nixlypkgs. prefix for insertion */
	if (strncmp(name, "nixlypkgs.", 10) == 0) {
		snprintf(pkg_name, sizeof(pkg_name), "%s", name + 10);
	} else {
		snprintf(pkg_name, sizeof(pkg_name), "%s", name);
	}

	/* Build path to packages.nix */
	home = getenv("HOME");
	if (!home)
		return;
	snprintf(packages_path, sizeof(packages_path),
		"%s/.nixlyos/modules/core/packages.nix", home);

	/* Read the file */
	f = fopen(packages_path, "r");
	if (!f) {
		wlr_log(WLR_ERROR, "Failed to open %s for reading", packages_path);
		return;
	}

	fseek(f, 0, SEEK_END);
	file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	content = malloc(file_size + 1);
	if (!content) {
		fclose(f);
		return;
	}

	if (fread(content, 1, file_size, f) != (size_t)file_size) {
		fclose(f);
		free(content);
		return;
	}
	content[file_size] = '\0';
	fclose(f);

	/* Check if package already exists */
	if (strstr(content, pkg_name)) {
		wlr_log(WLR_INFO, "Package %s already in packages.nix", pkg_name);
		free(content);
		nixpkgs_hide_all();
		return;
	}

	/* Find the first "];" which marks the end of hmPackages */
	insert_pos = strstr(content, marker);
	if (!insert_pos) {
		wlr_log(WLR_ERROR, "Could not find hmPackages end marker in packages.nix");
		free(content);
		return;
	}

	/* Create new content with package inserted before "];" */
	/* "    pkg_name\n  " = 4 spaces + name + newline + 2 spaces = 7 + strlen */
	new_size = file_size + strlen(pkg_name) + 10;
	new_content = malloc(new_size + 1);
	if (!new_content) {
		free(content);
		return;
	}

	/* Copy content before marker, add new package, add marker and rest */
	{
		size_t prefix_len = insert_pos - content;
		char *p = new_content;

		memcpy(p, content, prefix_len);
		p += prefix_len;

		p += sprintf(p, "    %s\n  ", pkg_name);

		strcpy(p, insert_pos);
	}

	/* Write the file back */
	f = fopen(packages_path, "w");
	if (!f) {
		wlr_log(WLR_ERROR, "Failed to open %s for writing", packages_path);
		free(content);
		free(new_content);
		return;
	}

	fputs(new_content, f);
	fclose(f);

	wlr_log(WLR_INFO, "Added %s to packages.nix", pkg_name);

	/* Mark package as installed in our cache */
	nixpkg_entries[idx].installed = 1;

	free(content);
	free(new_content);
	nixpkgs_hide_all();

	/* Show sudo popup to run nixos-rebuild boot */
	{
		char rebuild_cmd[512];
		snprintf(rebuild_cmd, sizeof(rebuild_cmd),
			"cd %s/.nixlyos && nixos-rebuild boot --flake .#nixlyos", home);
		sudo_popup_show(m, "sudo:", rebuild_cmd, pkg_name);
	}
}

int
nixpkgs_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym)
{
	NixpkgsOverlay *no;

	if (!m)
		return 0;
	no = &m->nixpkgs;

	if (sym == XKB_KEY_Escape) {
		nixpkgs_hide_all();
		return 1;
	}

	if (sym == XKB_KEY_BackSpace && no->search_len > 0 && mods == 0) {
		no->search_len--;
		no->search[no->search_len] = '\0';
		return 1;
	}

	if (sym == XKB_KEY_Down && no->result_count > 0) {
		int sel = no->selected;
		if (sel < 0)
			sel = 0;
		else if (sel + 1 < no->result_count)
			sel++;
		else
			sel = 0; /* wrap */
		no->selected = sel;
		nixpkgs_ensure_selection_visible(m);
		return 1;
	}

	if (sym == XKB_KEY_Up && no->result_count > 0) {
		int sel = no->selected;
		if (sel < 0)
			sel = no->result_count - 1;
		else if (sel > 0)
			sel--;
		else
			sel = no->result_count - 1; /* wrap */
		no->selected = sel;
		nixpkgs_ensure_selection_visible(m);
		return 1;
	}

	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		nixpkgs_install_selected(m);
		return 1;
	}

	/* Mod+w: Open source in browser */
	if ((sym == XKB_KEY_w || sym == XKB_KEY_W) && (mods & WLR_MODIFIER_LOGO)) {
		nixpkgs_open_source(m);
		return 1;
	}

	/* Text input */
	{
		int is_altgr = (mods & WLR_MODIFIER_CTRL) && (mods & WLR_MODIFIER_ALT);
		int is_ctrl_only = (mods & WLR_MODIFIER_CTRL) && !is_altgr;
		if (sym >= 0x20 && sym <= 0x7e && !is_ctrl_only && !(mods & WLR_MODIFIER_LOGO)) {
			int len = no->search_len;
			if (len + 1 < (int)sizeof(no->search)) {
				no->search[len] = (char)sym;
				no->search[len + 1] = '\0';
				no->search_len = len + 1;
			}
			return 1;
		}
	}

	return 0;
}

void
nixpkgs_show(const Arg *arg)
{
	Monitor *m = selmon ? selmon : xytomon(cursor->x, cursor->y);
	Monitor *vm = nixpkgs_visible_monitor();
	NixpkgsOverlay *no;
	int mw, mh, w, h, x, y;
	int min_w = 300, min_h = 200;
	int max_w = 800, max_h = 600;

	(void)arg;

	if (!m || !m->nixpkgs.tree)
		return;

	/* Hide modal if visible */
	modal_hide_all();

	if (vm && vm != m)
		nixpkgs_hide_all();

	/* Load cache on first show */
	ensure_nixpkgs_cache_loaded();

	mw = m->w.width > 0 ? m->w.width : m->m.width;
	mh = m->w.height > 0 ? m->w.height : m->m.height;
	w = mw > 0 ? (int)(mw * 0.5) : 600;
	h = mh > 0 ? (int)(mh * 0.6) : 500;
	if (w < min_w) w = min_w;
	if (h < min_h) h = min_h;
	if (w > max_w) w = max_w;
	if (h > max_h) h = max_h;

	x = m->m.x + (m->m.width - w) / 2;
	y = m->m.y + (m->m.height - h) / 2;
	if (x < m->m.x) x = m->m.x;
	if (y < m->m.y) y = m->m.y;

	no = &m->nixpkgs;
	no->width = w;
	no->height = h;
	no->x = x;
	no->y = y;

	if (!no->bg)
		no->bg = wlr_scene_tree_create(no->tree);
	if (no->bg) {
		struct wlr_scene_node *node, *tmp;
		wl_list_for_each_safe(node, tmp, &no->bg->children, link)
			wlr_scene_node_destroy(node);
		drawrect(no->bg, 0, 0, w, h, statusbar_popup_bg);
	}

	wlr_scene_node_set_position(&no->tree->node, x, y);
	wlr_scene_node_set_enabled(&no->tree->node, 1);

	/* Reset state */
	no->search[0] = '\0';
	no->search_len = 0;
	no->search_rendered[0] = '\0';
	no->selected = 0;
	no->scroll = 0;
	no->search_field_tree = NULL;
	no->results_tree = NULL;
	no->row_highlight_count = 0;

	no->visible = 1;
	nixpkgs_update_results(m);
	nixpkgs_render(m);
}


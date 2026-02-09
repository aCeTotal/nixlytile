#include "nixlytile.h"
#include "client.h"

int
tray_load_icon_file(TrayItem *it, const char *path, int desired_h)
{
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	GdkPixbuf *pixbuf = NULL;

	if (!it || !path || !*path)
		return -1;

	if (has_svg_extension(path)) {
		if (tray_load_svg_pixbuf(path, desired_h, &pixbuf) != 0) {
			pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
			if (!pixbuf && gerr) {
				wlr_log(WLR_ERROR, "tray: failed to load icon '%s': %s", path, gerr->message);
				g_error_free(gerr);
				gerr = NULL;
			}
			if (!pixbuf)
				return -1;
		}
	} else {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "tray: failed to load icon '%s': %s", path, gerr->message);
				g_error_free(gerr);
				gerr = NULL;
			}
			if (has_svg_extension(path) &&
					tray_load_svg_pixbuf(path, desired_h, &pixbuf) != 0)
				return -1;
			if (!pixbuf)
				return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, desired_h, &w, &h);
	if (!buf)
		return -1;

	if (it->icon_buf)
		wlr_buffer_drop(it->icon_buf);
	it->icon_buf = buf;
	it->icon_w = w;
	it->icon_h = h;
	return 0;
}

int
tray_lookup_icon_in_theme(const char *theme_root, const char *name, int desired_h,
		char *best_path, int *best_diff, int *found)
{
	static const char *subdirs[] = {
		"", "status", "apps", "panel", "actions",
		"devices", "emblems", "places", "categories", "mimetypes"
	};
	static const char *exts[] = { "png", "svg", "xpm", "jpg", "jpeg" };
	const int base_sizes[] = { 16, 22, 24, 32, 48, 64, 96, 128, 256 };
	int sizes[LENGTH(base_sizes) + 1] = {0};
	size_t size_count = 0;
	char symbolic[128] = {0};
	char nosymbolic[128] = {0};
	const char *candidates[3] = { name, NULL, NULL };
	size_t cand_count = 1;

	if (!theme_root || !name || !*name || !pathisdir(theme_root))
		return -1;

	if (desired_h > 0)
		sizes[size_count++] = desired_h;
	for (size_t i = 0; i < LENGTH(base_sizes) && size_count < LENGTH(sizes); i++) {
		if (desired_h > 0 && base_sizes[i] == desired_h)
			continue;
		sizes[size_count++] = base_sizes[i];
	}

	if (strip_symbolic_suffix(name, nosymbolic, sizeof(nosymbolic))) {
		candidates[0] = nosymbolic; /* Prefer colored variant over symbolic */
		candidates[cand_count++] = name;
	} else {
		candidates[0] = name;
	}
	if (!strstr(name, "-symbolic") && cand_count < LENGTH(candidates)) {
		snprintf(symbolic, sizeof(symbolic), "%s-symbolic", name);
		candidates[cand_count++] = symbolic;
	}

	for (size_t s = 0; s < size_count; s++) {
		char sizedir[32];
		snprintf(sizedir, sizeof(sizedir), "%dx%d", sizes[s], sizes[s]);
		for (size_t d = 0; d < LENGTH(subdirs); d++) {
			for (size_t c = 0; c < cand_count; c++) {
				if (!candidates[c])
					continue;
				for (size_t e = 0; e < LENGTH(exts); e++) {
					char path[PATH_MAX];
					if (*subdirs[d])
						snprintf(path, sizeof(path), "%s/%s/%s/%s.%s",
								theme_root, sizedir, subdirs[d], candidates[c], exts[e]);
					else
						snprintf(path, sizeof(path), "%s/%s/%s.%s",
								theme_root, sizedir, candidates[c], exts[e]);
					tray_consider_icon(path, sizes[s], desired_h, best_path, best_diff, found);
					if (*found && *best_diff == 0)
						return 0;
				}
			}
		}
	}

	for (size_t d = 0; d < LENGTH(subdirs); d++) {
		for (size_t c = 0; c < cand_count; c++) {
			if (!candidates[c])
				continue;
			for (size_t e = 0; e < LENGTH(exts); e++) {
				char path[PATH_MAX];
				if (*subdirs[d])
					snprintf(path, sizeof(path), "%s/scalable/%s/%s.%s",
							theme_root, subdirs[d], candidates[c], exts[e]);
				else
					snprintf(path, sizeof(path), "%s/scalable/%s.%s",
							theme_root, candidates[c], exts[e]);
				tray_consider_icon(path, desired_h, desired_h, best_path, best_diff, found);
				if (*found && *best_diff == 0)
					return 0;
			}
		}
	}

	for (size_t d = 0; d < LENGTH(subdirs); d++) {
		for (size_t c = 0; c < cand_count; c++) {
			if (!candidates[c])
				continue;
			for (size_t e = 0; e < LENGTH(exts); e++) {
				char path[PATH_MAX];
				if (*subdirs[d])
					snprintf(path, sizeof(path), "%s/%s/%s.%s",
							theme_root, subdirs[d], candidates[c], exts[e]);
				else
					snprintf(path, sizeof(path), "%s/%s.%s",
							theme_root, candidates[c], exts[e]);
				tray_consider_icon(path, desired_h, desired_h, best_path, best_diff, found);
				if (*found && *best_diff == 0)
					return 0;
			}
		}
	}

	return *found ? 0 : -1;
}

int
tray_lookup_icon_in_root(const char *base, const char *name, int desired_h,
		char *best_path, int *best_diff, int *found)
{
	DIR *dir;
	struct dirent *ent;
	static const char *exts[] = { "png", "svg", "xpm", "jpg", "jpeg" };

	if (!base || !*base || !name || !*name)
		return -1;

	tray_lookup_icon_in_theme(base, name, desired_h, best_path, best_diff, found);
	if (*found && *best_diff == 0)
		return 0;

	if (pathisdir(base)) {
		dir = opendir(base);
		if (dir) {
			while ((ent = readdir(dir))) {
				char sub[PATH_MAX];
				if (ent->d_name[0] == '.')
					continue;
				snprintf(sub, sizeof(sub), "%s/%s", base, ent->d_name);
				if (!pathisdir(sub))
					continue;
				tray_lookup_icon_in_theme(sub, name, desired_h, best_path, best_diff, found);
				if (*found && *best_diff == 0) {
					closedir(dir);
					return 0;
				}
			}
			closedir(dir);
		}
	}

	for (size_t e = 0; e < LENGTH(exts); e++) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s.%s", base, name, exts[e]);
		tray_consider_icon(path, desired_h, desired_h, best_path, best_diff, found);
		if (*found && *best_diff == 0)
			return 0;
	}

	return *found ? 0 : -1;
}

/* Parse a separator-delimited env var and add icon paths for each entry */
static void
tray_search_icon_prefix(const char *envval, char sep, const char *suffix,
		const char *pixmap_suffix, const char *themes[], size_t theme_count,
		char pathbufs[][PATH_MAX], size_t *pathcount, size_t maxpaths)
{
	while (envval && *envval && *pathcount < maxpaths) {
		const char *end = strchrnul(envval, sep);
		size_t len = (size_t)(end - envval);
		if (len > 0 && len < PATH_MAX - 20) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%.*s%s", (int)len, envval, suffix);
			add_icon_root_paths(path, themes, theme_count, pathbufs, pathcount, maxpaths);
			if (pixmap_suffix && *pathcount < maxpaths) {
				snprintf(path, sizeof(path), "%.*s%s", (int)len, envval, pixmap_suffix);
				if (*pathcount < maxpaths) {
					snprintf(pathbufs[*pathcount], sizeof(pathbufs[*pathcount]), "%s", path);
					(*pathcount)++;
				}
			}
		}
		if (*end == '\0')
			break;
		envval = end + 1;
	}
}

int
tray_find_icon_path(const char *name, const char *theme_path, int desired_h,
		char *out, size_t outlen)
{
	const char *xdg_data_dirs;
	char best_path[PATH_MAX] = {0};
	char pathbufs[64][PATH_MAX];
	size_t pathcount = 0;
	int found = 0;
	int best_diff = INT_MAX;
	const char *themes[16] = {0};
	size_t theme_count = 0;
	char theme_env[256] = {0};
	const char *default_themes[] = {
		"Papirus", "Papirus-Dark", "Papirus-Light", "Adwaita", "hicolor"
	};

#define ADD_PATH(P) do { \
	const char *p_ = (P); \
	if (p_ && *p_ && pathcount < LENGTH(pathbufs)) { \
		snprintf(pathbufs[pathcount], sizeof(pathbufs[pathcount]), "%s", p_); \
		pathcount++; \
	} \
} while (0)

	if (getenv("STATUSBAR_ICON_THEMES")) {
		snprintf(theme_env, sizeof(theme_env), "%s", getenv("STATUSBAR_ICON_THEMES"));
		for (char *tok = theme_env; tok && *tok && theme_count < LENGTH(themes); ) {
			char *sep = strchr(tok, ':');
			if (sep)
				*sep = '\0';
			if (*tok)
				themes[theme_count++] = tok;
			if (!sep)
				break;
			tok = sep + 1;
		}
	}
	if (theme_count == 0) {
		for (size_t i = 0; i < LENGTH(default_themes) && theme_count < LENGTH(themes); i++)
			themes[theme_count++] = default_themes[i];
	}

	if (!name || !*name || !out || outlen == 0)
		return -1;

	if (name[0] == '/' && access(name, R_OK) == 0) {
		snprintf(out, outlen, "%s", name);
		return 0;
	}

	if (theme_path && *theme_path && pathcount < LENGTH(pathbufs)) {
		ADD_PATH(theme_path);
	}

	{
		const char *xdg_home = getenv("XDG_DATA_HOME");
		const char *home = getenv("HOME");
		if (xdg_home && *xdg_home && pathcount < LENGTH(pathbufs)) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/icons", xdg_home);
			add_icon_root_paths(path, themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));
		} else if (home && *home && pathcount < LENGTH(pathbufs)) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/.local/share/icons", home);
			add_icon_root_paths(path, themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));
		}
		if (home && *home && pathcount < LENGTH(pathbufs)) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/.icons", home);
			add_icon_root_paths(path, themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));
		}
	}

	xdg_data_dirs = getenv("XDG_DATA_DIRS");
	if (!xdg_data_dirs || !*xdg_data_dirs)
		xdg_data_dirs = "/usr/local/share:/usr/share";
	tray_search_icon_prefix(xdg_data_dirs, ':', "/icons", NULL,
			themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));

	tray_search_icon_prefix(getenv("NIX_PROFILES"), ' ',
			"/share/icons", "/share/pixmaps",
			themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));

	if (pathcount < LENGTH(pathbufs)) {
		add_icon_root_paths("/run/current-system/sw/share/icons", themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));
	}
	if (pathcount < LENGTH(pathbufs)) {
		ADD_PATH("/run/current-system/sw/share/pixmaps");
	}

	if (pathcount < LENGTH(pathbufs)) {
		ADD_PATH("/usr/share/pixmaps");
	}

#undef ADD_PATH

	for (size_t i = 0; i < pathcount; i++) {
		tray_lookup_icon_in_root(pathbufs[i], name, desired_h, best_path, &best_diff, &found);
		if (found && best_diff == 0)
			break;
	}

	if (!found)
		return -1;

	snprintf(out, outlen, "%s", best_path);
	return 0;
}

int
tray_get_string_property(TrayItem *it, const char *prop, char *out, size_t outlen)
{
	const char *ifaces[] = {
		"org.kde.StatusNotifierItem",
		"org.freedesktop.StatusNotifierItem",
	};
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	const char *val = NULL;
	int r = -1;

	if (!tray_bus || !it || !prop)
		return -1;

	for (size_t iface_idx = 0; iface_idx < LENGTH(ifaces); iface_idx++) {
		sd_bus_error_free(&err);
		sd_bus_message_unref(reply);
		reply = NULL;

		r = sd_bus_call_method(tray_bus, it->service, it->path,
				"org.freedesktop.DBus.Properties", "Get",
				&err, &reply, "ss", ifaces[iface_idx], prop);
		if (r >= 0)
			break;
	}
	if (r < 0 || !reply)
		goto out;
	if (sd_bus_message_enter_container(reply, 'v', "s") < 0)
		goto out;
	if (sd_bus_message_read(reply, "s", &val) < 0)
		goto out;
	if (val && *val && out && outlen > 0)
		snprintf(out, outlen, "%s", val);

out:
	sd_bus_error_free(&err);
	sd_bus_message_unref(reply);
	return (val && *val) ? 0 : -1;
}

int
tray_item_load_icon(TrayItem *it)
{
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	const void *best_data = NULL;
	size_t best_len = 0;
	int desired_h = statusbar_height > 0 ? MAX(12, (int)statusbar_height - 4) : 24;
	int best_score = INT_MAX;
	int best_w = 0, best_h = 0;
	int r;
	const char *ifaces[] = {
		"org.kde.StatusNotifierItem",
		"org.freedesktop.StatusNotifierItem",
	};
	const char *props[] = { "IconPixmap", "AttentionIconPixmap" };
	int prop_idx;

	if (!tray_bus || !it)
		return -1;
	if (it->icon_tried && it->icon_failed)
		return -1;
	it->icon_tried = 1;

	/* Prefer themed icons first so the user's icon theme takes effect */
	{
		char icon_theme_path[PATH_MAX] = {0};
		char icon_path[PATH_MAX] = {0};
		char icon_name[128] = {0};

		tray_get_string_property(it, "IconThemePath", icon_theme_path,
				sizeof(icon_theme_path));

		if (tray_get_string_property(it, "AttentionIconName", icon_name, sizeof(icon_name)) == 0 &&
				tray_find_icon_path(icon_name, icon_theme_path, desired_h,
					icon_path, sizeof(icon_path)) == 0 &&
				tray_load_icon_file(it, icon_path, desired_h) == 0) {
			it->icon_failed = 0;
			return 0;
		}

		memset(icon_name, 0, sizeof(icon_name));
		memset(icon_path, 0, sizeof(icon_path));
		if (tray_get_string_property(it, "IconName", icon_name, sizeof(icon_name)) == 0 &&
				tray_find_icon_path(icon_name, icon_theme_path, desired_h,
					icon_path, sizeof(icon_path)) == 0 &&
				tray_load_icon_file(it, icon_path, desired_h) == 0) {
			it->icon_failed = 0;
			return 0;
		}
	}

	for (prop_idx = 0; prop_idx < (int)LENGTH(props); prop_idx++) {
		best_data = NULL;
		best_len = 0;
		best_score = INT_MAX;
		best_w = best_h = 0;

		for (size_t iface_idx = 0; iface_idx < LENGTH(ifaces); iface_idx++) {
			sd_bus_error_free(&err);
			sd_bus_message_unref(reply);
			reply = NULL;

			r = sd_bus_call_method(tray_bus, it->service, it->path,
					"org.freedesktop.DBus.Properties", "Get",
					&err, &reply, "ss", ifaces[iface_idx], props[prop_idx]);
			if (r >= 0)
				break;
		}
		if (r < 0)
			continue;

		if (sd_bus_message_enter_container(reply, 'v', "a(iiay)") < 0)
			continue;
		if (sd_bus_message_enter_container(reply, 'a', "(iiay)") < 0)
			continue;

		while (sd_bus_message_enter_container(reply, 'r', "iiay") > 0) {
			int w = 0, h = 0;
			const void *pix = NULL;
			size_t len = 0;
			if (sd_bus_message_read(reply, "ii", &w, &h) < 0) {
				sd_bus_message_exit_container(reply); /* struct */
				break;
			}
			if (sd_bus_message_read_array(reply, 'y', &pix, &len) < 0) {
				sd_bus_message_exit_container(reply); /* struct */
				break;
			}
			if (w > 0 && h > 0 && len >= (size_t)w * (size_t)h * 4) {
				int diff = desired_h > 0 ? abs(h - desired_h) : INT_MAX;
				size_t area = (size_t)w * (size_t)h;
				if ((desired_h > 0 && (diff < best_score ||
						(diff == best_score && h < best_h))) ||
						(desired_h <= 0 && area > best_len)) {
					best_score = diff;
					best_len = area;
					best_w = w;
					best_h = h;
					best_data = pix;
				}
			}
			sd_bus_message_exit_container(reply); /* struct */
		}

		sd_bus_message_exit_container(reply);
		sd_bus_message_exit_container(reply);

		if (best_data && best_w > 0 && best_h > 0)
			break;
	}

	if (best_data && best_w > 0 && best_h > 0) {
		int target_h = (desired_h > 0) ? desired_h : best_h;
		struct wlr_buffer *buf = NULL;

		if (best_h > target_h && target_h > 0)
			buf = statusbar_scaled_buffer_from_argb32(best_data, best_w, best_h, target_h);
		else
			buf = statusbar_buffer_from_argb32(best_data, best_w, best_h);
		if (buf) {
			if (it->icon_buf)
				wlr_buffer_drop(it->icon_buf);
			it->icon_buf = buf;
			if (target_h > 0 && best_h > target_h) {
				it->icon_w = (int)lround(((double)best_w * (double)target_h) / (double)best_h);
				it->icon_h = target_h;
			} else {
				it->icon_w = best_w;
				it->icon_h = best_h;
			}
			sd_bus_message_unref(reply);
			sd_bus_error_free(&err);
			it->icon_failed = 0;
			return 0;
		}
	}

	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
	it->icon_failed = 1;
	return -1;
}

void
tray_emit_host_registered(void)
{
	if (!tray_bus)
		return;
	sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher",
			"StatusNotifierHostRegistered", "");
	sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
			"org.freedesktop.StatusNotifierWatcher",
			"StatusNotifierHostRegistered", "");
}

int
tray_search_item_path(const char *service, const char *start_path,
		char *out, size_t outlen, int depth)
{
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	const char *xml;
	const char *p;
	int r = -1;

	if (!tray_bus || !service || !start_path || !out || outlen == 0 || depth <= 0)
		return -EINVAL;

	r = sd_bus_call_method(tray_bus, service, start_path,
			"org.freedesktop.DBus.Introspectable", "Introspect",
			&err, &reply, "");
	if (r < 0)
		goto out;

	if (sd_bus_message_read(reply, "s", &xml) < 0 || !xml) {
		r = -EINVAL;
		goto out;
	}

	if (strstr(xml, "org.kde.StatusNotifierItem")
			|| strstr(xml, "org.freedesktop.StatusNotifierItem")) {
		snprintf(out, outlen, "%s", start_path);
		r = 0;
		goto out;
	}

	if (depth <= 1) {
		r = 1;
		goto out;
	}

	p = xml;
	while ((p = strstr(p, "<node name=\""))) {
		char name[128];
		char child[256];
		const char *end;
		size_t nlen;

		p += strlen("<node name=\"");
		end = strchr(p, '"');
		if (!end)
			break;
		nlen = (size_t)(end - p);
		if (nlen == 0 || nlen >= sizeof(name)) {
			p = end ? end + 1 : p + 1;
			continue;
		}
		memcpy(name, p, nlen);
		name[nlen] = '\0';

		if (strcmp(start_path, "/") == 0)
			snprintf(child, sizeof(child), "/%s", name);
		else
			snprintf(child, sizeof(child), "%s/%s", start_path, name);

		if (tray_search_item_path(service, child, out, outlen, depth - 1) == 0) {
			r = 0;
			goto out;
		}
		p = end + 1;
	}

out:
	sd_bus_error_free(&err);
	sd_bus_message_unref(reply);
	return r;
}

int
tray_find_item_path(const char *service, char *path, size_t pathlen)
{
	static const char *candidates[] = {
		"/StatusNotifierItem",
		"/org/ayatana/NotificationItem",
	};

	if (!service || !path || pathlen == 0)
		return -EINVAL;

	for (size_t i = 0; i < LENGTH(candidates); i++) {
		if (tray_search_item_path(service, candidates[i], path, pathlen, 1) == 0)
			return 0;
	}

	if (tray_search_item_path(service, "/", path, pathlen, 6) == 0)
		return 0;

	snprintf(path, pathlen, "%s", "/StatusNotifierItem");
	return -1;
}

void
tray_sanitize_label(const char *src, char *dst, size_t len)
{
	size_t di = 0;
	int in_tag = 0;

	if (!dst || len == 0) {
		return;
	}

	dst[0] = '\0';
	if (!src)
		return;

	for (size_t i = 0; src[i] && di + 1 < len; i++) {
		char c = src[i];
		if (c == '<') {
			in_tag = 1;
			continue;
		}
		if (in_tag) {
			if (c == '>')
				in_tag = 0;
			continue;
		}
		if (c == '_' || c == '&')
			continue;
		dst[di++] = c;
	}

	while (di > 0 && (dst[di - 1] == ' ' || dst[di - 1] == '\t'))
		di--;
	dst[di] = '\0';
}

void
tray_menu_clear(TrayMenu *menu)
{
	TrayMenuEntry *e, *tmp;

	if (!menu)
		return;

	wl_list_for_each_safe(e, tmp, &menu->entries, link) {
		wl_list_remove(&e->link);
		free(e);
	}
	wl_list_init(&menu->entries);

	menu->width = menu->height = 0;
	menu->x = menu->y = 0;
	menu->visible = 0;
	menu->service[0] = '\0';
	menu->menu_path[0] = '\0';

	if (menu->tree)
		wlr_scene_node_set_enabled(&menu->tree->node, 0);
}

void
tray_menu_hide(Monitor *m)
{
	if (!m)
		return;
	tray_menu_clear(&m->statusbar.tray_menu);
}

void
tray_menu_hide_all(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link)
		tray_menu_hide(m);
}

int
tray_item_get_menu_path(TrayItem *it)
{
	static const char *ifaces[] = {
		"org.kde.StatusNotifierItem",
		"org.freedesktop.StatusNotifierItem",
	};
	const char *item_path;
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	const char *path = NULL;
	int r = -1;

	if (!tray_bus || !it)
		return -EINVAL;

	it->has_menu = 0;
	it->menu[0] = '\0';
	item_path = it->path[0] ? it->path : "/StatusNotifierItem";

	for (size_t i = 0; i < LENGTH(ifaces); i++) {
		sd_bus_error_free(&err);
		reply = NULL;
		r = sd_bus_call_method(tray_bus, it->service,
				item_path,
				"org.freedesktop.DBus.Properties", "Get",
				&err, &reply, "ss", ifaces[i], "Menu");
		if (r < 0)
			continue;
		if (sd_bus_message_enter_container(reply, 'v', "o") < 0) {
			sd_bus_message_unref(reply);
			continue;
		}
		if (sd_bus_message_read(reply, "o", &path) < 0) {
			sd_bus_message_exit_container(reply);
			sd_bus_message_unref(reply);
			continue;
		}
		sd_bus_message_exit_container(reply);
		if (path && path[0] && strcmp(path, "/") != 0) {
			snprintf(it->menu, sizeof(it->menu), "%s", path);
			it->has_menu = 1;
			sd_bus_message_unref(reply);
			sd_bus_error_free(&err);
			return 0;
		}
		wlr_log(WLR_ERROR, "tray: service %s Menu property empty/invalid", it->service);
		sd_bus_message_unref(reply);
	}

	sd_bus_error_free(&err);
	wlr_log(WLR_ERROR, "tray: failed to get Menu property for %s: %s",
			it->service, strerror(-r));
	return -1;
}

int
tray_menu_parse_node(sd_bus_message *msg, TrayMenu *menu, int depth, int max_depth)
{
	int r;

	if (!msg || !menu)
		return -EINVAL;

	r = sd_bus_message_enter_container(msg, 'r', "ia{sv}av");
	if (r < 0)
		return r;
	if (r == 0)
		return 0;
	if (depth < 0 || depth > max_depth + 4) /* sanity */
		return -EINVAL;

	r = tray_menu_parse_node_body(msg, menu, depth, max_depth);
	sd_bus_message_exit_container(msg);
	return r;
}

int
tray_menu_parse_node_body(sd_bus_message *msg, TrayMenu *menu, int depth, int max_depth)
{
	TrayMenuEntry *entry;
	int id = 0, r = 0;
	int enabled = 1, visible = 1;
	int is_separator = 0, has_submenu = 0;
	int toggle_type = 0, toggle_state = 0;
	int child_count = 0;
	char label[256] = {0};

	if (!msg || !menu)
		return -EINVAL;
	if (depth > max_depth + 4)
		return -EINVAL;

	if (sd_bus_message_read(msg, "i", &id) < 0)
		return -EINVAL;

	r = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (r < 0)
		return r;
	while ((r = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		const char *key = NULL;
		if (sd_bus_message_read(msg, "s", &key) < 0) {
			sd_bus_message_exit_container(msg);
			r = -EINVAL;
			break;
		}
		if (!key) {
			sd_bus_message_skip(msg, "v");
		} else if (strcmp(key, "label") == 0) {
			const char *val = NULL;
			if (sd_bus_message_enter_container(msg, 'v', "s") >= 0) {
				if (sd_bus_message_read(msg, "s", &val) >= 0 && val)
					tray_sanitize_label(val, label, sizeof(label));
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (strcmp(key, "enabled") == 0) {
			int v = 1;
			if (sd_bus_message_enter_container(msg, 'v', "b") >= 0) {
				sd_bus_message_read(msg, "b", &v);
				enabled = v;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (strcmp(key, "visible") == 0) {
			int v = 1;
			if (sd_bus_message_enter_container(msg, 'v', "b") >= 0) {
				sd_bus_message_read(msg, "b", &v);
				visible = v;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (strcmp(key, "type") == 0) {
			const char *val = NULL;
			if (sd_bus_message_enter_container(msg, 'v', "s") >= 0) {
				if (sd_bus_message_read(msg, "s", &val) >= 0 && val
						&& strcmp(val, "separator") == 0)
					is_separator = 1;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (strcmp(key, "toggle-type") == 0) {
			const char *val = NULL;
			if (sd_bus_message_enter_container(msg, 'v', "s") >= 0) {
				if (sd_bus_message_read(msg, "s", &val) >= 0 && val) {
					if (strcmp(val, "checkmark") == 0)
						toggle_type = 1;
					else if (strcmp(val, "radio") == 0)
						toggle_type = 2;
				}
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (strcmp(key, "toggle-state") == 0) {
			int32_t state = 0;
			if (sd_bus_message_enter_container(msg, 'v', "i") >= 0) {
				sd_bus_message_read(msg, "i", &state);
				if (state < 0)
					state = 0;
				toggle_state = state;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (strcmp(key, "children-display") == 0) {
			const char *val = NULL;
			if (sd_bus_message_enter_container(msg, 'v', "s") >= 0) {
				if (sd_bus_message_read(msg, "s", &val) >= 0 && val
						&& strcmp(val, "submenu") == 0)
					has_submenu = 1;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else {
			sd_bus_message_skip(msg, "v");
		}
		sd_bus_message_exit_container(msg);
	}
	sd_bus_message_exit_container(msg);
	if (r < 0)
		return r;

	{
		r = sd_bus_message_enter_container(msg, 'a', "(ia{sv}av)");
		if (r < 0)
			return r;
		while ((r = sd_bus_message_enter_container(msg, 'r', "ia{sv}av")) > 0) {
			child_count++;
			if (depth < max_depth) {
				int cr = tray_menu_parse_node_body(msg, menu, depth + 1, max_depth);
				if (cr < 0) {
					sd_bus_message_exit_container(msg);
					r = cr;
					break;
				}
			} else {
				if (sd_bus_message_skip(msg, "ia{sv}av") < 0) {
					sd_bus_message_exit_container(msg);
					r = -EINVAL;
					break;
				}
			}
			sd_bus_message_exit_container(msg);
		}
		sd_bus_message_exit_container(msg);
		if (r < 0)
			return r;
	}

	if (!visible || depth == 0 || depth > max_depth)
		return 0;

	entry = ecalloc(1, sizeof(*entry));
	entry->id = id;
	entry->enabled = enabled;
	entry->is_separator = is_separator;
	entry->depth = depth - 1;
	entry->has_submenu = has_submenu || child_count > 0;
	entry->toggle_type = toggle_type;
	entry->toggle_state = toggle_state;
	if (label[0])
		snprintf(entry->label, sizeof(entry->label), "%s", label);

	wl_list_insert(menu->entries.prev, &entry->link);
	return 0;
}

void
tray_menu_draw_text(struct wlr_scene_tree *tree, const char *text, int x, int y, int row_h)
{
	tll(struct GlyphRun) glyphs = tll_init();
	const struct fcft_glyph *glyph;
	struct wlr_scene_buffer *scene_buf;
	struct wlr_buffer *buffer;
	uint32_t prev_cp = 0;
	int pen_x = 0;
	int min_y = INT_MAX, max_y = INT_MIN;

	if (!tree || !text || !*text || row_h <= 0)
		return;
	if (!statusfont.font)
		return;

	for (size_t i = 0; text[i]; i++) {
		long kern_x = 0, kern_y = 0;
		uint32_t cp = (unsigned char)text[i];

		if (prev_cp)
			fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
		pen_x += (int)kern_x;

		glyph = fcft_rasterize_char_utf32(statusfont.font, cp, statusbar_font_subpixel);
		if (glyph && glyph->pix) {
			tll_push_back(glyphs, ((struct GlyphRun){
				.glyph = glyph,
				.pen_x = pen_x,
				.codepoint = cp,
			}));
			if (-glyph->y < min_y)
				min_y = -glyph->y;
			if (-glyph->y + glyph->height > max_y)
				max_y = -glyph->y + glyph->height;
			pen_x += glyph->advance.x;
			if (text[i + 1])
				pen_x += statusbar_font_spacing;
		}
		prev_cp = cp;
	}

	if (tll_length(glyphs) == 0) {
		tll_free(glyphs);
		return;
	}

	{
		int text_height = max_y - min_y;
		int origin_y = y + (row_h - text_height) / 2 - min_y;

		tll_foreach(glyphs, it) {
			glyph = it->item.glyph;
			buffer = statusbar_buffer_from_glyph(glyph);
			if (!buffer)
				continue;

			scene_buf = wlr_scene_buffer_create(tree, NULL);
			if (scene_buf) {
				wlr_scene_buffer_set_buffer(scene_buf, buffer);
				wlr_scene_node_set_position(&scene_buf->node,
						x + it->item.pen_x + glyph->x,
						origin_y - glyph->y);
			}
			wlr_buffer_drop(buffer);
		}
	}

	tll_free(glyphs);
}

void
tray_menu_render(Monitor *m)
{
	TrayMenu *menu;
	struct wlr_scene_node *node, *tmp;
	TrayMenuEntry *entry;
	int padding, line_spacing, indent_w;
	int max_width = 0;
	int total_height;
	int row_h;

	if (!m || !m->statusbar.tray_menu.tree)
		return;
	menu = &m->statusbar.tray_menu;

	padding = statusbar_module_padding;
	line_spacing = 4;
	indent_w = statusbar_font_spacing > 0 ? statusbar_font_spacing * 2 : 10;

	wl_list_for_each_safe(node, tmp, &menu->tree->children, link) {
		if (menu->bg && node == &menu->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!statusfont.font || wl_list_empty(&menu->entries)) {
		menu->width = menu->height = 0;
		wlr_scene_node_set_enabled(&menu->tree->node, 0);
		menu->visible = 0;
		return;
	}

	row_h = statusfont.height + line_spacing;
	if (row_h < statusfont.height)
		row_h = statusfont.height;
	total_height = padding * 2;

	wl_list_for_each(entry, &menu->entries, link) {
		int text_w = entry->is_separator ? 0 : status_text_width(
				entry->label[0] ? entry->label : " ");
		int row_width = text_w + 2 * padding + indent_w * entry->depth;
		if (entry->toggle_type)
			row_width += row_h;
		if (entry->has_submenu)
			row_width += row_h / 2;
		max_width = MAX(max_width, row_width);
		total_height += entry->is_separator ? line_spacing : row_h;
	}

	if (max_width <= 0)
		max_width = 80;

	menu->width = max_width;
	menu->height = total_height;

	if (!menu->bg && !(menu->bg = wlr_scene_tree_create(menu->tree)))
		return;
	wlr_scene_node_set_enabled(&menu->bg->node, 1);
	wlr_scene_node_set_position(&menu->bg->node, 0, 0);
	wl_list_for_each_safe(node, tmp, &menu->bg->children, link)
		wlr_scene_node_destroy(node);
	drawrect(menu->bg, 0, 0, menu->width, menu->height, statusbar_bg);

	{
		int y = padding;
		wl_list_for_each(entry, &menu->entries, link) {
			char text[512];
			int row = entry->is_separator ? line_spacing : row_h;
			int x = padding + indent_w * entry->depth;

			entry->y = y;
			entry->height = row;

			if (entry->is_separator) {
				int sep_y = y + row / 2;
				drawrect(menu->tree, padding, sep_y, menu->width - 2 * padding, 1, statusbar_fg);
				y += row;
				continue;
			}

			text[0] = '\0';
			if (entry->toggle_type == 1) {
				snprintf(text, sizeof(text), "%s%s%s",
						entry->toggle_state ? "[x] " : "[ ] ",
						entry->label[0] ? entry->label : "",
						entry->has_submenu ? "  >" : "");
			} else if (entry->toggle_type == 2) {
				snprintf(text, sizeof(text), "%s%s%s",
						entry->toggle_state ? "(o) " : "( ) ",
						entry->label[0] ? entry->label : "",
						entry->has_submenu ? "  >" : "");
			} else {
				snprintf(text, sizeof(text), "%s%s",
						entry->label[0] ? entry->label : " ",
						entry->has_submenu ? "  >" : "");
			}

			tray_menu_draw_text(menu->tree, text, x, y, row);
			y += row;
		}
	}
}

__attribute__((unused)) int
tray_menu_open_at(Monitor *m, TrayItem *it, int icon_x)
{
	sd_bus_message *req = NULL, *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	char *props[] = {"label", "enabled", "type", "children-display",
		"toggle-type", "toggle-state", "visible", NULL};
	int r;
	int max_depth = 3; /* limited depth for safety; can be raised if needed */
	TrayMenu *menu;
	int desired_x, max_x;
	const char *sig;

	if (!m || !m->showbar || !m->statusbar.tray_menu.tree || !it || !tray_bus)
		return 0;
	if (!m->statusbar.area.width || !m->statusbar.area.height)
		return 0;

	if (tray_item_get_menu_path(it) != 0 || !it->has_menu)
		return 0;

	menu = &m->statusbar.tray_menu;
	tray_menu_hide_all();
	tray_menu_clear(menu);
	wl_list_init(&menu->entries);
	if (!menu->tree) {
		return 0;
	}
	if (max_depth < 1)
		max_depth = 1;

	/* Temporarily disable custom menu parsing to avoid crashes; fall back to client ContextMenu */
	return 0;

	r = sd_bus_message_new_method_call(tray_bus, &req, it->service, it->menu,
			"com.canonical.dbusmenu", "GetLayout");
	if (r < 0)
		goto fail;

	r = sd_bus_message_append(req, "iias", 0, max_depth, props);
	if (r < 0)
		goto fail;

	r = sd_bus_call(tray_bus, req, 2 * 1000 * 1000, &err, &reply);
	if (r < 0)
		goto fail;

	sig = sd_bus_message_get_signature(reply, 0);
	if (!sig || !strstr(sig, "(ia{sv}av)")) {
		r = -EINVAL;
		goto fail;
	}

	{
		int revision = 0;
		if (sd_bus_message_read(reply, "i", &revision) < 0)
			goto fail;
		(void)revision;
		if ((r = tray_menu_parse_node(reply, menu, 0, max_depth)) < 0)
			goto fail;
	}

	sd_bus_message_unref(req);
	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);

	if (wl_list_empty(&menu->entries))
		goto fail;

	snprintf(menu->service, sizeof(menu->service), "%s", it->service);
	snprintf(menu->menu_path, sizeof(menu->menu_path), "%s", it->menu);

	tray_menu_render(m);
	if (menu->width <= 0 || menu->height <= 0)
		goto fail;

	desired_x = icon_x - menu->width / 2;
	if (desired_x < 0)
		desired_x = 0;
	max_x = m->statusbar.area.width - menu->width;
	if (max_x < 0)
		max_x = 0;
	if (desired_x > max_x)
		desired_x = max_x;

	menu->x = desired_x;
	menu->y = m->statusbar.area.height;
	wlr_scene_node_set_position(&menu->tree->node, menu->x, menu->y);
	wlr_scene_node_set_enabled(&menu->tree->node, 1);
	menu->visible = 1;
	return 1;

fail:
	wlr_log(WLR_ERROR, "tray: GetLayout failed for %s%s: %s",
			it->service, it->menu, err.message ? err.message : strerror(-r));
	if (req)
		sd_bus_message_unref(req);
	if (reply)
		sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
	tray_menu_hide_all();
	return 0;
}

TrayMenuEntry *
tray_menu_entry_at(Monitor *m, int lx, int ly)
{
	TrayMenuEntry *entry;
	TrayMenu *menu;

	if (!m)
		return NULL;
	menu = &m->statusbar.tray_menu;
	wl_list_for_each(entry, &menu->entries, link) {
		if (ly >= entry->y && ly < entry->y + entry->height) {
			if (entry->is_separator || !entry->enabled)
				return NULL;
			return entry;
		}
	}
	return NULL;
}

int
tray_menu_send_event(TrayMenu *menu, TrayMenuEntry *entry, uint32_t time_msec)
{
	sd_bus_message *msg = NULL, *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	int r;

	if (!tray_bus || !menu || !entry || !menu->service[0] || !menu->menu_path[0])
		return -EINVAL;

	r = sd_bus_message_new_method_call(tray_bus, &msg, menu->service, menu->menu_path,
			"com.canonical.dbusmenu", "Event");
	if (r < 0)
		goto out;

	r = sd_bus_message_append(msg, "is", entry->id, "clicked");
	if (r < 0)
		goto out;

	r = sd_bus_message_open_container(msg, 'v', "s");
	if (r < 0)
		goto out;
	r = sd_bus_message_append_basic(msg, 's', "");
	if (r < 0)
		goto out;
	r = sd_bus_message_close_container(msg);
	if (r < 0)
		goto out;

	r = sd_bus_message_append(msg, "u", time_msec);
	if (r < 0)
		goto out;

	r = sd_bus_call(tray_bus, msg, 2 * 1000 * 1000, &err, &reply);

out:
	if (msg)
		sd_bus_message_unref(msg);
	if (reply)
		sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
	return r < 0 ? r : 0;
}

void
tray_add_item(const char *service, const char *path, int emit_signals)
{
	const char *base;
	TrayItem *it;

	if (!service || !service[0] || !path || !path[0])
		return;

	tray_remove_item(service);
	it = ecalloc(1, sizeof(*it));
	snprintf(it->service, sizeof(it->service), "%s", service);
	snprintf(it->path, sizeof(it->path), "%s", path);
	base = strrchr(service, '.');
	base = base ? base + 1 : service;
	snprintf(it->label, sizeof(it->label), "%s", base);
	it->icon_tried = 0;
	it->icon_failed = 0;
	wl_list_insert(&tray_items, &it->link);
	wlr_log(WLR_INFO, "tray: registered %s%s", service, path);

	tray_update_icons_text();

	if (!emit_signals || !tray_bus)
		return;

	sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher",
			"StatusNotifierItemRegistered", "s", service);
	sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
			"org.freedesktop.StatusNotifierWatcher",
			"StatusNotifierItemRegistered", "s", service);
}

int
tray_method_register_item(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	const char *arg = NULL;
	const char *sender = sd_bus_message_get_sender(m);
	char service[128] = {0};
	char path[128] = {0};
	sd_bus_error err = SD_BUS_ERROR_NULL;

	(void)userdata;
	(void)ret_error;

	if (sd_bus_message_read(m, "s", &arg) < 0) {
		sd_bus_error_set_const(&err, SD_BUS_ERROR_INVALID_ARGS, "invalid arg");
		return sd_bus_reply_method_error(m, &err);
	}
	if (!sender || !sender[0]) {
		sd_bus_error_set_const(&err, SD_BUS_ERROR_FAILED, "no sender");
		return sd_bus_reply_method_error(m, &err);
	}

	if (arg && strchr(arg, '/')) {
		snprintf(service, sizeof(service), "%s", sender);
		snprintf(path, sizeof(path), "%s", arg);
	} else {
		snprintf(service, sizeof(service), "%s", arg && arg[0] ? arg : sender);
		snprintf(path, sizeof(path), "/StatusNotifierItem");
	}

	if (tray_find_item_path(service, path, sizeof(path)) == 0) {
		/* path updated by helper */
	}

	tray_add_item(service, path, 1);
	tray_emit_host_registered();
	return sd_bus_reply_method_return(m, "");
}

int
tray_method_register_host(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	(void)userdata;
	(void)ret_error;
	tray_host_registered = 1;
	tray_emit_host_registered();
	return sd_bus_reply_method_return(m, "");
}

void
tray_scan_existing_items(void)
{
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	const char *name;

	if (!tray_bus)
		return;

	if (sd_bus_call_method(tray_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
				"org.freedesktop.DBus", "ListNames", &err, &reply, "") < 0)
		goto out;
	if (sd_bus_message_enter_container(reply, 'a', "s") < 0)
		goto out;

	while (sd_bus_message_read_basic(reply, 's', &name) > 0) {
		if (!name)
			continue;
		if (strstr(name, "StatusNotifierItem") || strstr(name, "NotificationItem")) {
			char path[128];
			if (tray_find_item_path(name, path, sizeof(path)) != 0)
				snprintf(path, sizeof(path), "%s", "/StatusNotifierItem");
			tray_add_item(name, path, 1);
		}
	}

	sd_bus_message_exit_container(reply);
out:
	sd_bus_error_free(&err);
	sd_bus_message_unref(reply);
}

int
tray_property_get_registered(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	TrayItem *it;

	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;

	if (sd_bus_message_open_container(reply, 'a', "s") < 0)
		return -EINVAL;

	wl_list_for_each(it, &tray_items, link) {
		char full[256];
		snprintf(full, sizeof(full), "%s%s", it->service, it->path);
		sd_bus_message_append_basic(reply, 's', full);
	}

	return sd_bus_message_close_container(reply);
}

int
tray_property_get_host_registered(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append_basic(reply, 'b', &tray_host_registered);
}

int
tray_property_get_protocol_version(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	int version = 1;

	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append_basic(reply, 'i', &version);
}

int
tray_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	const char *name, *old_owner, *new_owner;

	(void)userdata;
	(void)ret_error;
	if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0)
		return 0;
	if (name && new_owner && *new_owner &&
			(strstr(name, "StatusNotifierItem") || strstr(name, "NotificationItem"))) {
		char path[128];
		if (tray_find_item_path(name, path, sizeof(path)) != 0)
			snprintf(path, sizeof(path), "%s", "/StatusNotifierItem");
		tray_add_item(name, path, 1);
		return 0;
	}
	if (name && old_owner && *old_owner && (!new_owner || !*new_owner))
		tray_remove_item(name);
	return 0;
}

int
tray_bus_event(int fd, uint32_t mask, void *data)
{
	sd_bus *bus = data;
	int r;
	int events;
	uint32_t newmask;

	(void)fd;
	if (!bus)
		return 0;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		return 0;

	while ((r = sd_bus_process(bus, NULL)) > 0)
		;

	events = sd_bus_get_events(bus);
	newmask = 0;
	if (events & SD_BUS_EVENT_READABLE)
		newmask |= WL_EVENT_READABLE;
	if (events & SD_BUS_EVENT_WRITABLE)
		newmask |= WL_EVENT_WRITABLE;
	if (tray_event)
		wl_event_source_fd_update(tray_event, newmask ? newmask : WL_EVENT_READABLE);

	return 0;
}

void
tray_init(void)
{
	static const sd_bus_vtable tray_vtable[] = {
		SD_BUS_VTABLE_START(0),
		SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", tray_method_register_item, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", tray_method_register_host, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", tray_property_get_registered, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
		SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", tray_property_get_host_registered, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
		SD_BUS_PROPERTY("ProtocolVersion", "i", tray_property_get_protocol_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
		SD_BUS_VTABLE_END
	};
	int r;
	int fd;
	int events;
	uint32_t mask = WL_EVENT_READABLE;
	uint64_t name_flags = SD_BUS_NAME_ALLOW_REPLACEMENT | SD_BUS_NAME_REPLACE_EXISTING;

	if (tray_bus)
		return;

	r = sd_bus_open_user(&tray_bus);
	if (r < 0) {
		wlr_log(WLR_ERROR, "tray: failed to connect to session bus: %s", strerror(-r));
		tray_bus = NULL;
		return;
	}

	/* Keep icon/property queries from stalling the compositor for too long */
	sd_bus_set_method_call_timeout(tray_bus, 2 * 1000 * 1000); /* 2s */

	r = sd_bus_request_name(tray_bus, "org.kde.StatusNotifierWatcher", name_flags);
	if (r < 0) {
		wlr_log(WLR_ERROR, "tray: failed to acquire watcher name: %s", strerror(-r));
		goto fail;
	}
	/* Some implementations also look for the freedesktop alias */
	sd_bus_request_name(tray_bus, "org.freedesktop.StatusNotifierWatcher", name_flags);
	sd_bus_add_object_vtable(tray_bus, &tray_vtable_slot, "/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher", tray_vtable, NULL);
	sd_bus_add_object_vtable(tray_bus, &tray_fdo_vtable_slot, "/StatusNotifierWatcher",
			"org.freedesktop.StatusNotifierWatcher", tray_vtable, NULL);
	sd_bus_add_match(tray_bus, &tray_name_slot,
		"type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
		tray_name_owner_changed, NULL);

	fd = sd_bus_get_fd(tray_bus);
	events = sd_bus_get_events(tray_bus);
	mask = 0;
	if (events & SD_BUS_EVENT_READABLE)
		mask |= WL_EVENT_READABLE;
	if (events & SD_BUS_EVENT_WRITABLE)
		mask |= WL_EVENT_WRITABLE;
	if (mask == 0)
		mask = WL_EVENT_READABLE;

	tray_event = wl_event_loop_add_fd(event_loop, fd, mask, tray_bus_event, tray_bus);
	tray_scan_existing_items();
	tray_host_registered = 1;
	tray_emit_host_registered();
	tray_update_icons_text();
	return;
fail:
	if (tray_vtable_slot)
		sd_bus_slot_unref(tray_vtable_slot);
	tray_vtable_slot = NULL;
	if (tray_fdo_vtable_slot)
		sd_bus_slot_unref(tray_fdo_vtable_slot);
	tray_fdo_vtable_slot = NULL;
	if (tray_name_slot)
		sd_bus_slot_unref(tray_name_slot);
	tray_name_slot = NULL;
	if (tray_event) {
		wl_event_source_remove(tray_event);
		tray_event = NULL;
	}
	if (tray_bus) {
		sd_bus_unref(tray_bus);
		tray_bus = NULL;
	}
}

void
tray_remove_item(const char *service)
{
	TrayItem *it, *tmp;
	Monitor *m;

	if (!service)
		return;

	wl_list_for_each(m, &mons, link) {
		if (strcmp(m->statusbar.tray_menu.service, service) == 0)
			tray_menu_hide(m);
	}

	wl_list_for_each_safe(it, tmp, &tray_items, link) {
		if (strcmp(it->service, service) == 0) {
			wl_list_remove(&it->link);
			if (it->icon_buf)
				wlr_buffer_drop(it->icon_buf);
			free(it);
			tray_update_icons_text();
			sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
					"org.kde.StatusNotifierWatcher",
					"StatusNotifierItemUnregistered", "s", service);
			sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
					"org.freedesktop.StatusNotifierWatcher",
					"StatusNotifierItemUnregistered", "s", service);
			return;
		}
	}
}

__attribute__((unused)) TrayItem *
tray_first_item(void)
{
	TrayItem *sample = NULL;

	if (wl_list_empty(&tray_items))
		return NULL;
	return wl_container_of(tray_items.next, sample, link);
}

__attribute__((unused)) void
tray_item_activate(TrayItem *it, int button, int context_menu, int x, int y)
{
	const char *method;
	const char *ifaces[] = {
		"org.kde.StatusNotifierItem",
		"org.freedesktop.StatusNotifierItem",
	};
	const char *item_path;
	int r = -1;

	if (!tray_bus || !it)
		return;
	if (context_menu)
		method = "ContextMenu";
	else if (button == BTN_MIDDLE)
		method = "SecondaryActivate";
	else
		method = "Activate";
	tray_anchor_x = x;
	tray_anchor_y = y;
	tray_anchor_time_ms = monotonic_msec();
	if (context_menu) {
		/* ensure transient menus are placed near the icon even if GetLayout fails */
		tray_anchor_x = x;
		tray_anchor_y = y;
	}
	item_path = it->path[0] ? it->path : "/StatusNotifierItem";
	for (int attempt = 0; attempt < 2; attempt++) {
		for (size_t i = 0; i < LENGTH(ifaces); i++) {
			r = sd_bus_call_method(tray_bus, it->service, item_path,
					ifaces[i], method, NULL, NULL, "ii", x, y);
			if (r >= 0)
				return;
		}
		/* If bus is broken, try to re-init once */
		if (r == -EBADFD || r == -EPIPE || r == -ENOTCONN) {
			tray_init();
			if (!tray_bus)
				break;
		} else {
			break;
		}
	}
	wlr_log(WLR_ERROR, "tray: %s %s failed on %s%s: %s",
			method, it->service, item_path, it->path[0] ? "" : "(null)", strerror(-r));
}

void
tray_update_icons_text(void)
{
	TrayItem *it;
	size_t off = 0;
	int running_width;
	char segment[256];

	snprintf(sysicons_text, sizeof(sysicons_text), "Tray");
	off = strlen(sysicons_text);
	if (off >= sizeof(sysicons_text))
		off = sizeof(sysicons_text) - 1;
	running_width = status_text_width(sysicons_text);

	wl_list_for_each(it, &tray_items, link) {
		const char *label = it->label[0] ? it->label : it->service;
		int n;
		it->x = running_width;
		if (off ? snprintf(segment, sizeof(segment), " %s", label)
				: snprintf(segment, sizeof(segment), "%s", label))
			it->w = status_text_width(segment);
		else
			it->w = status_text_width(label);
		if (it->w <= 0)
			it->w = statusbar_font_spacing;
		n = snprintf(sysicons_text + off, sizeof(sysicons_text) - off,
				off ? " %s" : "%s", label);
		if (n < 0)
			break;
		if ((size_t)n >= sizeof(sysicons_text) - off) {
			sysicons_text[sizeof(sysicons_text) - 1] = '\0';
			break;
		}
		off += (size_t)n;
		running_width += it->w;
	}

	refreshstatusicons();
}


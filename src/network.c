/* network.c - Auto-extracted from nixlytile.c */
#include "nixlytile.h"
#include "client.h"

void
request_public_ip_async_ex(int force)
{
	const char *cmd;
	int pipefd[2] = {-1, -1};
	time_t now = time(NULL);

	if (now == (time_t)-1)
		return;

	/* Skip rate limit if force is set (real-time update when hovering) */
	if (!force && net_public_ip_last != 0 && (now - net_public_ip_last) < 300)
		return;

	if (public_ip_pid > 0 || public_ip_event)
		return;

	cmd = getenv("NIXLYTILE_PUBLIC_IP_CMD");
	if (!cmd || !cmd[0])
		cmd = "curl -4 -s https://ifconfig.me";

	if (pipe(pipefd) != 0)
		return;

	public_ip_pid = fork();
	if (public_ip_pid == 0) {
		/* child */
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		_exit(127);
	} else if (public_ip_pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		public_ip_pid = -1;
		return;
	}

	/* parent */
	close(pipefd[1]);
	public_ip_fd = pipefd[0];
	fcntl(public_ip_fd, F_SETFL, fcntl(public_ip_fd, F_GETFL) | O_NONBLOCK);
	public_ip_len = 0;
	public_ip_buf[0] = '\0';
	net_public_ip_last = now; /* rate-limit even if fetch fails */
	public_ip_event = wl_event_loop_add_fd(event_loop, public_ip_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP,
			public_ip_event_cb, NULL);
	if (!public_ip_event)
		stop_public_ip_fetch();
}

void
request_public_ip_async(void)
{
	request_public_ip_async_ex(0);
}

void
stop_public_ip_fetch(void)
{
	if (public_ip_event) {
		wl_event_source_remove(public_ip_event);
		public_ip_event = NULL;
	}
	if (public_ip_fd >= 0) {
		close(public_ip_fd);
		public_ip_fd = -1;
	}
	if (public_ip_pid > 0) {
		waitpid(public_ip_pid, NULL, WNOHANG);
		public_ip_pid = -1;
	}
	public_ip_len = 0;
	public_ip_buf[0] = '\0';
}

int
public_ip_event_cb(int fd, uint32_t mask, void *data)
{
	ssize_t n;
	(void)data;

	(void)mask;

	for (;;) {
		if (public_ip_len >= sizeof(public_ip_buf) - 1)
			break;
		n = read(fd, public_ip_buf + public_ip_len,
				sizeof(public_ip_buf) - 1 - public_ip_len);
		if (n > 0) {
			public_ip_len += (size_t)n;
			continue;
		} else if (n == 0) {
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		} else {
			break;
		}
	}

	public_ip_buf[public_ip_len] = '\0';
	for (size_t i = 0; i < public_ip_len; i++) {
		if (public_ip_buf[i] == '\n' || public_ip_buf[i] == '\r') {
			public_ip_buf[i] = '\0';
			break;
		}
	}
	if (public_ip_buf[0])
		snprintf(net_public_ip, sizeof(net_public_ip), "%s", public_ip_buf);
	else if (!net_public_ip[0])
		snprintf(net_public_ip, sizeof(net_public_ip), "--");
	net_public_ip_last = time(NULL);
	stop_public_ip_fetch();
	return 0;
}

void
request_ssid_async(const char *iface)
{
	int pipefd[2] = {-1, -1};
	char cmd[256];
	const char *iw = "/run/current-system/sw/bin/iw";
	const char *nmcli = "/run/current-system/sw/bin/nmcli";

	if (!iface || !*iface)
		return;
	if (ssid_pid > 0 || ssid_event)
		return;
	if (access(nmcli, X_OK) != 0) {
		nmcli = "/run/wrappers/bin/nmcli";
		if (access(nmcli, X_OK) != 0)
			nmcli = "nmcli";
	}
	if (access(iw, X_OK) != 0) {
		iw = "/run/wrappers/bin/iw";
		if (access(iw, X_OK) != 0)
			iw = "iw";
	}
	if (snprintf(cmd, sizeof(cmd),
				"%s -t -f active,ssid dev wifi | grep '^yes' | cut -d: -f2 || "
				"%s dev %s link 2>/dev/null",
				nmcli, iw, iface) >= (int)sizeof(cmd))
		return;

	if (pipe(pipefd) != 0)
		return;

	ssid_pid = fork();
	if (ssid_pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		_exit(127);
	} else if (ssid_pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		ssid_pid = -1;
		return;
	}

	close(pipefd[1]);
	ssid_fd = pipefd[0];
	fcntl(ssid_fd, F_SETFL, fcntl(ssid_fd, F_GETFL) | O_NONBLOCK);
	ssid_len = 0;
	ssid_buf[0] = '\0';
	ssid_event = wl_event_loop_add_fd(event_loop, ssid_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP,
			ssid_event_cb, NULL);
	if (!ssid_event)
		stop_ssid_fetch();
}

void
stop_ssid_fetch(void)
{
	if (ssid_event) {
		wl_event_source_remove(ssid_event);
		ssid_event = NULL;
	}
	if (ssid_fd >= 0) {
		close(ssid_fd);
		ssid_fd = -1;
	}
	if (ssid_pid > 0) {
		waitpid(ssid_pid, NULL, WNOHANG);
		ssid_pid = -1;
	}
	ssid_len = 0;
	ssid_buf[0] = '\0';
}

int
ssid_event_cb(int fd, uint32_t mask, void *data)
{
	ssize_t n;
	(void)data;
	(void)mask;

	for (;;) {
		if (ssid_len >= sizeof(ssid_buf) - 1)
			break;
		n = read(fd, ssid_buf + ssid_len, sizeof(ssid_buf) - 1 - ssid_len);
		if (n > 0) {
			ssid_len += (size_t)n;
			continue;
		} else if (n == 0) {
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		} else {
			break;
		}
	}

	ssid_buf[ssid_len] = '\0';
	if (ssid_buf[0]) {
		char *line = ssid_buf;
		while (line && *line) {
			char *next = strpbrk(line, "\r\n");
			if (next)
				*next = '\0';

			/* Try iw-style output first */
			char *ssid = strstr(line, "SSID");
			if (ssid) {
				while (*ssid && *ssid != ':' && *ssid != ' ')
					ssid++;
				while (*ssid == ':' || *ssid == ' ' || *ssid == '\t')
					ssid++;
			} else {
				ssid = line;
				while (*ssid == ' ' || *ssid == '\t')
					ssid++;
			}

			if (ssid && *ssid) {
				snprintf(net_ssid, sizeof(net_ssid), "%s", ssid);
				break;
			}

			line = next ? next + 1 : NULL;
		}
	}
	if (!net_ssid[0])
		snprintf(net_ssid, sizeof(net_ssid), "WiFi");
	ssid_last_time = time(NULL);
	stop_ssid_fetch();
	return 0;
}

void
connect_wifi_ssid(const char *ssid)
{
	if (!ssid || !*ssid)
		return;
	if (fork() == 0) {
		setsid();
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		/* Include ifname to ensure proper network switching */
		if (net_iface[0])
			execlp("nmcli", "nmcli", "device", "wifi", "connect", ssid, "ifname", net_iface, (char *)NULL);
		else
			execlp("nmcli", "nmcli", "device", "wifi", "connect", ssid, (char *)NULL);
		_exit(0);
	}
}

void
connect_wifi_with_prompt(const char *ssid, int secure)
{
	if (!ssid || !*ssid)
		return;
	/* Prefer zenity if present, else fallback to a terminal prompt */
	if (secure) {
		char cmd[1024];
		/* Include ifname ($2) to ensure proper network switching */
		snprintf(cmd, sizeof(cmd),
			"if command -v zenity >/dev/null 2>&1; then "
				"pw=$(zenity --entry --hide-text --text=\"Passphrase for $1\" --title=\"Wi-Fi\" 2>/dev/null) || exit 1; "
			"elif command -v wofi >/dev/null 2>&1; then "
				"pw=$(printf '' | wofi --dmenu --password --prompt \"Passphrase for $1\" 2>/dev/null) || exit 1; "
			"elif command -v bemenu >/dev/null 2>&1; then "
				"pw=$(printf '' | bemenu -p \"Passphrase for $1\" -P 2>/dev/null) || exit 1; "
			"elif command -v dmenu >/dev/null 2>&1; then "
				"pw=$(printf '' | dmenu -p \"Passphrase for $1\" 2>/dev/null) || exit 1; "
			"else exit 1; fi; "
			"nmcli device wifi connect \"$1\" password \"$pw\"%s >/dev/null 2>&1",
			net_iface[0] ? " ifname \"$2\"" : "");
		if (fork() == 0) {
			setsid();
			if (net_iface[0])
				execl("/bin/sh", "/bin/sh", "-c", cmd, "nmcli-connect", ssid, net_iface, (char *)NULL);
			else
				execl("/bin/sh", "/bin/sh", "-c", cmd, "nmcli-connect", ssid, (char *)NULL);
			_exit(0);
		}
		return;
	}
	/* Fallback: attempt plain connect (will fail if not saved) */
	connect_wifi_ssid(ssid);
}

void
connect_wifi_with_password(const char *ssid, const char *password)
{
	if (!ssid || !*ssid || !password)
		return;
	if (fork() == 0) {
		char cmd[512];
		setsid();
		/* Include ifname to ensure proper network switching */
		if (net_iface[0])
			snprintf(cmd, sizeof(cmd),
				"nmcli device wifi connect \"%s\" password \"%s\" ifname \"%s\" >/dev/null 2>&1",
				ssid, password, net_iface);
		else
			snprintf(cmd, sizeof(cmd),
				"nmcli device wifi connect \"%s\" password \"%s\" >/dev/null 2>&1",
				ssid, password);
		execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
		_exit(0);
	}
}

void
wifi_networks_clear(void)
{
	WifiNetwork *n, *tmp;
	wl_list_for_each_safe(n, tmp, &wifi_networks, link) {
		wl_list_remove(&n->link);
		free(n);
	}
	wl_list_init(&wifi_networks);
}

void
net_menu_hide_all(void)
{
	Monitor *m;
	int any_visible = 0;

	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.net_menu.tree) {
			if (m->statusbar.net_menu.visible)
				any_visible = 1;
			wlr_scene_node_set_enabled(&m->statusbar.net_menu.tree->node, 0);
			m->statusbar.net_menu.visible = 0;
		}
		if (m->statusbar.net_menu.submenu_tree) {
			wlr_scene_node_set_enabled(&m->statusbar.net_menu.submenu_tree->node, 0);
			m->statusbar.net_menu.submenu_visible = 0;
		}
		m->statusbar.net_menu.hover = -1;
		m->statusbar.net_menu.submenu_hover = -1;
	}
	if (any_visible) {
		wifi_networks_accept_updates = 0;
		wifi_networks_freeze_existing = 0;
		wifi_networks_generation++;
		wifi_networks_clear();
	}
}

void
transfer_status_menus(Monitor *from, Monitor *to)
{
	if (!from || !to || from == to)
		return;

	/* Transfer net_menu */
	if (from->statusbar.net_menu.visible && to->statusbar.net_menu.tree) {
		/* Hide on old monitor */
		wlr_scene_node_set_enabled(&from->statusbar.net_menu.tree->node, 0);
		from->statusbar.net_menu.visible = 0;
		if (from->statusbar.net_menu.submenu_tree) {
			wlr_scene_node_set_enabled(&from->statusbar.net_menu.submenu_tree->node, 0);
			from->statusbar.net_menu.submenu_visible = 0;
		}
		/* Show on new monitor */
		net_menu_render(to);
		if (from->statusbar.net_menu.submenu_visible)
			net_menu_submenu_render(to);
	}

	/* Transfer cpu_popup */
	if (from->statusbar.cpu_popup.visible && to->statusbar.cpu_popup.tree) {
		wlr_scene_node_set_enabled(&from->statusbar.cpu_popup.tree->node, 0);
		from->statusbar.cpu_popup.visible = 0;
		to->statusbar.cpu_popup.visible = 1;
		to->statusbar.cpu_popup.refresh_data = 1;
		rendercpupopup(to);
		wlr_scene_node_set_enabled(&to->statusbar.cpu_popup.tree->node, 1);
	}

	/* Transfer wifi_popup */
	if (from->wifi_popup.visible && to->wifi_popup.tree) {
		/* Copy popup state */
		memcpy(to->wifi_popup.ssid, from->wifi_popup.ssid, sizeof(to->wifi_popup.ssid));
		memcpy(to->wifi_popup.password, from->wifi_popup.password, sizeof(to->wifi_popup.password));
		to->wifi_popup.password_len = from->wifi_popup.password_len;
		to->wifi_popup.cursor_pos = from->wifi_popup.cursor_pos;
		to->wifi_popup.error = from->wifi_popup.error;
		/* Hide on old, show on new */
		wlr_scene_node_set_enabled(&from->wifi_popup.tree->node, 0);
		from->wifi_popup.visible = 0;
		to->wifi_popup.visible = 1;
		wifi_popup_render(to);
	}
}

void
net_menu_render(Monitor *m)
{
	const char *wifi_label = "Available networks";
	const char *vpn_label = "VPN";
	int padding = statusbar_module_padding;
	int line_spacing = 4;
	int row_h;
	int wifi_text_w, vpn_text_w;
	int max_w;
	NetMenu *menu;
	struct wlr_scene_node *node, *tmp;
	float border_col[4] = {0, 0, 0, 1};
	int border_px = 1;
	int y;

	if (!m || !m->statusbar.net_menu.tree)
		return;
	menu = &m->statusbar.net_menu;

	wl_list_for_each_safe(node, tmp, &menu->tree->children, link) {
		if (menu->bg && node == &menu->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!statusfont.font) {
		menu->width = menu->height = 0;
		wlr_scene_node_set_enabled(&menu->tree->node, 0);
		menu->visible = 0;
		return;
	}

	row_h = statusfont.height + line_spacing;
	if (row_h < statusfont.height)
		row_h = statusfont.height;
	wifi_text_w = status_text_width(wifi_label);
	vpn_text_w = status_text_width(vpn_label);
	max_w = (wifi_text_w > vpn_text_w ? wifi_text_w : vpn_text_w) + 2 * padding + row_h; /* space for arrow */
	menu->width = max_w;
	menu->height = 2 * row_h + 2 * padding; /* Two rows */

	if (!menu->bg && !(menu->bg = wlr_scene_tree_create(menu->tree)))
		return;
	wlr_scene_node_set_enabled(&menu->bg->node, 1);
	wlr_scene_node_set_position(&menu->bg->node, 0, 0);
	wl_list_for_each_safe(node, tmp, &menu->bg->children, link)
		wlr_scene_node_destroy(node);
	drawrect(menu->bg, 0, 0, menu->width, menu->height, net_menu_row_bg);
	drawrect(menu->bg, 0, 0, menu->width, border_px, border_col);
	drawrect(menu->bg, 0, menu->height - border_px, menu->width, border_px, border_col);
	drawrect(menu->bg, 0, 0, border_px, menu->height, border_col);
	drawrect(menu->bg, menu->width - border_px, 0, border_px, menu->height, border_col);

	/* Row 0: Available networks */
	y = padding;
	{
		char text[256];
		snprintf(text, sizeof(text), "%s  >", wifi_label);
		drawrect(menu->tree, padding, y, menu->width - 2 * padding, row_h,
				menu->hover == 0 ? net_menu_row_bg_hover : net_menu_row_bg);
		tray_menu_draw_text(menu->tree, text, padding + 4, y, row_h);
	}

	/* Row 1: VPN */
	y += row_h;
	{
		char text[256];
		snprintf(text, sizeof(text), "%s  >", vpn_label);
		drawrect(menu->tree, padding, y, menu->width - 2 * padding, row_h,
				menu->hover == 1 ? net_menu_row_bg_hover : net_menu_row_bg);
		tray_menu_draw_text(menu->tree, text, padding + 4, y, row_h);
	}

	menu->visible = 1;
	wlr_scene_node_set_enabled(&menu->tree->node, 1);
}

void
net_menu_submenu_render(Monitor *m)
{
	NetMenu *menu;
	struct wlr_scene_node *node, *tmp;
	int padding = statusbar_module_padding;
	int line_spacing = 4;
	int row_h;
	int max_w = 0;
	int total_h = 0;
	int y;
	int count = 0;
	char line[256];
	float border_col[4] = {0, 0, 0, 1};
	int border_px = 1;

	if (!m || !m->statusbar.net_menu.submenu_tree)
		return;
	menu = &m->statusbar.net_menu;

	wl_list_for_each_safe(node, tmp, &menu->submenu_tree->children, link) {
		if (menu->submenu_bg && node == &menu->submenu_bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!statusfont.font) {
		wlr_scene_node_set_enabled(&menu->submenu_tree->node, 0);
		menu->submenu_visible = 0;
		return;
	}

	row_h = statusfont.height + line_spacing;
	if (row_h < statusfont.height)
		row_h = statusfont.height;

	/* Render based on submenu_type: 0 = WiFi, 1 = VPN */
	if (menu->submenu_type == 0) {
		/* WiFi submenu */
		WifiNetwork *n;
		if (wifi_scan_inflight && wl_list_empty(&wifi_networks)) {
			snprintf(line, sizeof(line), "Scanning...");
			max_w = status_text_width(line);
			total_h = row_h + 2 * padding;
		} else if (wl_list_empty(&wifi_networks)) {
			snprintf(line, sizeof(line), "No networks");
			max_w = status_text_width(line);
			total_h = row_h + 2 * padding;
		} else {
			wl_list_for_each(n, &wifi_networks, link) {
				int w;
				int is_connected = (net_ssid[0] && strcmp(n->ssid, net_ssid) == 0);
				count++;
				snprintf(line, sizeof(line), "%s (%d%%%s%s)", n->ssid, n->strength,
						n->secure ? ", secured" : "",
						is_connected ? ", Connected" : "");
				w = status_text_width(line);
				if (w > max_w)
					max_w = w;
			}
			total_h = padding * 2 + count * row_h;
		}
	} else {
		/* VPN submenu */
		VpnConnection *v;
		if (vpn_scan_inflight && wl_list_empty(&vpn_connections)) {
			snprintf(line, sizeof(line), "Loading...");
			max_w = status_text_width(line);
			total_h = row_h + 2 * padding;
		} else if (wl_list_empty(&vpn_connections)) {
			snprintf(line, sizeof(line), "No VPN connections");
			max_w = status_text_width(line);
			total_h = row_h + 2 * padding;
		} else {
			wl_list_for_each(v, &vpn_connections, link) {
				int w;
				count++;
				snprintf(line, sizeof(line), "%s%s", v->name,
						v->active ? " (Connected)" : "");
				w = status_text_width(line);
				if (w > max_w)
					max_w = w;
			}
			total_h = padding * 2 + count * row_h;
		}
	}

	if (max_w <= 0)
		max_w = 100;

	menu->submenu_width = max_w + 2 * padding;
	menu->submenu_height = total_h;

	if (!menu->submenu_bg && !(menu->submenu_bg = wlr_scene_tree_create(menu->submenu_tree)))
		return;
	wlr_scene_node_set_enabled(&menu->submenu_bg->node, 1);
	wlr_scene_node_set_position(&menu->submenu_bg->node, 0, 0);
	wl_list_for_each_safe(node, tmp, &menu->submenu_bg->children, link)
		wlr_scene_node_destroy(node);
	drawrect(menu->submenu_bg, 0, 0, menu->submenu_width, menu->submenu_height, net_menu_row_bg);
	drawrect(menu->submenu_bg, 0, 0, menu->submenu_width, border_px, border_col);
	drawrect(menu->submenu_bg, 0, menu->submenu_height - border_px, menu->submenu_width, border_px, border_col);
	drawrect(menu->submenu_bg, 0, 0, border_px, menu->submenu_height, border_col);
	drawrect(menu->submenu_bg, menu->submenu_width - border_px, 0, border_px, menu->submenu_height, border_col);

	y = padding;
	if (menu->submenu_type == 0) {
		/* WiFi items */
		if (wl_list_empty(&wifi_networks)) {
			drawrect(menu->submenu_tree, padding, y, menu->submenu_width - 2 * padding, row_h,
					net_menu_row_bg);
			tray_menu_draw_text(menu->submenu_tree, line, padding + 4, y, row_h);
		} else {
			WifiNetwork *n;
			int idx = 0;
			wl_list_for_each(n, &wifi_networks, link) {
				int x = padding;
				int hover = (menu->submenu_hover == idx);
				int is_connected = (net_ssid[0] && strcmp(n->ssid, net_ssid) == 0);
				snprintf(line, sizeof(line), "%s (%d%%%s%s)", n->ssid, n->strength,
						n->secure ? ", secured" : "",
						is_connected ? ", Connected" : "");
				drawrect(menu->submenu_tree, x, y, menu->submenu_width - 2 * padding, row_h,
						hover ? net_menu_row_bg_hover : net_menu_row_bg);
				tray_menu_draw_text(menu->submenu_tree, line, x + 4, y, row_h);
				n->secure = n->secure ? 1 : 0;
				idx++;
				y += row_h;
			}
		}
	} else {
		/* VPN items */
		if (wl_list_empty(&vpn_connections)) {
			drawrect(menu->submenu_tree, padding, y, menu->submenu_width - 2 * padding, row_h,
					net_menu_row_bg);
			tray_menu_draw_text(menu->submenu_tree, line, padding + 4, y, row_h);
		} else {
			VpnConnection *v;
			int idx = 0;
			wl_list_for_each(v, &vpn_connections, link) {
				int x = padding;
				int hover = (menu->submenu_hover == idx);
				snprintf(line, sizeof(line), "%s%s", v->name,
						v->active ? " (Connected)" : "");
				drawrect(menu->submenu_tree, x, y, menu->submenu_width - 2 * padding, row_h,
						hover ? net_menu_row_bg_hover : net_menu_row_bg);
				tray_menu_draw_text(menu->submenu_tree, line, x + 4, y, row_h);
				idx++;
				y += row_h;
			}
		}
	}

	menu->submenu_visible = 1;
	wlr_scene_node_set_enabled(&menu->submenu_tree->node, 1);
}

void
request_wifi_scan(void)
{
	const char *cmd = "nmcli -t -f SSID,SIGNAL,SECURITY device wifi list --rescan auto";
	int pipefd[2] = {-1, -1};

	if (wifi_scan_inflight)
		return;

	if (pipe(pipefd) != 0)
		return;

	wifi_scan_generation = wifi_networks_generation;
	wifi_scan_pid = fork();
	if (wifi_scan_pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		_exit(127);
	} else if (wifi_scan_pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		wifi_scan_pid = -1;
		return;
	}

	close(pipefd[1]);
	wifi_scan_fd = pipefd[0];
	fcntl(wifi_scan_fd, F_SETFL, fcntl(wifi_scan_fd, F_GETFL) | O_NONBLOCK);
	wifi_scan_len = 0;
	wifi_scan_buf[0] = '\0';
	wifi_scan_inflight = 1;
	wifi_scan_event = wl_event_loop_add_fd(event_loop, wifi_scan_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP, wifi_scan_event_cb, NULL);
	if (!wifi_scan_event) {
		wifi_scan_inflight = 0;
		close(wifi_scan_fd);
		wifi_scan_fd = -1;
		if (wifi_scan_pid > 0)
			waitpid(wifi_scan_pid, NULL, WNOHANG);
		wifi_scan_pid = -1;
	}
}

void
vpn_connections_clear(void)
{
	VpnConnection *v, *tmp;
	wl_list_for_each_safe(v, tmp, &vpn_connections, link) {
		wl_list_remove(&v->link);
		free(v);
	}
}

void
vpn_scan_finish(void)
{
	if (vpn_scan_event) {
		wl_event_source_remove(vpn_scan_event);
		vpn_scan_event = NULL;
	}
	if (vpn_scan_fd >= 0) {
		close(vpn_scan_fd);
		vpn_scan_fd = -1;
	}
	if (vpn_scan_pid > 0) {
		waitpid(vpn_scan_pid, NULL, WNOHANG);
		vpn_scan_pid = -1;
	}
	vpn_scan_inflight = 0;
}

int
vpn_scan_event_cb(int fd, uint32_t mask, void *data)
{
	ssize_t n;
	char *saveptr = NULL;
	char *line;
	(void)data;
	(void)mask;

	for (;;) {
		if (vpn_scan_len >= sizeof(vpn_scan_buf) - 1)
			break;
		n = read(fd, vpn_scan_buf + vpn_scan_len,
				sizeof(vpn_scan_buf) - 1 - vpn_scan_len);
		if (n > 0) {
			vpn_scan_len += (size_t)n;
			continue;
		} else if (n == 0) {
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		} else {
			break;
		}
	}

	vpn_scan_buf[vpn_scan_len] = '\0';

	vpn_connections_clear();

	/* Parse nmcli output: NAME:UUID:TYPE:DEVICE
	 * We only want VPN connections (TYPE=vpn) */
	line = strtok_r(vpn_scan_buf, "\n", &saveptr);
	while (line) {
		char *uuid_s, *type_s, *device_s;
		VpnConnection *ventry;
		char name[128] = {0};
		char uuid[64] = {0};
		int active = 0;

		/* Parse NAME:UUID:TYPE:DEVICE */
		uuid_s = strchr(line, ':');
		if (uuid_s) {
			*uuid_s = '\0';
			uuid_s++;
			type_s = strchr(uuid_s, ':');
			if (type_s) {
				*type_s = '\0';
				type_s++;
				device_s = strchr(type_s, ':');
				if (device_s) {
					*device_s = '\0';
					device_s++;
				}
			} else {
				device_s = NULL;
			}
		} else {
			type_s = NULL;
			device_s = NULL;
		}

		/* Only process VPN connections */
		if (!type_s || strcmp(type_s, "vpn") != 0) {
			line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}

		if (line[0])
			snprintf(name, sizeof(name), "%s", line);
		if (!name[0]) {
			line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}
		if (uuid_s)
			snprintf(uuid, sizeof(uuid), "%s", uuid_s);
		/* Active if DEVICE is not empty (e.g., has interface like "tun0") */
		if (device_s && device_s[0] && strcmp(device_s, "--") != 0)
			active = 1;

		ventry = ecalloc(1, sizeof(*ventry));
		snprintf(ventry->name, sizeof(ventry->name), "%s", name);
		snprintf(ventry->uuid, sizeof(ventry->uuid), "%s", uuid);
		ventry->active = active;
		wl_list_insert(vpn_connections.prev, &ventry->link);

		line = strtok_r(NULL, "\n", &saveptr);
	}

	vpn_scan_finish();

	/* Update submenu if visible and showing VPN */
	{
		Monitor *m;
		wl_list_for_each(m, &mons, link) {
			if (m->statusbar.net_menu.visible && m->statusbar.net_menu.submenu_type == 1) {
				net_menu_submenu_render(m);
				if (m->statusbar.net_menu.submenu_tree)
					wlr_scene_node_set_position(&m->statusbar.net_menu.submenu_tree->node,
							m->statusbar.net_menu.submenu_x, m->statusbar.net_menu.submenu_y);
			}
		}
	}
	return 0;
}

void
request_vpn_scan(void)
{
	/* Get all VPN connections with their status */
	const char *cmd = "nmcli -t -f NAME,UUID,TYPE,DEVICE connection show";
	int pipefd[2] = {-1, -1};

	if (vpn_scan_inflight)
		return;

	if (pipe(pipefd) != 0)
		return;

	vpn_scan_pid = fork();
	if (vpn_scan_pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		_exit(127);
	} else if (vpn_scan_pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		vpn_scan_pid = -1;
		return;
	}

	close(pipefd[1]);
	vpn_scan_fd = pipefd[0];
	fcntl(vpn_scan_fd, F_SETFL, fcntl(vpn_scan_fd, F_GETFL) | O_NONBLOCK);
	vpn_scan_len = 0;
	vpn_scan_buf[0] = '\0';
	vpn_scan_inflight = 1;
	vpn_scan_event = wl_event_loop_add_fd(event_loop, vpn_scan_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP, vpn_scan_event_cb, NULL);
	if (!vpn_scan_event) {
		vpn_scan_inflight = 0;
		close(vpn_scan_fd);
		vpn_scan_fd = -1;
		if (vpn_scan_pid > 0)
			waitpid(vpn_scan_pid, NULL, WNOHANG);
		vpn_scan_pid = -1;
	}
}

void
vpn_connect_finish(void)
{
	if (vpn_connect_event) {
		wl_event_source_remove(vpn_connect_event);
		vpn_connect_event = NULL;
	}
	if (vpn_connect_fd >= 0) {
		close(vpn_connect_fd);
		vpn_connect_fd = -1;
	}
	if (vpn_connect_pid > 0) {
		int status;
		waitpid(vpn_connect_pid, &status, 0);
		vpn_connect_pid = -1;
	}
}

int
vpn_connect_event_cb(int fd, uint32_t mask, void *data)
{
	ssize_t n;
	int success = 0;
	(void)data;
	(void)mask;

	for (;;) {
		if (vpn_connect_len >= sizeof(vpn_connect_buf) - 1)
			break;
		n = read(fd, vpn_connect_buf + vpn_connect_len,
				sizeof(vpn_connect_buf) - 1 - vpn_connect_len);
		if (n > 0) {
			vpn_connect_len += (size_t)n;
			continue;
		} else if (n == 0) {
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		} else {
			break;
		}
	}

	vpn_connect_buf[vpn_connect_len] = '\0';

	/* Check if connection was successful by looking at the output */
	if (strstr(vpn_connect_buf, "successfully activated") ||
	    strstr(vpn_connect_buf, "Connection successfully activated")) {
		success = 1;
	}

	vpn_connect_finish();

	if (!success && vpn_pending_name[0]) {
		/* Show error toast */
		char error_msg[256];
		/* Extract error message if possible, otherwise generic */
		if (strstr(vpn_connect_buf, "Error:")) {
			snprintf(error_msg, sizeof(error_msg), "VPN failed: %s", vpn_pending_name);
		} else {
			snprintf(error_msg, sizeof(error_msg), "VPN connection failed: %s", vpn_pending_name);
		}
		toast_show(selmon, error_msg, 3000);
	}

	/* Refresh VPN list to update status */
	request_vpn_scan();
	vpn_pending_name[0] = '\0';

	return 0;
}

void
vpn_connect(const char *name)
{
	char cmd[512];
	int pipefd[2] = {-1, -1};

	if (!name || !name[0])
		return;

	/* Check if already connecting */
	if (vpn_connect_pid > 0)
		return;

	snprintf(cmd, sizeof(cmd), "nmcli connection up \"%s\" 2>&1", name);
	snprintf(vpn_pending_name, sizeof(vpn_pending_name), "%s", name);

	if (pipe(pipefd) != 0) {
		toast_show(selmon, "VPN: Failed to create pipe", 3000);
		return;
	}

	vpn_connect_pid = fork();
	if (vpn_connect_pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		_exit(127);
	} else if (vpn_connect_pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		vpn_connect_pid = -1;
		toast_show(selmon, "VPN: Failed to fork", 3000);
		return;
	}

	close(pipefd[1]);
	vpn_connect_fd = pipefd[0];
	fcntl(vpn_connect_fd, F_SETFL, fcntl(vpn_connect_fd, F_GETFL) | O_NONBLOCK);
	vpn_connect_len = 0;
	vpn_connect_buf[0] = '\0';
	vpn_connect_event = wl_event_loop_add_fd(event_loop, vpn_connect_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP, vpn_connect_event_cb, NULL);
	if (!vpn_connect_event) {
		close(vpn_connect_fd);
		vpn_connect_fd = -1;
		if (vpn_connect_pid > 0)
			waitpid(vpn_connect_pid, NULL, WNOHANG);
		vpn_connect_pid = -1;
		toast_show(selmon, "VPN: Failed to add event", 3000);
	}
}

int
wifi_network_exists(const char *ssid)
{
	WifiNetwork *it;

	if (!ssid || !*ssid)
		return 0;
	wl_list_for_each(it, &wifi_networks, link) {
		if (strncmp(it->ssid, ssid, sizeof(it->ssid)) == 0)
			return 1;
	}
	return 0;
}

void
wifi_networks_insert_sorted(WifiNetwork *n)
{
	WifiNetwork *it;

	/* de-duplicate by SSID, keep strongest and mark secure if any entry is secure */
	wl_list_for_each(it, &wifi_networks, link) {
		if (strncmp(it->ssid, n->ssid, sizeof(it->ssid)) == 0) {
			if (n->strength > it->strength)
				it->strength = n->strength;
			if (n->secure)
				it->secure = 1;
			free(n);
			return;
		}
	}

	wl_list_for_each(it, &wifi_networks, link) {
		if (n->strength > it->strength) {
			wl_list_insert(it->link.prev, &n->link);
			return;
		}
	}
	wl_list_insert(wifi_networks.prev, &n->link);
}

void
wifi_scan_finish(void)
{
	if (wifi_scan_event) {
		wl_event_source_remove(wifi_scan_event);
		wifi_scan_event = NULL;
	}
	if (wifi_scan_fd >= 0) {
		close(wifi_scan_fd);
		wifi_scan_fd = -1;
	}
	if (wifi_scan_pid > 0) {
		waitpid(wifi_scan_pid, NULL, WNOHANG);
		wifi_scan_pid = -1;
	}
	wifi_scan_inflight = 0;
}

int
wifi_scan_event_cb(int fd, uint32_t mask, void *data)
{
	ssize_t n;
	char *saveptr = NULL;
	char *line;
	int discard_results;
	(void)data;

	discard_results = (!wifi_networks_accept_updates)
			|| (wifi_scan_generation != wifi_networks_generation);

	for (;;) {
		if (wifi_scan_len >= sizeof(wifi_scan_buf) - 1)
			break;
		n = read(fd, wifi_scan_buf + wifi_scan_len,
				sizeof(wifi_scan_buf) - 1 - wifi_scan_len);
		if (n > 0) {
			wifi_scan_len += (size_t)n;
			continue;
		} else if (n == 0) {
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		} else {
			break;
		}
	}

	wifi_scan_buf[wifi_scan_len] = '\0';

	if (discard_results) {
		wifi_scan_finish();
		if (wifi_networks_accept_updates && !wifi_scan_inflight)
			request_wifi_scan();
		wifi_scan_plan_rescan();
		return 0;
	}

	if (!wifi_networks_freeze_existing)
		wifi_networks_clear();

	line = strtok_r(wifi_scan_buf, "\n", &saveptr);
	while (line) {
		char *sig_s, *sec_s;
		WifiNetwork *nentry;
		int strength = 0;
		int secure = 0;
		char ssid[128] = {0};

		sig_s = strchr(line, ':');
		if (sig_s) {
			*sig_s = '\0';
			sig_s++;
			sec_s = strchr(sig_s, ':');
			if (sec_s) {
				*sec_s = '\0';
				sec_s++;
			}
		} else {
			sec_s = NULL;
		}

		if (line[0])
			snprintf(ssid, sizeof(ssid), "%s", line);
		if (!ssid[0]) {
			line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}
		if (wifi_networks_freeze_existing && wifi_network_exists(ssid)) {
			line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}
		if (sig_s)
			strength = atoi(sig_s);
		if (sec_s && *sec_s && strcmp(sec_s, "--") != 0)
			secure = 1;

		nentry = ecalloc(1, sizeof(*nentry));
		snprintf(nentry->ssid, sizeof(nentry->ssid), "%s", ssid);
		nentry->strength = strength;
		nentry->secure = secure;
		wifi_networks_insert_sorted(nentry);

		line = strtok_r(NULL, "\n", &saveptr);
	}

	wifi_scan_finish();

	/* Update submenu if visible and showing WiFi */
	{
		Monitor *m;
		wl_list_for_each(m, &mons, link) {
			if (m->statusbar.net_menu.visible && m->statusbar.net_menu.submenu_type == 0) {
				net_menu_submenu_render(m);
				if (m->statusbar.net_menu.submenu_tree)
					wlr_scene_node_set_position(&m->statusbar.net_menu.submenu_tree->node,
							m->statusbar.net_menu.submenu_x, m->statusbar.net_menu.submenu_y);
			}
		}
	}
	wifi_scan_plan_rescan();
	return 0;
}

void
wifi_scan_plan_rescan(void)
{
	if (wifi_scan_timer)
		wl_event_source_timer_update(wifi_scan_timer, 500);
}

int
wifi_scan_timer_cb(void *data)
{
	Monitor *m;
	int visible = 0;
	(void)data;

	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.net_menu.visible) {
			visible = 1;
			break;
		}
	}

	if (!visible)
		return 0;

	if (!wifi_scan_inflight)
		request_wifi_scan();

	/* Always reschedule while menu is visible */
	wifi_scan_plan_rescan();

	return 0;
}

void request_vpn_scan(void);

static void
net_menu_open(Monitor *m)
{
	const int offset = statusbar_module_padding;
	int desired_x, max_x;
	NetMenu *menu;

	if (!m || !m->showbar || !m->statusbar.net_menu.tree || !m->statusbar.sysicons.tree)
		return;
	if (!m->statusbar.area.width || !m->statusbar.area.height)
		return;

	menu = &m->statusbar.net_menu;
	net_menu_hide_all();
	wifi_networks_generation++;
	wifi_networks_accept_updates = 1;
	wifi_networks_freeze_existing = 0;
	wifi_networks_clear();
	if (m->statusbar.net_popup.tree) {
		wlr_scene_node_set_enabled(&m->statusbar.net_popup.tree->node, 0);
		m->statusbar.net_popup.visible = 0;
	}
	/* Pre-fetch both WiFi and VPN data */
	request_wifi_scan();
	request_vpn_scan();
	net_menu_render(m);
	if (menu->width <= 0 || menu->height <= 0)
		return;

	desired_x = m->statusbar.sysicons.x - menu->width / 2 + m->statusbar.sysicons.width / 2;
	if (desired_x < 0)
		desired_x = 0;
	max_x = m->statusbar.area.width - menu->width;
	if (max_x < 0)
		max_x = 0;
	if (desired_x > max_x)
		desired_x = max_x;

	menu->x = desired_x;
	menu->y = m->statusbar.area.height + statusbar_module_padding;
	wlr_scene_node_set_position(&menu->tree->node, menu->x, menu->y);
	wlr_scene_node_set_enabled(&menu->tree->node, 1);
	menu->visible = 1;
	menu->hover = -1;  /* No row hovered initially */
	menu->submenu_visible = 0;
	menu->submenu_type = -1;

	menu->submenu_x = menu->x + menu->width + offset;
	menu->submenu_y = menu->y + offset;
	/* Don't show submenu automatically - wait for hover */
	if (menu->submenu_tree) {
		wlr_scene_node_set_enabled(&menu->submenu_tree->node, 0);
	}
	wifi_scan_plan_rescan();
}

void
net_menu_update_hover(Monitor *m, double cx, double cy)
{
	NetMenu *menu;
	int lx, ly;
	int row_h, padding;
	int prev_hover, prev_sub, prev_submenu_type;
	int new_hover = -1, new_sub = -1;
	int show_submenu = 0;
	const int offset = statusbar_module_padding;

	if (!m || !m->statusbar.net_menu.visible || !m->statusbar.net_menu.submenu_tree)
		return;
	menu = &m->statusbar.net_menu;
	lx = (int)floor(cx) - m->statusbar.area.x;
	ly = (int)floor(cy) - m->statusbar.area.y;
	padding = statusbar_module_padding;
	row_h = statusfont.height + 4;
	if (row_h < statusfont.height)
		row_h = statusfont.height;

	/* Check if hovering over main menu rows */
	if (lx >= menu->x && lx < menu->x + menu->width &&
			ly >= menu->y && ly < menu->y + menu->height) {
		int rel_y = ly - menu->y - padding;
		if (rel_y >= 0 && rel_y < row_h) {
			new_hover = 0;  /* WiFi row */
		} else if (rel_y >= row_h && rel_y < 2 * row_h) {
			new_hover = 1;  /* VPN row */
		}
	}

	/* Check if hovering over submenu */
	if (menu->submenu_visible &&
			lx >= menu->submenu_x && lx < menu->submenu_x + menu->submenu_width &&
			ly >= menu->submenu_y && ly < menu->submenu_y + menu->submenu_height) {
		/* Keep current main menu hover when in submenu */
		new_hover = menu->hover;
		if (menu->submenu_type == 0) {
			/* WiFi submenu */
			if (!wl_list_empty(&wifi_networks)) {
				int rel_y = ly - menu->submenu_y - padding;
				if (rel_y >= 0) {
					new_sub = rel_y / row_h;
				}
			}
		} else if (menu->submenu_type == 1) {
			/* VPN submenu */
			if (!wl_list_empty(&vpn_connections)) {
				int rel_y = ly - menu->submenu_y - padding;
				if (rel_y >= 0) {
					new_sub = rel_y / row_h;
				}
			}
		}
	}

	prev_hover = menu->hover;
	prev_sub = menu->submenu_hover;
	prev_submenu_type = menu->submenu_type;
	menu->hover = new_hover;
	menu->submenu_hover = new_sub;

	/* Determine if we should show submenu and which type */
	if (new_hover == 0) {
		menu->submenu_type = 0;  /* WiFi */
		show_submenu = 1;
		/* Adjust submenu Y position for WiFi row */
		menu->submenu_y = menu->y + offset;
	} else if (new_hover == 1) {
		menu->submenu_type = 1;  /* VPN */
		show_submenu = 1;
		/* Adjust submenu Y position for VPN row */
		menu->submenu_y = menu->y + row_h + offset;
	} else if (new_hover == -1 && !menu->submenu_visible) {
		/* Not hovering over anything and submenu not visible */
		show_submenu = 0;
	} else {
		/* Keep submenu if hovering over it */
		show_submenu = menu->submenu_visible;
	}

	if (menu->hover != prev_hover)
		net_menu_render(m);

	if (show_submenu) {
		if (menu->submenu_type != prev_submenu_type || !menu->submenu_visible) {
			menu->submenu_hover = -1;
			net_menu_submenu_render(m);
			wlr_scene_node_set_position(&menu->submenu_tree->node, menu->submenu_x, menu->submenu_y);
		} else if (menu->submenu_hover != prev_sub) {
			net_menu_submenu_render(m);
			wlr_scene_node_set_position(&menu->submenu_tree->node, menu->submenu_x, menu->submenu_y);
		}
	} else if (menu->submenu_visible) {
		/* Hide submenu */
		wlr_scene_node_set_enabled(&menu->submenu_tree->node, 0);
		menu->submenu_visible = 0;
	}
}

int
net_menu_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	NetMenu *menu;
	int padding = statusbar_module_padding;
	int row_h;

	if (!m || !m->statusbar.net_menu.visible)
		return 0;
	menu = &m->statusbar.net_menu;
	row_h = statusfont.height + 4;
	if (row_h < statusfont.height)
		row_h = statusfont.height;

	if (lx >= menu->x && lx < menu->x + menu->width &&
			ly >= menu->y && ly < menu->y + menu->height) {
		/* main menu rows - just consume click, hover handles submenu */
		return 1;
	}

	if (menu->submenu_visible &&
			lx >= menu->submenu_x && lx < menu->submenu_x + menu->submenu_width &&
			ly >= menu->submenu_y && ly < menu->submenu_y + menu->submenu_height) {
		if (button != BTN_LEFT)
			return 1;

		int rel_y = ly - menu->submenu_y - padding;
		int idx = rel_y / row_h;

		if (menu->submenu_type == 0) {
			/* WiFi submenu */
			if (!wl_list_empty(&wifi_networks) && rel_y >= 0) {
				WifiNetwork *n = wifi_network_at_index(idx);
				if (n) {
					/* Copy SSID before hiding menu - net_menu_hide_all() frees WifiNetwork structs */
					char ssid_copy[128];
					int secure = n->secure;
					snprintf(ssid_copy, sizeof(ssid_copy), "%s", n->ssid);
					net_menu_hide_all();
					if (secure) {
						/* Secure network: try saved credentials first,
						 * password popup will appear if not saved */
						wifi_try_saved_connect(m, ssid_copy);
					} else {
						/* Open network: connect directly without password */
						connect_wifi_ssid(ssid_copy);
					}
					return 1;
				}
			}
		} else if (menu->submenu_type == 1) {
			/* VPN submenu */
			if (!wl_list_empty(&vpn_connections) && rel_y >= 0) {
				VpnConnection *v = vpn_connection_at_index(idx);
				if (v) {
					/* Copy name before hiding menu */
					char name_copy[128];
					snprintf(name_copy, sizeof(name_copy), "%s", v->name);
					net_menu_hide_all();
					/* Connect to VPN immediately */
					vpn_connect(name_copy);
					return 1;
				}
			}
		}
		return 1;
	}

	net_menu_hide_all();
	return 1;
}


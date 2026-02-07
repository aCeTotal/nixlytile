/* popup.c - Auto-extracted from nixlytile.c */
#include "nixlytile.h"
#include "client.h"

int
wifi_popup_connect_cb(int fd, uint32_t mask, void *data)
{
	Monitor *m = data;
	WifiPasswordPopup *p;
	char buf[256];
	ssize_t n;
	int status = 0;
	int finished = 0;

	(void)mask;

	if (!m)
		return 0;

	p = &m->wifi_popup;

	/* Read until EOF or would block */
	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			continue;  /* More data, keep reading */
		} else if (n == 0) {
			finished = 1;  /* EOF - process finished */
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;  /* No more data available yet */
		} else {
			finished = 1;  /* Error - treat as finished */
			break;
		}
	}

	if (!finished)
		return 0;

	/* Process finished - get exit status */
	if (p->connect_pid > 0) {
		waitpid(p->connect_pid, &status, 0);
		p->connect_pid = -1;
	}
	if (p->connect_event) {
		wl_event_source_remove(p->connect_event);
		p->connect_event = NULL;
	}
	if (p->connect_fd >= 0) {
		close(p->connect_fd);
		p->connect_fd = -1;
	}

	p->connecting = 0;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		/* Success - hide popup and menu */
		wifi_popup_hide_all();
		net_menu_hide_all();
	} else if (p->try_saved) {
		/* Saved credentials failed - show password popup */
		p->try_saved = 0;
		net_menu_hide_all();  /* Hide menu before showing popup */
		p->visible = 1;
		p->error = 0;
		wifi_popup_render(m);
	} else {
		/* Password entry failed - show error */
		p->error = 1;
		wifi_popup_render(m);
	}
	return 0;
}

void
wifi_popup_connect(Monitor *m)
{
	WifiPasswordPopup *p;
	int pipefd[2];
	pid_t pid;

	if (!m)
		return;

	p = &m->wifi_popup;

	if (p->connecting || p->password_len == 0)
		return;

	/* Clean up any previous connection attempt */
	if (p->connect_event) {
		wl_event_source_remove(p->connect_event);
		p->connect_event = NULL;
	}
	if (p->connect_fd >= 0) {
		close(p->connect_fd);
		p->connect_fd = -1;
	}
	if (p->connect_pid > 0) {
		kill(p->connect_pid, SIGTERM);
		waitpid(p->connect_pid, NULL, WNOHANG);
		p->connect_pid = -1;
	}

	if (pipe(pipefd) < 0)
		return;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		/* Child */
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		setsid();

		/* Delete any existing corrupted connection profile first (ignore errors),
		 * then connect with the new password.
		 * Use execlp with separate args to avoid shell escaping issues. */
		{
			pid_t del_pid = fork();
			if (del_pid == 0) {
				/* Delete child - silence output */
				int null_fd = open("/dev/null", O_RDWR);
				if (null_fd >= 0) {
					dup2(null_fd, STDOUT_FILENO);
					dup2(null_fd, STDERR_FILENO);
					close(null_fd);
				}
				execlp("nmcli", "nmcli", "connection", "delete", p->ssid, (char *)NULL);
				_exit(0);
			}
			if (del_pid > 0)
				waitpid(del_pid, NULL, 0);
		}

		/* Now connect with password */
		if (net_iface[0])
			execlp("nmcli", "nmcli", "device", "wifi", "connect",
				p->ssid, "password", p->password, "ifname", net_iface, (char *)NULL);
		else
			execlp("nmcli", "nmcli", "device", "wifi", "connect",
				p->ssid, "password", p->password, (char *)NULL);
		_exit(1);
	}

	/* Parent */
	close(pipefd[1]);

	int flags = fcntl(pipefd[0], F_GETFL, 0);
	fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

	p->connect_pid = pid;
	p->connect_fd = pipefd[0];
	p->connecting = 1;
	p->error = 0;
	p->connect_event = wl_event_loop_add_fd(event_loop,
		pipefd[0], WL_EVENT_READABLE | WL_EVENT_HANGUP,
		wifi_popup_connect_cb, m);

	wifi_popup_render(m);
}

void
wifi_try_saved_connect(Monitor *m, const char *ssid)
{
	WifiPasswordPopup *p;
	int pipefd[2];
	pid_t pid;

	if (!m || !ssid || !*ssid)
		return;

	p = &m->wifi_popup;

	/* Store SSID for potential password popup later */
	snprintf(p->ssid, sizeof(p->ssid), "%s", ssid);
	p->password[0] = '\0';
	p->password_len = 0;
	p->visible = 0;  /* Don't show popup yet */
	p->error = 0;
	p->try_saved = 1;  /* Mark that we're trying saved credentials */

	/* Clean up any previous connection attempt */
	if (p->connect_event) {
		wl_event_source_remove(p->connect_event);
		p->connect_event = NULL;
	}
	if (p->connect_fd >= 0) {
		close(p->connect_fd);
		p->connect_fd = -1;
	}
	if (p->connect_pid > 0) {
		kill(p->connect_pid, SIGTERM);
		waitpid(p->connect_pid, NULL, WNOHANG);
		p->connect_pid = -1;
	}

	if (pipe(pipefd) < 0)
		return;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		/* Child */
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		setsid();

		/* Try connecting without password - nmcli will use saved credentials
		 * Include ifname to ensure proper network switching */
		if (net_iface[0])
			execlp("nmcli", "nmcli", "device", "wifi", "connect", ssid, "ifname", net_iface, (char *)NULL);
		else
			execlp("nmcli", "nmcli", "device", "wifi", "connect", ssid, (char *)NULL);
		_exit(1);
	}

	/* Parent */
	close(pipefd[1]);

	int flags = fcntl(pipefd[0], F_GETFL, 0);
	fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

	p->connect_pid = pid;
	p->connect_fd = pipefd[0];
	p->connecting = 1;
	p->connect_event = wl_event_loop_add_fd(event_loop,
		pipefd[0], WL_EVENT_READABLE | WL_EVENT_HANGUP,
		wifi_popup_connect_cb, m);
}

Monitor *
wifi_popup_visible_monitor(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->wifi_popup.visible)
			return m;
	}
	return NULL;
}

void
wifi_popup_hide(Monitor *m)
{
	if (!m || !m->wifi_popup.tree)
		return;
	/* Clean up any ongoing connection */
	if (m->wifi_popup.connect_event) {
		wl_event_source_remove(m->wifi_popup.connect_event);
		m->wifi_popup.connect_event = NULL;
	}
	if (m->wifi_popup.connect_fd >= 0) {
		close(m->wifi_popup.connect_fd);
		m->wifi_popup.connect_fd = -1;
	}
	if (m->wifi_popup.connect_pid > 0) {
		kill(m->wifi_popup.connect_pid, SIGTERM);
		waitpid(m->wifi_popup.connect_pid, NULL, WNOHANG);
		m->wifi_popup.connect_pid = -1;
	}
	m->wifi_popup.visible = 0;
	m->wifi_popup.password[0] = '\0';
	m->wifi_popup.password_len = 0;
	m->wifi_popup.ssid[0] = '\0';
	m->wifi_popup.button_hover = 0;
	m->wifi_popup.connecting = 0;
	m->wifi_popup.error = 0;
	m->wifi_popup.try_saved = 0;
	wlr_scene_node_set_enabled(&m->wifi_popup.tree->node, 0);
	/* Also hide on-screen keyboard */
	osk_hide(m);
}

void
wifi_popup_hide_all(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		wifi_popup_hide(m);
}

void
wifi_popup_show(Monitor *m, const char *ssid)
{
	if (!m || !ssid || !*ssid)
		return;

	/* Copy SSID first - net_menu_hide_all() frees the WifiNetwork structs */
	snprintf(m->wifi_popup.ssid, sizeof(m->wifi_popup.ssid), "%s", ssid);

	wifi_popup_hide_all();
	net_menu_hide_all();

	m->wifi_popup.password[0] = '\0';
	m->wifi_popup.password_len = 0;
	m->wifi_popup.cursor_pos = 0;
	m->wifi_popup.button_hover = 0;
	m->wifi_popup.visible = 1;

	wifi_popup_render(m);

	/* Show on-screen keyboard in HTPC mode when controller is connected */
	if (htpc_mode_active && !wl_list_empty(&gamepads))
		osk_show(m, NULL);
}

void
wifi_popup_render(Monitor *m)
{
	WifiPasswordPopup *p;
	struct wlr_scene_node *node, *tmp;
	int padding = 20;
	int input_width = 300;
	int input_height;
	int button_width = 120;
	int button_height;
	int title_width = 0;
	int total_width, total_height;
	int center_x, center_y;
	char title[256];
	const char *btn_text;
	int line_spacing = 10;

	if (!m || !m->wifi_popup.tree)
		return;

	p = &m->wifi_popup;

	/* Set button text based on state */
	if (p->connecting)
		btn_text = "Connecting...";
	else if (p->error)
		btn_text = "Retry";
	else
		btn_text = "Connect";

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &p->tree->children, link) {
		if (p->bg && node == &p->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!p->visible || !statusfont.font) {
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		return;
	}

	input_height = statusfont.height + 12;
	button_height = statusfont.height + 10;

	snprintf(title, sizeof(title), "Wifi Passphrase:");

	/* Compute title width */
	{
		int pen_x = 0;
		uint32_t prev_cp = 0;
		for (size_t i = 0; title[i]; i++) {
			long kern_x = 0, kern_y = 0;
			uint32_t cp = (unsigned char)title[i];
			const struct fcft_glyph *glyph;
			if (prev_cp)
				fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
			pen_x += (int)kern_x;
			glyph = fcft_rasterize_char_utf32(statusfont.font, cp, statusbar_font_subpixel);
			if (glyph)
				pen_x += glyph->advance.x;
			if (title[i + 1])
				pen_x += statusbar_font_spacing;
			prev_cp = cp;
		}
		title_width = pen_x;
	}

	total_width = MAX(input_width, title_width) + 2 * padding;
	total_height = statusfont.height + line_spacing + input_height + line_spacing + button_height + 2 * padding;

	p->width = total_width;
	p->height = total_height;

	/* Center on monitor */
	center_x = m->m.x + (m->m.width - total_width) / 2;
	center_y = m->m.y + (m->m.height - total_height) / 2;

	wlr_scene_node_set_position(&p->tree->node, center_x, center_y);

	/* Background */
	if (!p->bg)
		p->bg = wlr_scene_tree_create(p->tree);
	if (p->bg) {
		wl_list_for_each_safe(node, tmp, &p->bg->children, link)
			wlr_scene_node_destroy(node);
		wlr_scene_node_set_position(&p->bg->node, 0, 0);
		drawrect(p->bg, 0, 0, total_width, total_height, statusbar_popup_bg);
		/* Border */
		float border_col[4] = {0.3f, 0.3f, 0.3f, 1.0f};
		drawrect(p->bg, 0, 0, total_width, 1, border_col);
		drawrect(p->bg, 0, total_height - 1, total_width, 1, border_col);
		drawrect(p->bg, 0, 0, 1, total_height, border_col);
		drawrect(p->bg, total_width - 1, 0, 1, total_height, border_col);
	}

	/* Draw title */
	{
		int origin_x = padding + (total_width - 2 * padding - title_width) / 2;
		int origin_y = padding + statusfont.ascent;
		int pen_x = 0;
		uint32_t prev_cp = 0;

		for (size_t i = 0; title[i]; i++) {
			long kern_x = 0, kern_y = 0;
			uint32_t cp = (unsigned char)title[i];
			const struct fcft_glyph *glyph;
			struct wlr_buffer *buffer;
			struct wlr_scene_buffer *scene_buf;

			if (prev_cp)
				fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
			pen_x += (int)kern_x;

			glyph = fcft_rasterize_char_utf32(statusfont.font, cp, statusbar_font_subpixel);
			if (!glyph || !glyph->pix) {
				prev_cp = cp;
				continue;
			}

			buffer = statusbar_buffer_from_glyph(glyph);
			if (buffer) {
				scene_buf = wlr_scene_buffer_create(p->tree, NULL);
				if (scene_buf) {
					wlr_scene_buffer_set_buffer(scene_buf, buffer);
					wlr_scene_node_set_position(&scene_buf->node,
						origin_x + pen_x + glyph->x,
						origin_y - glyph->y);
				}
				wlr_buffer_drop(buffer);
			}
			pen_x += glyph->advance.x;
			if (title[i + 1])
				pen_x += statusbar_font_spacing;
			prev_cp = cp;
		}
	}

	/* Draw input box */
	{
		int input_x = padding;
		int input_y = padding + statusfont.height + line_spacing;
		float input_bg[4] = {0.1f, 0.1f, 0.1f, 1.0f};
		float input_border[4] = {0.4f, 0.4f, 0.4f, 1.0f};
		float error_border[4] = {0.8f, 0.2f, 0.2f, 1.0f};
		float *border = p->error ? error_border : input_border;

		drawrect(p->tree, input_x, input_y, input_width, input_height, input_bg);
		drawrect(p->tree, input_x, input_y, input_width, 1, border);
		drawrect(p->tree, input_x, input_y + input_height - 1, input_width, 1, border);
		drawrect(p->tree, input_x, input_y, 1, input_height, border);
		drawrect(p->tree, input_x + input_width - 1, input_y, 1, input_height, border);

		/* Draw password as dots */
		{
			int text_x = input_x + 6;
			int text_y = input_y + (input_height - statusfont.height) / 2 + statusfont.ascent;
			int pen_x = 0;

			for (int i = 0; i < p->password_len && i < (int)sizeof(p->password) - 1; i++) {
				const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(
					statusfont.font, 0x2022 /* bullet */, statusbar_font_subpixel);
				if (!glyph)
					glyph = fcft_rasterize_char_utf32(statusfont.font, '*', statusbar_font_subpixel);
				if (glyph && glyph->pix) {
					struct wlr_buffer *buffer = statusbar_buffer_from_glyph(glyph);
					if (buffer) {
						struct wlr_scene_buffer *scene_buf = wlr_scene_buffer_create(p->tree, NULL);
						if (scene_buf) {
							wlr_scene_buffer_set_buffer(scene_buf, buffer);
							wlr_scene_node_set_position(&scene_buf->node,
								text_x + pen_x + glyph->x,
								text_y - glyph->y);
						}
						wlr_buffer_drop(buffer);
					}
					pen_x += glyph->advance.x + statusbar_font_spacing;
				}
			}

			/* Draw cursor */
			float cursor_col[4] = {1.0f, 1.0f, 1.0f, 1.0f};
			drawrect(p->tree, text_x + pen_x, input_y + 4, 2, input_height - 8, cursor_col);
		}
	}

	/* Draw connect button */
	{
		int btn_x = padding + (input_width - button_width) / 2;
		int btn_y = padding + statusfont.height + line_spacing + input_height + line_spacing;
		float btn_bg[4] = {0.2f, 0.4f, 0.6f, 1.0f};
		float btn_hover_bg[4] = {0.3f, 0.5f, 0.7f, 1.0f};

		if (p->button_hover)
			drawrect(p->tree, btn_x, btn_y, button_width, button_height, btn_hover_bg);
		else
			drawrect(p->tree, btn_x, btn_y, button_width, button_height, btn_bg);

		/* Center button text */
		int btn_text_width = 0;
		{
			int pen_x = 0;
			uint32_t prev_cp = 0;
			for (size_t i = 0; btn_text[i]; i++) {
				long kern_x = 0, kern_y = 0;
				uint32_t cp = (unsigned char)btn_text[i];
				const struct fcft_glyph *glyph;
				if (prev_cp)
					fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
				pen_x += (int)kern_x;
				glyph = fcft_rasterize_char_utf32(statusfont.font, cp, statusbar_font_subpixel);
				if (glyph)
					pen_x += glyph->advance.x;
				if (btn_text[i + 1])
					pen_x += statusbar_font_spacing;
				prev_cp = cp;
			}
			btn_text_width = pen_x;
		}

		int text_x = btn_x + (button_width - btn_text_width) / 2;
		int text_y = btn_y + (button_height - statusfont.height) / 2 + statusfont.ascent;
		int pen_x = 0;
		uint32_t prev_cp = 0;

		for (size_t i = 0; btn_text[i]; i++) {
			long kern_x = 0, kern_y = 0;
			uint32_t cp = (unsigned char)btn_text[i];
			const struct fcft_glyph *glyph;
			struct wlr_buffer *buffer;
			struct wlr_scene_buffer *scene_buf;

			if (prev_cp)
				fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
			pen_x += (int)kern_x;

			glyph = fcft_rasterize_char_utf32(statusfont.font, cp, statusbar_font_subpixel);
			if (!glyph || !glyph->pix) {
				prev_cp = cp;
				continue;
			}

			buffer = statusbar_buffer_from_glyph(glyph);
			if (buffer) {
				scene_buf = wlr_scene_buffer_create(p->tree, NULL);
				if (scene_buf) {
					wlr_scene_buffer_set_buffer(scene_buf, buffer);
					wlr_scene_node_set_position(&scene_buf->node,
						text_x + pen_x + glyph->x,
						text_y - glyph->y);
				}
				wlr_buffer_drop(buffer);
			}
			pen_x += glyph->advance.x;
			if (btn_text[i + 1])
				pen_x += statusbar_font_spacing;
			prev_cp = cp;
		}
	}

	wlr_scene_node_set_enabled(&p->tree->node, 1);
}

int
wifi_popup_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym)
{
	WifiPasswordPopup *p;

	if (!m || !m->wifi_popup.visible)
		return 0;

	p = &m->wifi_popup;

	if (sym == XKB_KEY_Escape) {
		wifi_popup_hide_all();
		return 1;
	}

	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		if (!p->connecting && p->password_len > 0) {
			wifi_popup_connect(m);
		}
		return 1;
	}

	/* Don't allow editing while connecting */
	if (p->connecting)
		return 1;

	if (sym == XKB_KEY_BackSpace && p->password_len > 0) {
		p->password_len--;
		p->password[p->password_len] = '\0';
		p->error = 0;  /* Clear error on edit */
		wifi_popup_render(m);
		return 1;
	}

	/* Handle printable characters
	 * Allow Ctrl+Alt (AltGr) combinations - on many systems AltGr is Ctrl+Alt
	 * Only block Ctrl without Alt (real Ctrl shortcuts) and Logo key */
	{
		int is_altgr = (mods & WLR_MODIFIER_CTRL) && (mods & WLR_MODIFIER_ALT);
		int is_ctrl_only = (mods & WLR_MODIFIER_CTRL) && !is_altgr;
		char ch = 0;

		/* Handle numpad digits (XKB_KEY_KP_0 to XKB_KEY_KP_9) */
		if (sym >= XKB_KEY_KP_0 && sym <= XKB_KEY_KP_9) {
			ch = '0' + (sym - XKB_KEY_KP_0);
		}
		/* Handle other numpad keys */
		else if (sym == XKB_KEY_KP_Space) ch = ' ';
		else if (sym == XKB_KEY_KP_Multiply) ch = '*';
		else if (sym == XKB_KEY_KP_Add) ch = '+';
		else if (sym == XKB_KEY_KP_Subtract) ch = '-';
		else if (sym == XKB_KEY_KP_Decimal) ch = '.';
		else if (sym == XKB_KEY_KP_Divide) ch = '/';
		else if (sym == XKB_KEY_KP_Equal) ch = '=';
		/* Handle regular printable ASCII */
		else if (sym >= 0x20 && sym <= 0x7e && !is_ctrl_only && !(mods & WLR_MODIFIER_LOGO)) {
			ch = (char)sym;
		}

		if (ch && p->password_len + 1 < (int)sizeof(p->password)) {
			p->password[p->password_len] = ch;
			p->password_len++;
			p->password[p->password_len] = '\0';
			p->error = 0;  /* Clear error on edit */
			wifi_popup_render(m);
			return 1;
		}
	}

	return 1; /* Consume all keys while popup is open */
}

int
wifi_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	WifiPasswordPopup *p;
	int popup_x, popup_y;
	int padding = 20;
	int input_width = 300;
	int button_width = 120;
	int button_height;
	int input_height;
	int line_spacing = 10;

	if (!m || !m->wifi_popup.visible)
		return 0;

	p = &m->wifi_popup;

	input_height = statusfont.height + 12;
	button_height = statusfont.height + 10;

	popup_x = m->m.x + (m->m.width - p->width) / 2;
	popup_y = m->m.y + (m->m.height - p->height) / 2;

	/* Check if click is inside popup */
	if (lx < popup_x || lx >= popup_x + p->width ||
	    ly < popup_y || ly >= popup_y + p->height) {
		if (!p->connecting)
			wifi_popup_hide_all();
		return 1;
	}

	/* Check button click */
	if (button == BTN_LEFT && !p->connecting) {
		int btn_x = popup_x + padding + (input_width - button_width) / 2;
		int btn_y = popup_y + padding + statusfont.height + line_spacing + input_height + line_spacing;

		if (lx >= btn_x && lx < btn_x + button_width &&
		    ly >= btn_y && ly < btn_y + button_height) {
			if (p->password_len > 0) {
				wifi_popup_connect(m);
			}
			return 1;
		}
	}

	return 1;
}

Monitor *
sudo_popup_visible_monitor(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->sudo_popup.visible)
			return m;
	}
	return NULL;
}

void
sudo_popup_hide(Monitor *m)
{
	if (!m)
		return;
	m->sudo_popup.visible = 0;
	m->sudo_popup.password[0] = '\0';
	m->sudo_popup.password_len = 0;
	m->sudo_popup.cursor_pos = 0;
	m->sudo_popup.error = 0;
	m->sudo_popup.running = 0;
	if (m->sudo_popup.wait_timer) {
		wl_event_source_remove(m->sudo_popup.wait_timer);
		m->sudo_popup.wait_timer = NULL;
	}
	if (m->sudo_popup.tree)
		wlr_scene_node_set_enabled(&m->sudo_popup.tree->node, 0);
	/* Also hide on-screen keyboard */
	osk_hide(m);
}

void
sudo_popup_hide_all(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		sudo_popup_hide(m);
}

void
sudo_popup_render(Monitor *m)
{
	SudoPopup *p;
	struct wlr_scene_node *node, *tmp;
	int padding = 20;
	int input_width = 300;
	int input_height;
	int button_width = 100;
	int button_height;
	int title_width = 0;
	int total_width, total_height;
	int center_x, center_y;
	const char *btn_text;
	int line_spacing = 10;

	if (!m || !m->sudo_popup.tree)
		return;

	p = &m->sudo_popup;

	/* Set button text based on state */
	if (p->running)
		btn_text = "Running...";
	else if (p->error)
		btn_text = "Retry";
	else
		btn_text = "OK";

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &p->tree->children, link) {
		if (p->bg && node == &p->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!p->visible || !statusfont.font) {
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		return;
	}

	/* Calculate sizes */
	title_width = status_text_width(p->title);
	input_height = statusfont.height + 12;
	button_height = statusfont.height + 10;

	total_width = input_width + padding * 2;
	if (title_width + padding * 2 > total_width)
		total_width = title_width + padding * 2;

	total_height = padding + statusfont.height + line_spacing +
	               input_height + line_spacing + button_height + padding;

	p->width = total_width;
	p->height = total_height;

	center_x = m->m.x + (m->m.width - total_width) / 2;
	center_y = m->m.y + (m->m.height - total_height) / 2;

	wlr_scene_node_set_position(&p->tree->node, center_x, center_y);

	/* Draw background */
	if (p->bg) {
		wl_list_for_each_safe(node, tmp, &p->bg->children, link)
			wlr_scene_node_destroy(node);
		drawrect(p->bg, 0, 0, total_width, total_height, statusbar_popup_bg);
		/* Border */
		const float border[4] = {0.3f, 0.3f, 0.3f, 1.0f};
		drawrect(p->bg, 0, 0, total_width, 1, border);
		drawrect(p->bg, 0, total_height - 1, total_width, 1, border);
		drawrect(p->bg, 0, 0, 1, total_height, border);
		drawrect(p->bg, total_width - 1, 0, 1, total_height, border);
	}

	/* Draw title */
	{
		struct wlr_scene_tree *title_tree = wlr_scene_tree_create(p->tree);
		if (title_tree) {
			StatusModule mod = {0};
			int title_x = (total_width - title_width) / 2;
			wlr_scene_node_set_position(&title_tree->node, title_x, padding);
			mod.tree = title_tree;
			tray_render_label(&mod, p->title, 0, statusfont.height, statusbar_fg);
		}
	}

	/* Draw password input field */
	{
		struct wlr_scene_tree *input_tree = wlr_scene_tree_create(p->tree);
		if (input_tree) {
			const float input_bg[4] = {0.1f, 0.1f, 0.1f, 0.8f};
			const float input_border[4] = {0.4f, 0.4f, 0.4f, 1.0f};
			int input_x = padding;
			int input_y = padding + statusfont.height + line_spacing;
			StatusModule mod = {0};
			char masked[257];
			int i;

			wlr_scene_node_set_position(&input_tree->node, input_x, input_y);

			/* Input background */
			drawrect(input_tree, 0, 0, input_width, input_height, input_bg);
			drawrect(input_tree, 0, 0, input_width, 1, input_border);
			drawrect(input_tree, 0, input_height - 1, input_width, 1, input_border);
			drawrect(input_tree, 0, 0, 1, input_height, input_border);
			drawrect(input_tree, input_width - 1, 0, 1, input_height, input_border);

			/* Masked password (show dots) */
			for (i = 0; i < p->password_len && i < 256; i++)
				masked[i] = '*';
			masked[i] = '\0';

			mod.tree = input_tree;
			tray_render_label(&mod, masked, 6, input_height, statusbar_fg);

			/* Draw cursor */
			if (!p->running) {
				int cursor_x = 6 + status_text_width(masked);
				const float cursor_color[4] = {1.0f, 1.0f, 1.0f, 0.8f};
				drawrect(input_tree, cursor_x, 4, 2, input_height - 8, cursor_color);
			}
		}
	}

	/* Draw OK button */
	{
		struct wlr_scene_tree *btn_tree = wlr_scene_tree_create(p->tree);
		if (btn_tree) {
			const float btn_bg[4] = {0.2f, 0.4f, 0.6f, 1.0f};
			const float btn_hover_bg[4] = {0.3f, 0.5f, 0.7f, 1.0f};
			int btn_x = padding + (input_width - button_width) / 2;
			int btn_y = padding + statusfont.height + line_spacing + input_height + line_spacing;
			int btn_text_w = status_text_width(btn_text);
			StatusModule mod = {0};

			wlr_scene_node_set_position(&btn_tree->node, btn_x, btn_y);

			drawrect(btn_tree, 0, 0, button_width, button_height,
			         p->button_hover ? btn_hover_bg : btn_bg);

			mod.tree = btn_tree;
			tray_render_label(&mod, btn_text, (button_width - btn_text_w) / 2,
			                  button_height, statusbar_fg);
		}
	}

	/* Show error message if auth failed */
	if (p->error) {
		struct wlr_scene_tree *err_tree = wlr_scene_tree_create(p->tree);
		if (err_tree) {
			const char *err_msg = "Authentication failed";
			const float err_color[4] = {1.0f, 0.3f, 0.3f, 1.0f};
			int err_w = status_text_width(err_msg);
			StatusModule mod = {0};

			wlr_scene_node_set_position(&err_tree->node,
			                            (total_width - err_w) / 2,
			                            total_height - padding + 2);
			mod.tree = err_tree;
			tray_render_label(&mod, err_msg, 0, statusfont.height, err_color);
		}
	}

	wlr_scene_node_set_enabled(&p->tree->node, 1);
}

int
sudo_popup_wait_timer(void *data)
{
	Monitor *m = data;
	SudoPopup *p;
	int status = 0;
	pid_t result;

	if (!m)
		return 0;

	p = &m->sudo_popup;

	if (p->sudo_pid <= 0) {
		/* No child to wait for */
		if (p->wait_timer) {
			wl_event_source_remove(p->wait_timer);
			p->wait_timer = NULL;
		}
		return 0;
	}

	/* Non-blocking check if child has exited */
	result = waitpid(p->sudo_pid, &status, WNOHANG);
	if (result == 0) {
		/* Child still running, re-arm timer to check again in 100ms */
		if (p->wait_timer)
			wl_event_source_timer_update(p->wait_timer, 100);
		return 0;
	}

	/* Child has exited (result > 0) or error (result < 0) */
	p->sudo_pid = -1;

	/* Remove timer */
	if (p->wait_timer) {
		wl_event_source_remove(p->wait_timer);
		p->wait_timer = NULL;
	}

	p->running = 0;
	if (result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		/* Success */
		char toast_msg[256];
		snprintf(toast_msg, sizeof(toast_msg), "%s installed", p->pending_pkg);
		toast_show(m, toast_msg, 4000);
		sudo_popup_hide_all();
		/* Reload desktop entries to pick up new applications */
		desktop_entries_loaded = 0;
		desktop_entry_count = 0;
	} else {
		/* Auth failed or error - show popup again with error message */
		p->error = 1;
		p->password[0] = '\0';
		p->password_len = 0;
		p->cursor_pos = 0;
		p->visible = 1;
		toast_show(m, "Authentication failed", 3000);
		sudo_popup_render(m);
	}
	return 0;
}

int
sudo_popup_cb(int fd, uint32_t mask, void *data)
{
	Monitor *m = data;
	SudoPopup *p;
	char buf[256];
	ssize_t n;

	if (!m)
		return 0;

	p = &m->sudo_popup;

	/* Read any available output (non-blocking since fd triggers the callback) */
	for (;;) {
		n = read(fd, buf, sizeof(buf) - 1);
		if (n <= 0)
			break;
		buf[n] = '\0';
		wlr_log(WLR_DEBUG, "sudo output: %s", buf);
	}

	/* Clean up event source for fd */
	if (p->sudo_event) {
		wl_event_source_remove(p->sudo_event);
		p->sudo_event = NULL;
	}
	if (p->sudo_fd >= 0) {
		close(p->sudo_fd);
		p->sudo_fd = -1;
	}

	/* Start a timer to poll for child exit instead of blocking */
	if (p->sudo_pid > 0 && !p->wait_timer) {
		p->wait_timer = wl_event_loop_add_timer(event_loop,
			sudo_popup_wait_timer, m);
		if (p->wait_timer)
			wl_event_source_timer_update(p->wait_timer, 100); /* Check every 100ms */
	}

	return 0;
}

void
sudo_popup_execute(Monitor *m)
{
	SudoPopup *p;
	int pipefd[2];
	pid_t pid;

	if (!m)
		return;

	p = &m->sudo_popup;

	if (p->password_len == 0 || p->running)
		return;

	if (pipe(pipefd) < 0) {
		wlr_log(WLR_ERROR, "Failed to create pipe for sudo");
		return;
	}

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	if (pid == 0) {
		/* Child process */
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		setsid();

		/* Run command with sudo -S (read password from stdin)
		 * Use printf %s to safely pass password without shell interpretation */
		char cmd[2048];
		snprintf(cmd, sizeof(cmd), "printf '%%s\\n' \"$SUDO_PASS\" | sudo -S sh -c '%s' 2>&1",
		         p->pending_cmd);
		setenv("SUDO_PASS", p->password, 1);
		execl("/bin/sh", "sh", "-c", cmd, NULL);
		_exit(127);
	}

	/* Parent */
	close(pipefd[1]);

	/* Set pipe to non-blocking to prevent freezing the compositor */
	fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);

	p->sudo_pid = pid;
	p->sudo_fd = pipefd[0];
	p->running = 1;
	p->error = 0;

	/* Set up event handler for reading output */
	p->sudo_event = wl_event_loop_add_fd(event_loop, pipefd[0],
	                                      WL_EVENT_READABLE,
	                                      sudo_popup_cb, m);

	/* Show "Installing..." toast */
	{
		char toast_msg[256];
		snprintf(toast_msg, sizeof(toast_msg), "Installing %s...", p->pending_pkg);
		toast_show(m, toast_msg, 60000); /* Long timeout, will be replaced on completion */
	}

	/* Hide popup immediately - installation runs in background */
	p->visible = 0;
	if (p->tree)
		wlr_scene_node_set_enabled(&p->tree->node, 0);
}

int
sudo_popup_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym)
{
	SudoPopup *p;
	char buf[8];
	int len;

	if (!m || !m->sudo_popup.visible)
		return 0;

	p = &m->sudo_popup;

	/* Don't accept input while running */
	if (p->running)
		return 1;

	/* Escape to close */
	if (sym == XKB_KEY_Escape) {
		sudo_popup_hide_all();
		return 1;
	}

	/* Enter to submit */
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		if (p->password_len > 0)
			sudo_popup_execute(m);
		return 1;
	}

	/* Backspace */
	if (sym == XKB_KEY_BackSpace) {
		if (p->password_len > 0) {
			p->password_len--;
			p->password[p->password_len] = '\0';
			sudo_popup_render(m);
		}
		return 1;
	}

	/* Regular character input */
	len = xkb_keysym_to_utf8(sym, buf, sizeof(buf));
	if (len > 0 && len < (int)sizeof(buf) && buf[0] >= 32 && buf[0] < 127) {
		if (p->password_len < (int)sizeof(p->password) - 1) {
			p->password[p->password_len++] = buf[0];
			p->password[p->password_len] = '\0';
			sudo_popup_render(m);
		}
		return 1;
	}

	return 1;
}

void
sudo_popup_show(Monitor *m, const char *title, const char *cmd, const char *pkg_name)
{
	if (!m || !cmd)
		return;

	sudo_popup_hide_all();

	snprintf(m->sudo_popup.title, sizeof(m->sudo_popup.title), "%s",
	         title ? title : "sudo:");
	snprintf(m->sudo_popup.pending_cmd, sizeof(m->sudo_popup.pending_cmd), "%s", cmd);
	snprintf(m->sudo_popup.pending_pkg, sizeof(m->sudo_popup.pending_pkg), "%s",
	         pkg_name ? pkg_name : "package");

	m->sudo_popup.password[0] = '\0';
	m->sudo_popup.password_len = 0;
	m->sudo_popup.cursor_pos = 0;
	m->sudo_popup.button_hover = 0;
	m->sudo_popup.error = 0;
	m->sudo_popup.running = 0;
	m->sudo_popup.visible = 1;

	sudo_popup_render(m);

	/* Show on-screen keyboard in HTPC mode when controller is connected */
	if (htpc_mode_active && !wl_list_empty(&gamepads))
		osk_show(m, NULL);
}

Monitor *
osk_visible_monitor(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->osk.visible)
			return m;
	}
	return NULL;
}

void
osk_hide(Monitor *m)
{
	if (!m || !m->osk.tree)
		return;

	m->osk.visible = 0;
	wlr_scene_node_set_enabled(&m->osk.tree->node, 0);
	m->osk.target_surface = NULL;
}

void
osk_hide_all(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		osk_hide(m);
	}
}

void
osk_render(Monitor *m)
{
	OnScreenKeyboard *osk;
	struct wlr_scene_node *node, *tmp;
	int key_width = 60;
	int key_height = 50;
	int key_spacing = 4;
	int padding = 10;
	int total_width, total_height;
	int start_x, start_y;
	const char *(*layout)[OSK_COLS];

	if (!m || !m->osk.tree)
		return;

	osk = &m->osk;

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &osk->tree->children, link) {
		if (osk->bg && node == &osk->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!osk->visible || !statusfont.font) {
		wlr_scene_node_set_enabled(&osk->tree->node, 0);
		return;
	}

	/* Select layout based on shift state */
	layout = (osk->shift_active || osk->caps_lock) ? osk_layout_upper : osk_layout_lower;

	total_width = OSK_COLS * (key_width + key_spacing) - key_spacing + 2 * padding;
	total_height = OSK_ROWS * (key_height + key_spacing) - key_spacing + 2 * padding;

	osk->width = total_width;
	osk->height = total_height;

	/* Position at bottom center of monitor */
	start_x = m->m.x + (m->m.width - total_width) / 2;
	start_y = m->m.y + m->m.height - total_height - 20;

	wlr_scene_node_set_position(&osk->tree->node, start_x, start_y);

	/* Background */
	if (!osk->bg)
		osk->bg = wlr_scene_tree_create(osk->tree);
	if (osk->bg) {
		wl_list_for_each_safe(node, tmp, &osk->bg->children, link)
			wlr_scene_node_destroy(node);
		wlr_scene_node_set_position(&osk->bg->node, 0, 0);
		float bg_color[4] = {0.1f, 0.1f, 0.12f, 0.95f};
		drawrect(osk->bg, 0, 0, total_width, total_height, bg_color);
		/* Border */
		float border_col[4] = {0.3f, 0.3f, 0.35f, 1.0f};
		drawrect(osk->bg, 0, 0, total_width, 2, border_col);
	}

	/* Draw keys */
	for (int row = 0; row < OSK_ROWS; row++) {
		for (int col = 0; col < OSK_COLS; col++) {
			int key_x = padding + col * (key_width + key_spacing);
			int key_y = padding + row * (key_height + key_spacing);
			const char *label = layout[row][col];
			int is_selected = (row == osk->sel_row && col == osk->sel_col);
			int is_special = 0;
			int actual_width = key_width;

			/* Check for special keys */
			if (strcmp(label, "⇧") == 0 || strcmp(label, "⌫") == 0 ||
			    strcmp(label, "↵") == 0) {
				is_special = 1;
			}

			/* Space bar spans multiple keys */
			if (row == 4 && col >= 3 && col <= 7) {
				if (col == 3) {
					actual_width = 5 * (key_width + key_spacing) - key_spacing;
				} else {
					continue; /* Skip cells covered by space bar */
				}
			}

			/* Enter key spans 2 columns */
			if (row == 4 && col == 11) {
				continue; /* Already drawn at col 10 */
			}
			if (row == 4 && col == 10) {
				actual_width = 2 * (key_width + key_spacing) - key_spacing;
			}

			/* Key background */
			float key_bg[4];
			if (is_selected) {
				key_bg[0] = 0.3f; key_bg[1] = 0.5f; key_bg[2] = 0.8f; key_bg[3] = 1.0f;
			} else if (is_special) {
				key_bg[0] = 0.25f; key_bg[1] = 0.25f; key_bg[2] = 0.3f; key_bg[3] = 1.0f;
			} else {
				key_bg[0] = 0.2f; key_bg[1] = 0.2f; key_bg[2] = 0.22f; key_bg[3] = 1.0f;
			}

			/* Highlight shift key when active */
			if (strcmp(label, "⇧") == 0 && (osk->shift_active || osk->caps_lock)) {
				key_bg[0] = 0.4f; key_bg[1] = 0.6f; key_bg[2] = 0.3f; key_bg[3] = 1.0f;
			}

			drawrect(osk->tree, key_x, key_y, actual_width, key_height, key_bg);

			/* Key border */
			float key_border[4] = {0.35f, 0.35f, 0.4f, 1.0f};
			drawrect(osk->tree, key_x, key_y, actual_width, 1, key_border);
			drawrect(osk->tree, key_x, key_y + key_height - 1, actual_width, 1, key_border);
			drawrect(osk->tree, key_x, key_y, 1, key_height, key_border);
			drawrect(osk->tree, key_x + actual_width - 1, key_y, 1, key_height, key_border);

			/* Draw label */
			const char *display_label = label;
			if (row == 4 && col == 3) display_label = "SPACE";

			int text_width = 0;
			int pen_x = 0;
			uint32_t prev_cp = 0;

			/* Calculate text width */
			const char *p = display_label;
			while (*p) {
				uint32_t cp;
				int bytes = 1;
				if ((*p & 0x80) == 0) {
					cp = *p;
				} else if ((*p & 0xE0) == 0xC0) {
					cp = (*p & 0x1F) << 6 | (*(p+1) & 0x3F);
					bytes = 2;
				} else if ((*p & 0xF0) == 0xE0) {
					cp = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F);
					bytes = 3;
				} else {
					cp = '?';
				}
				const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(statusfont.font, cp, statusbar_font_subpixel);
				if (glyph)
					text_width += glyph->advance.x;
				p += bytes;
			}

			/* Draw centered text */
			int text_x = key_x + (actual_width - text_width) / 2;
			int text_y = key_y + (key_height + statusfont.height) / 2 - statusfont.descent;

			p = display_label;
			pen_x = 0;
			prev_cp = 0;
			while (*p) {
				uint32_t cp;
				int bytes = 1;
				if ((*p & 0x80) == 0) {
					cp = *p;
				} else if ((*p & 0xE0) == 0xC0) {
					cp = (*p & 0x1F) << 6 | (*(p+1) & 0x3F);
					bytes = 2;
				} else if ((*p & 0xF0) == 0xE0) {
					cp = (*p & 0x0F) << 12 | (*(p+1) & 0x3F) << 6 | (*(p+2) & 0x3F);
					bytes = 3;
				} else {
					cp = '?';
				}

				long kern_x = 0, kern_y = 0;
				if (prev_cp)
					fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
				pen_x += (int)kern_x;

				const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(statusfont.font, cp, statusbar_font_subpixel);
				if (glyph && glyph->pix) {
					struct wlr_buffer *buffer = statusbar_buffer_from_glyph(glyph);
					if (buffer) {
						struct wlr_scene_buffer *scene_buf = wlr_scene_buffer_create(osk->tree, NULL);
						if (scene_buf) {
							wlr_scene_buffer_set_buffer(scene_buf, buffer);
							wlr_scene_node_set_position(&scene_buf->node,
								text_x + pen_x + glyph->x,
								text_y - glyph->y);
						}
						wlr_buffer_drop(buffer);
					}
				}
				if (glyph)
					pen_x += glyph->advance.x;
				prev_cp = cp;
				p += bytes;
			}
		}
	}

	wlr_scene_node_set_enabled(&osk->tree->node, 1);
	wlr_scene_node_raise_to_top(&osk->tree->node);
}

void
osk_show(Monitor *m, struct wlr_surface *target)
{
	if (!m)
		return;

	/* Hide any existing OSK on other monitors */
	osk_hide_all();

	/* Create scene tree if needed */
	if (!m->osk.tree) {
		m->osk.tree = wlr_scene_tree_create(layers[LyrBlock]);
		if (!m->osk.tree)
			return;
	}

	m->osk.visible = 1;
	m->osk.sel_row = 2;  /* Start on middle row */
	m->osk.sel_col = 5;  /* Start in middle */
	m->osk.shift_active = 0;
	m->osk.caps_lock = 0;
	m->osk.target_surface = target;

	osk_render(m);
}

void
osk_send_text(const char *text)
{
	if (!text || !text[0])
		return;

	/* Use zwp_text_input or direct keyboard events */
	/* For now, we'll use xdotool-style key injection via uinput
	 * or the simpler approach of using wtype */

	/* Fork and use wtype to send text */
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		execlp("wtype", "wtype", text, (char *)NULL);
		/* Fallback: try ydotool */
		execlp("ydotool", "ydotool", "type", text, (char *)NULL);
		_exit(127);
	}
}

void
osk_send_backspace(Monitor *m)
{
	(void)m;
	/* Send backspace using wtype */
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		execlp("wtype", "wtype", "-k", "BackSpace", (char *)NULL);
		_exit(127);
	}
}

void
osk_send_key(Monitor *m)
{
	OnScreenKeyboard *osk;
	const char *(*layout)[OSK_COLS];
	const char *key;

	if (!m)
		return;

	osk = &m->osk;
	layout = (osk->shift_active || osk->caps_lock) ? osk_layout_upper : osk_layout_lower;
	key = layout[osk->sel_row][osk->sel_col];

	/* Handle special keys */
	if (strcmp(key, "⇧") == 0) {
		/* Toggle shift */
		if (osk->shift_active) {
			osk->caps_lock = !osk->caps_lock;
			osk->shift_active = 0;
		} else {
			osk->shift_active = 1;
		}
		osk_render(m);
		return;
	}

	if (strcmp(key, "⌫") == 0) {
		/* Backspace */
		osk_send_backspace(m);
		return;
	}

	if (strcmp(key, "↵") == 0) {
		/* Enter - send enter and optionally hide keyboard */
		pid_t pid = fork();
		if (pid == 0) {
			setsid();
			execlp("wtype", "wtype", "-k", "Return", (char *)NULL);
			_exit(127);
		}
		return;
	}

	/* Regular key */
	osk_send_text(key);

	/* Turn off shift after typing (unless caps lock) */
	if (osk->shift_active && !osk->caps_lock) {
		osk->shift_active = 0;
		osk_render(m);
	}
}

int
osk_handle_button(Monitor *m, int button, int value)
{
	OnScreenKeyboard *osk;

	if (!m || !m->osk.visible)
		return 0;

	/* Only handle button press */
	if (value != 1)
		return 0;

	osk = &m->osk;

	switch (button) {
	case BTN_SOUTH:  /* A button - press selected key */
		osk_send_key(m);
		return 1;

	case BTN_EAST:   /* B button - backspace (delete last character) */
		osk_send_backspace(m);
		return 1;

	case BTN_WEST:   /* X button - toggle shift */
		osk->shift_active = !osk->shift_active;
		osk_render(m);
		return 1;

	case BTN_NORTH:  /* Y button - close keyboard */
		osk_hide(m);
		return 1;

	case BTN_START:  /* Start button - also close keyboard */
	case BTN_SELECT: /* Select button - also close keyboard */
		osk_hide(m);
		return 1;

	case BTN_DPAD_UP:
		if (osk->sel_row > 0) {
			osk->sel_row--;
			/* Skip cells covered by space bar */
			if (osk->sel_row == 4 && osk->sel_col >= 4 && osk->sel_col <= 7)
				osk->sel_col = 3;
			osk_render(m);
		}
		return 1;

	case BTN_DPAD_DOWN:
		if (osk->sel_row < OSK_ROWS - 1) {
			osk->sel_row++;
			/* Skip cells covered by space bar */
			if (osk->sel_row == 4 && osk->sel_col >= 4 && osk->sel_col <= 7)
				osk->sel_col = 3;
			osk_render(m);
		}
		return 1;

	case BTN_DPAD_LEFT:
		if (osk->sel_col > 0) {
			osk->sel_col--;
			/* Skip cells covered by space bar */
			if (osk->sel_row == 4 && osk->sel_col >= 4 && osk->sel_col <= 7)
				osk->sel_col = 3;
			/* Skip enter key duplicate */
			if (osk->sel_row == 4 && osk->sel_col == 11)
				osk->sel_col = 10;
			osk_render(m);
		}
		return 1;

	case BTN_DPAD_RIGHT:
		if (osk->sel_col < OSK_COLS - 1) {
			osk->sel_col++;
			/* Skip cells covered by space bar */
			if (osk->sel_row == 4 && osk->sel_col >= 4 && osk->sel_col <= 7)
				osk->sel_col = 8;
			/* Skip enter key duplicate */
			if (osk->sel_row == 4 && osk->sel_col == 11)
				osk->sel_col = 10;
			osk_render(m);
		}
		return 1;

	case BTN_TL:  /* Left bumper - move to start of row */
		osk->sel_col = 0;
		osk_render(m);
		return 1;

	case BTN_TR:  /* Right bumper - move to end of row */
		osk->sel_col = OSK_COLS - 1;
		if (osk->sel_row == 4)
			osk->sel_col = 10; /* Enter key */
		osk_render(m);
		return 1;
	}

	return 0;
}

int
osk_dpad_repeat_cb(void *data)
{
	(void)data;

	/* Check if still valid */
	if (!osk_dpad_held_button || !osk_dpad_held_mon || !osk_dpad_held_mon->osk.visible) {
		osk_dpad_held_button = 0;
		osk_dpad_held_mon = NULL;
		return 0;
	}

	/* Try to move in the held direction */
	OnScreenKeyboard *osk = &osk_dpad_held_mon->osk;
	int old_row = osk->sel_row;
	int old_col = osk->sel_col;

	/* Call the button handler (it will move if possible) */
	osk_handle_button(osk_dpad_held_mon, osk_dpad_held_button, 1);

	/* If position changed, schedule next repeat */
	if (osk->sel_row != old_row || osk->sel_col != old_col) {
		if (osk_dpad_repeat_timer)
			wl_event_source_timer_update(osk_dpad_repeat_timer, OSK_DPAD_REPEAT_RATE);
	} else {
		/* Hit a wall - stop repeating */
		osk_dpad_held_button = 0;
		osk_dpad_held_mon = NULL;
	}

	return 0;
}

void
osk_dpad_repeat_start(Monitor *m, int button)
{
	if (!m || !m->osk.visible)
		return;

	osk_dpad_held_button = button;
	osk_dpad_held_mon = m;

	/* Create timer if needed */
	if (!osk_dpad_repeat_timer) {
		osk_dpad_repeat_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(dpy), osk_dpad_repeat_cb, NULL);
	}

	/* Start with initial delay */
	if (osk_dpad_repeat_timer)
		wl_event_source_timer_update(osk_dpad_repeat_timer, OSK_DPAD_INITIAL_DELAY);
}

void
osk_dpad_repeat_stop(void)
{
	osk_dpad_held_button = 0;
	osk_dpad_held_mon = NULL;
	if (osk_dpad_repeat_timer)
		wl_event_source_timer_update(osk_dpad_repeat_timer, 0);
}

int
toast_hide_timer(void *data)
{
	Monitor *m = data;
	if (m && m->toast_tree) {
		wlr_scene_node_set_enabled(&m->toast_tree->node, 0);
		m->toast_visible = 0;
	}
	return 0;
}

void
toast_show(Monitor *m, const char *message, int duration_ms)
{
	struct wlr_scene_node *node, *tmp;
	int text_w, pad = 12, h = 28;
	int x, y, w;
	const float bg[4] = {0.15f, 0.15f, 0.15f, 0.95f};
	const float border[4] = {0.3f, 0.3f, 0.3f, 1.0f};
	StatusModule mod = {0};

	if (!m)
		m = selmon;
	if (!m)
		return;

	/* Create toast tree if needed */
	if (!m->toast_tree) {
		m->toast_tree = wlr_scene_tree_create(layers[LyrTop]);
		if (!m->toast_tree)
			return;
	}

	/* Clear existing content */
	wl_list_for_each_safe(node, tmp, &m->toast_tree->children, link)
		wlr_scene_node_destroy(node);

	/* Calculate size and position (top right corner) */
	text_w = status_text_width(message);
	w = text_w + pad * 2;
	x = m->m.x + m->m.width - w - 20;
	y = m->m.y + 50; /* Below status bar */

	wlr_scene_node_set_position(&m->toast_tree->node, x, y);

	/* Draw background */
	drawrect(m->toast_tree, 0, 0, w, h, bg);
	/* Draw border */
	drawrect(m->toast_tree, 0, 0, w, 1, border);
	drawrect(m->toast_tree, 0, h - 1, w, 1, border);
	drawrect(m->toast_tree, 0, 0, 1, h, border);
	drawrect(m->toast_tree, w - 1, 0, 1, h, border);

	/* Draw text */
	mod.tree = m->toast_tree;
	tray_render_label(&mod, message, pad, h, statusbar_fg);

	/* Show toast */
	wlr_scene_node_set_enabled(&m->toast_tree->node, 1);
	m->toast_visible = 1;

	/* Set up timer to hide */
	if (!m->toast_timer)
		m->toast_timer = wl_event_loop_add_timer(event_loop, toast_hide_timer, m);
	wl_event_source_timer_update(m->toast_timer, duration_ms);
}


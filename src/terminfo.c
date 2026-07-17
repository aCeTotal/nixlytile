/*
 * terminfo.c — statusbar module showing the focused terminal's context:
 * the shell's working directory, or the ssh destination when an ssh
 * session is the foreground job.  Rendered centered between the left
 * module group (tags/steam/net/tray) and the right module group, and
 * only while a terminal client has keyboard focus.
 */
#include <fcntl.h>
#include <limits.h>

#include "nixlytile.h"
#include "client.h"

int
is_terminal_client(Client *c)
{
	static const char *terms[] = {
		"Alacritty", "foot", "footclient", "kitty", "wezterm",
		"org.wezfurlong.wezterm", "st", "st-256color", "xterm",
		"konsole", "org.kde.konsole", "gnome-terminal",
		"org.gnome.Terminal", "rio", "ghostty",
		"com.mitchellh.ghostty",
	};
	const char *app;
	size_t i;

	if (!c)
		return 0;
	app = client_get_appid(c);
	if (!app)
		return 0;
	for (i = 0; i < LENGTH(terms); i++)
		if (strcasecmp(app, terms[i]) == 0)
			return 1;
	return 0;
}

/* Last entry in /proc/PID/task/PID/children — the most recently spawned
 * child, which for a shell is the foreground job. */
static pid_t
last_child(pid_t pid)
{
	char path[64];
	FILE *fp;
	long c, last = 0;

	snprintf(path, sizeof(path), "/proc/%d/task/%d/children",
			(int)pid, (int)pid);
	fp = fopen(path, "r");
	if (!fp)
		return 0;
	while (fscanf(fp, "%ld", &c) == 1)
		last = c;
	fclose(fp);
	return (pid_t)last;
}

static void
proc_comm(pid_t pid, char *buf, size_t n)
{
	char path[64];
	FILE *fp;
	char *nl;

	buf[0] = '\0';
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	fp = fopen(path, "r");
	if (!fp)
		return;
	if (fgets(buf, (int)n, fp) && (nl = strchr(buf, '\n')))
		*nl = '\0';
	fclose(fp);
}

/* Extract the destination (user@host) from an ssh process's cmdline:
 * the first argument that isn't an option or an option's value. */
static int
ssh_target(pid_t pid, char *out, size_t n)
{
	/* ssh options that consume the following argument */
	static const char optarg_opts[] = "bcDeEFiJlLmoOpQRSwW";
	char path[64], buf[4096];
	ssize_t len;
	size_t i;
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len <= 0)
		return 0;
	buf[len] = '\0';

	i = strlen(buf) + 1; /* skip argv[0] */
	while (i < (size_t)len) {
		const char *arg = buf + i;
		size_t alen = strlen(arg);

		i += alen + 1;
		if (arg[0] == '-') {
			if (alen == 2 && strchr(optarg_opts, arg[1]) &&
					i < (size_t)len)
				i += strlen(buf + i) + 1; /* skip the value */
			continue;
		}
		snprintf(out, n, "%s", arg);
		return 1;
	}
	return 0;
}

static void
terminfo_collect(Client *c, char *out, size_t n)
{
	char comm[64], target[128], cwd[PATH_MAX], path[64];
	pid_t cur, next, deepest;
	ssize_t l;
	int depth;

	out[0] = '\0';
	cur = client_get_pid(c);
	if (cur <= 1)
		return;

	/* Follow the newest-child chain down from the terminal (terminal →
	 * shell → foreground job).  An ssh anywhere on the chain wins;
	 * otherwise show the deepest process's working directory. */
	deepest = cur;
	for (depth = 0; depth < 8; depth++) {
		next = last_child(cur);
		if (next <= 0)
			break;
		cur = deepest = next;
		proc_comm(cur, comm, sizeof(comm));
		if (strcmp(comm, "ssh") == 0 &&
				ssh_target(cur, target, sizeof(target))) {
			snprintf(out, n, "ssh %s", target);
			return;
		}
	}

	snprintf(path, sizeof(path), "/proc/%d/cwd", (int)deepest);
	l = readlink(path, cwd, sizeof(cwd) - 1);
	if (l <= 0)
		return;
	cwd[l] = '\0';

	{
		const char *home = getenv("HOME");
		size_t hl = home ? strlen(home) : 0;

		if (hl && strncmp(cwd, home, hl) == 0 &&
				(cwd[hl] == '/' || cwd[hl] == '\0'))
			snprintf(out, n, "~%s", cwd + hl);
		else
			snprintf(out, n, "%s", cwd);
	}

	/* Long path: keep the tail — the most specific part */
	if (strlen(out) > 48) {
		char tail[64];
		snprintf(tail, sizeof(tail), "…%s", out + strlen(out) - 46);
		snprintf(out, n, "%s", tail);
	}
}

/* The terminal that actually holds keyboard focus.  Not focustop(): in
 * workspace mode every client matches VISIBLEON (tagset is static), so
 * focustop keeps returning the terminal after switching to an empty
 * workspace even though its keyboard focus was cleared. */
static Client *
focused_terminal(void)
{
	struct wlr_surface *surf = seat ? seat->keyboard_state.focused_surface : NULL;
	Client *c = NULL;

	if (!surf)
		return NULL;
	toplevel_from_wlr_surface(surf, &c, NULL);
	if (c && !c->isfullscreen && is_terminal_client(c) &&
			client_surface(c) && client_surface(c)->mapped)
		return c;
	return NULL;
}

int
terminfo_wants_fast_poll(void)
{
	return focused_terminal() != NULL;
}

static void
renderterminfo(StatusModule *module, int bar_height, const char *text)
{
	int padding = statusbar_module_padding;
	int text_w;

	if (!module || !module->tree)
		return;

	clearstatusmodule(module);

	text_w = (bar_height > 0 && text) ? status_text_width(text) : 0;
	if (text_w <= 0) {
		module->width = 0;
		wlr_scene_node_set_enabled(&module->tree->node, 0);
		return;
	}

	tray_render_label(module, text, padding, bar_height, statusbar_fg);
	module->width = text_w + 2 * padding;
	wlr_scene_node_set_enabled(&module->tree->node, 1);
}

void
refreshstatusterminfo(void)
{
	char text[128];
	Monitor *m;
	Client *c;
	int barh;

	text[0] = '\0';
	c = focused_terminal();
	if (c)
		terminfo_collect(c, text, sizeof(text));

	wl_list_for_each(m, &mons, link) {
		const char *t = (m == selmon) ? text : "";

		if (!m->statusbar.terminfo.tree || !m->showbar)
			continue;
		/* Hidden and staying hidden: skip — status_should_render
		 * treats a cached empty string as "always render". */
		if (!*t && m->statusbar.terminfo.width == 0)
			continue;
		barh = m->statusbar.area.height
			? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.terminfo, barh, t)
				|| (!*t && m->statusbar.terminfo.width > 0)) {
			renderterminfo(&m->statusbar.terminfo, barh, t);
			positionstatusmodules(m);
		}
	}
}

/* statusbar_conf.c — ~/.local/nixlyos/statusbar.conf: the whole statusbar in
 * one user-editable file.
 *
 *   module "<name>" "<left|middle|right>" [icons-only]
 *       One line per thing shown in the bar. File order = bar order.
 *       Remove a line to hide the module; icons-only drops the text/percent.
 *
 *   chargelimit / brightness / volume / mic <value>
 *       Persistent levels. nixlytile rewrites ONLY these lines on every
 *       manual change (the rest of the file is preserved byte for byte),
 *       and restores them at the next login. brightness -1 = automatic.
 *
 * An inotify watch (same pattern as bindings_conf.c) applies external edits
 * live — both level changes and layout/order changes. Writes from this
 * process are counted and skipped by the watcher.
 */

#include "nixlytile.h"

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATUSBARCONF_NAME "statusbar.conf"

static double sc_brightness = -1.0;   /* -1 = auto */
static double sc_volume = -1.0;       /* -1 = unset, use built-in default */
static double sc_mic = -1.0;
static int sc_chargelimit = -1;

static BarModCfg sc_mods[BARMOD_MAX];
static int sc_mod_count;

static int sc_loaded;
/* Content hash of the file as this process last wrote it. The watcher
 * compares instead of counting write events: inotify batches renames, so
 * a counter drifts and silently eats the user's next real edit. */
static unsigned long sc_last_write_hash;
static char sc_path[PATH_MAX];
static char sc_dir[PATH_MAX];

static int statusbarconf_fd = -1;
static struct wl_event_source *statusbarconf_source;

/* Every module the bar can show, in the default order. */
static const BarModCfg sc_mod_defaults[] = {
	{ "workspaces",   0, 0 },
	{ "tray",         0, 0 },
	{ "net",          0, 0 },
	{ "bluetooth",    0, 0 },
	{ "display",      0, 0 },
	{ "window-title", 1, 0 },
	{ "battery",      2, 0 },
	{ "light",        2, 0 },
	{ "volume",       2, 0 },
	{ "mic",          2, 0 },
	{ "disk",         2, 0 },
	{ "cpu",          2, 0 },
	{ "ram",          2, 0 },
	{ "clock",        2, 0 },
	{ "power",        2, 0 },
};

static unsigned long
sc_hash_file(const char *path)
{
	FILE *fp = fopen(path, "r");
	unsigned long h = 5381;
	int c;

	if (!fp)
		return 0;
	while ((c = fgetc(fp)) != EOF)
		h = h * 33 + (unsigned char)c;
	fclose(fp);
	return h;
}

static const char sc_seed[] =
"// NixlyOS statusbar — everything the bar shows, in one file.\n"
"//\n"
"//   module \"<name>\" \"<left|middle|right>\" [icons-only]\n"
"//\n"
"// One line per module; the order here is the order in the bar.\n"
"// Remove a line to hide that module. Add icons-only to show just the\n"
"// icon without the number/percent. Edits apply immediately.\n"
"//\n"
"// Modules: workspaces tray net bluetooth display window-title battery\n"
"//          light volume mic disk cpu ram clock power\n"
"\n"
"module \"workspaces\"   \"left\"\n"
"module \"tray\"         \"left\"\n"
"module \"net\"          \"left\"\n"
"module \"bluetooth\"    \"left\"\n"
"module \"display\"      \"left\"\n"
"\n"
"module \"window-title\" \"middle\"\n"
"\n"
"module \"battery\"      \"right\"\n"
"module \"light\"        \"right\"\n"
"module \"volume\"       \"right\"\n"
"module \"mic\"          \"right\"\n"
"module \"disk\"         \"right\"\n"
"module \"cpu\"          \"right\"\n"
"module \"ram\"          \"right\"\n"
"module \"clock\"        \"right\"\n"
"module \"power\"        \"right\"\n"
"\n"
"// Persistent levels — rewritten by nixlytile on every manual change and\n"
"// restored at the next login. brightness -1 = automatic (webcam ambient).\n"
"chargelimit 80\n"
"brightness -1.0\n"
"volume 80.0\n"
"mic 50.0\n";

static void
sc_resolve_path(void)
{
	const char *dir = getenv("NIXLYOS_DIR");

	if (dir && *dir) {
		snprintf(sc_dir, sizeof(sc_dir), "%s", dir);
	} else {
		const char *home = getenv("HOME");
		if (!home) {
			struct passwd *pw = getpwuid(getuid());
			if (pw)
				home = pw->pw_dir;
		}
		if (!home)
			home = "/";
		snprintf(sc_dir, sizeof(sc_dir), "%s/.local/nixlyos", home);
	}
	snprintf(sc_path, sizeof(sc_path), "%s/%s", sc_dir, STATUSBARCONF_NAME);
}

static void
sc_parse_value(const char *key, double v)
{
	if (!strcmp(key, "chargelimit")) {
		int i = (int)v;
		if (i == 80 || i == 90 || i == 100)
			sc_chargelimit = i;
	} else if (!strcmp(key, "brightness")) {
		sc_brightness = (v >= 0.0 && v <= 100.0) ? v : -1.0;
	} else if (!strcmp(key, "volume")) {
		if (v >= 0.0 && v <= 150.0)
			sc_volume = v;
	} else if (!strcmp(key, "mic")) {
		if (v >= 0.0 && v <= 150.0)
			sc_mic = v;
	}
}

static void
sc_parse(void)
{
	FILE *fp = fopen(sc_path, "r");
	char line[256];
	char key[32], name[24], side[16];
	double v;

	if (!fp)
		return;
	sc_mod_count = 0;
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, " module \"%23[^\"]\" \"%15[^\"]\"",
				name, side) == 2) {
			int s = !strcmp(side, "left") ? 0
				: !strcmp(side, "middle") ? 1
				: !strcmp(side, "right") ? 2 : -1;
			if (s < 0 || sc_mod_count >= BARMOD_MAX)
				continue;
			snprintf(sc_mods[sc_mod_count].name,
				sizeof(sc_mods[sc_mod_count].name), "%s", name);
			sc_mods[sc_mod_count].side = s;
			sc_mods[sc_mod_count].icons_only =
				strstr(line, "icons-only") != NULL;
			sc_mod_count++;
		} else if (sscanf(line, " %31[a-z] %lf", key, &v) == 2) {
			sc_parse_value(key, v);
		}
	}
	fclose(fp);
	/* No module lines at all (stripped file): show everything rather
	 * than an empty bar. An intentional subset still works — one line
	 * is enough. */
	if (sc_mod_count == 0) {
		memcpy(sc_mods, sc_mod_defaults, sizeof(sc_mod_defaults));
		sc_mod_count = (int)(sizeof(sc_mod_defaults) / sizeof(sc_mod_defaults[0]));
	}
}

/* One-time migration from the old per-value files. */
static void
sc_migrate_legacy(void)
{
	char path[PATH_MAX];
	FILE *fp;
	double d;
	int i;

	snprintf(path, sizeof(path), "%s/brightness.conf", sc_dir);
	fp = fopen(path, "r");
	if (fp) {
		if (fscanf(fp, "manual %lf", &d) == 1 && d >= 0.0 && d <= 100.0)
			sc_brightness = d;
		fclose(fp);
	}
	snprintf(path, sizeof(path), "%s/charge_limit.conf", sc_dir);
	fp = fopen(path, "r");
	if (fp) {
		if (fscanf(fp, "limit %d", &i) == 1 &&
				(i == 80 || i == 90 || i == 100))
			sc_chargelimit = i;
		fclose(fp);
	}
}

/* Rewrite ONLY the four value lines; every other byte (module order,
 * comments, whitespace) is the user's and survives untouched. Missing
 * value lines are appended. */
static void
sc_save(void)
{
	char tmp[PATH_MAX + 8];
	char line[256];
	FILE *in, *out;
	int done_cl = 0, done_b = 0, done_v = 0, done_m = 0;
	char key[32];
	double dummy;

	if (!sc_path[0])
		sc_resolve_path();
	mkdir(sc_dir, 0755);
	snprintf(tmp, sizeof(tmp), "%s.tmp", sc_path);
	out = fopen(tmp, "w");
	if (!out)
		return;

	in = fopen(sc_path, "r");
	if (!in) {
		fputs(sc_seed, out);
		fclose(out);
		if (rename(tmp, sc_path) != 0) {
			unlink(tmp);
			return;
		}
		/* Seed written; run again so the migrated values land. */
		sc_save();
		return;
	}
	{
		while (fgets(line, sizeof(line), in)) {
			if (sscanf(line, " %31[a-z] %lf", key, &dummy) == 2 &&
					strcmp(key, "module") != 0) {
				if (!strcmp(key, "chargelimit") && !done_cl) {
					fprintf(out, "chargelimit %d\n",
						sc_chargelimit >= 0 ? sc_chargelimit : 80);
					done_cl = 1;
					continue;
				}
				if (!strcmp(key, "brightness") && !done_b) {
					fprintf(out, "brightness %.1f\n", sc_brightness);
					done_b = 1;
					continue;
				}
				if (!strcmp(key, "volume") && !done_v) {
					fprintf(out, "volume %.1f\n",
						sc_volume >= 0.0 ? sc_volume : 80.0);
					done_v = 1;
					continue;
				}
				if (!strcmp(key, "mic") && !done_m) {
					fprintf(out, "mic %.1f\n",
						sc_mic >= 0.0 ? sc_mic : 50.0);
					done_m = 1;
					continue;
				}
			}
			fputs(line, out);
		}
		fclose(in);
	}

	if (!done_cl)
		fprintf(out, "chargelimit %d\n",
			sc_chargelimit >= 0 ? sc_chargelimit : 80);
	if (!done_b)
		fprintf(out, "brightness %.1f\n", sc_brightness);
	if (!done_v)
		fprintf(out, "volume %.1f\n", sc_volume >= 0.0 ? sc_volume : 80.0);
	if (!done_m)
		fprintf(out, "mic %.1f\n", sc_mic >= 0.0 ? sc_mic : 50.0);
	fclose(out);

	if (rename(tmp, sc_path) != 0) {
		unlink(tmp);
		return;
	}
	sc_last_write_hash = sc_hash_file(sc_path);
}

static void
sc_load(void)
{
	if (sc_loaded)
		return;
	sc_loaded = 1;
	sc_resolve_path();
	if (access(sc_path, R_OK) == 0) {
		sc_parse();
		sc_last_write_hash = sc_hash_file(sc_path);
		return;
	}
	sc_migrate_legacy();
	sc_save();
	sc_parse();
}

double
status_conf_brightness(void)
{
	sc_load();
	return sc_brightness;
}

double
status_conf_volume(void)
{
	sc_load();
	return sc_volume;
}

double
status_conf_mic(void)
{
	sc_load();
	return sc_mic;
}

int
status_conf_chargelimit(void)
{
	sc_load();
	return sc_chargelimit;
}

void
status_conf_update(const char *key, double value)
{
	sc_load();
	if (!strcmp(key, "brightness"))
		sc_brightness = value;
	else if (!strcmp(key, "volume"))
		sc_volume = value;
	else if (!strcmp(key, "mic"))
		sc_mic = value;
	else if (!strcmp(key, "chargelimit"))
		sc_chargelimit = (int)value;
	else
		return;
	sc_save();
}

int
barmod_count(void)
{
	sc_load();
	return sc_mod_count;
}

const BarModCfg *
barmod_get(int i)
{
	sc_load();
	if (i < 0 || i >= sc_mod_count)
		return NULL;
	return &sc_mods[i];
}

const BarModCfg *
barmod_find(const char *name)
{
	int i;

	sc_load();
	for (i = 0; i < sc_mod_count; i++)
		if (!strcmp(sc_mods[i].name, name))
			return &sc_mods[i];
	return NULL;
}

/* External edit of statusbar.conf: re-read, push changed levels to the
 * hardware/PipeWire through the same setters manual changes use (they
 * save again — one guarded self-write, no loop), and re-render/relayout
 * the bar for module/order/icons-only changes. */
static void
sc_apply_external(void)
{
	double ob = sc_brightness, ov = sc_volume, om = sc_mic;
	int oc = sc_chargelimit;

	sc_parse();
	sc_last_write_hash = sc_hash_file(sc_path);

	if (sc_brightness != ob) {
		if (sc_brightness >= 0.0) {
			light_mode_set_manual(sc_brightness);
			if (backlight_available &&
					set_backlight_percent(sc_brightness) == 0) {
				light_last_percent = sc_brightness;
				light_cached_percent = sc_brightness;
				refreshstatuslight();
			}
		} else {
			light_mode_set_auto();
		}
	}
	if (sc_volume != ov && sc_volume >= 0.0)
		set_pipewire_volume(sc_volume);
	if (sc_mic != om && sc_mic >= 0.0)
		set_pipewire_mic_volume(sc_mic);
	if (sc_chargelimit != oc &&
			(sc_chargelimit == 80 || sc_chargelimit == 90 ||
			 sc_chargelimit == 100))
		charge_limit_set(sc_chargelimit);

	statusbar_conf_changed();
}

static int
statusbarconf_readable(int fd, uint32_t mask, void *data)
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
			if (ev->len && strcmp(ev->name, STATUSBARCONF_NAME) == 0)
				relevant = 1;
			p += sizeof(*ev) + ev->len;
		}
	}

	if (relevant && sc_hash_file(sc_path) != sc_last_write_hash)
		sc_apply_external();
	return 0;
}

void
setup_statusbar_conf_watch(void)
{
	int wd;

	sc_load();

	statusbarconf_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (statusbarconf_fd < 0) {
		wlr_log(WLR_ERROR, "statusbar_conf: inotify_init failed: %s",
			strerror(errno));
		return;
	}

	wd = inotify_add_watch(statusbarconf_fd, sc_dir,
		IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF);
	if (wd < 0) {
		wlr_log(WLR_INFO, "statusbar_conf: no %s yet (%s)", sc_dir,
			strerror(errno));
		close(statusbarconf_fd);
		statusbarconf_fd = -1;
		return;
	}

	statusbarconf_source = wl_event_loop_add_fd(event_loop, statusbarconf_fd,
		WL_EVENT_READABLE, statusbarconf_readable, NULL);
	if (!statusbarconf_source) {
		close(statusbarconf_fd);
		statusbarconf_fd = -1;
		return;
	}
	wlr_log(WLR_INFO, "statusbar_conf: watching %s", sc_path);
}

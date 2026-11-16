#include "nixlytile.h"
#include "vrslayer/link.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define GAMINGCONF_NAME "gaming.conf"
#define DYNAMIC_RENDER_KEY "dynamic-render"
#define FOVEA_MIN_PCT 5
#define FOVEA_DEFAULT_PCT 40
#define BLEND_DEFAULT_PCT 15
#define QUALITY_DEFAULT 0
#define ZONE_MAX_PCT 100
#define PERMILLE_PER_PCT (VRS_SIZE_UNIT / 100u)

struct vrs_zones vrs_zones;

/* Sizes: percent of screen height. */
struct zone_key {
	const char *key;
	uint32_t *value;
	long min, max, scale;
};

static const struct zone_key zone_keys[] = {
	{ "fovea-size", &vrs_zones.fovea, FOVEA_MIN_PCT, ZONE_MAX_PCT, PERMILLE_PER_PCT },
	{ "blend-size", &vrs_zones.blend, 0, ZONE_MAX_PCT, PERMILLE_PER_PCT },
	{ "periphery-quality", &vrs_zones.quality, 0, VRS_QUALITY_MAX, 1 },
};

static char gamingconf_dir[PATH_MAX];
static char gamingconf_path[PATH_MAX];
static int gamingconf_fd = -1;
static struct wl_event_source *gamingconf_source;

static void
gamingconf_resolve_paths(void)
{
	const char *home = getenv("HOME");
	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw)
			home = pw->pw_dir;
	}
	if (!home)
		home = "/";
	snprintf(gamingconf_dir, sizeof(gamingconf_dir),
		"%s/.local/nixlyos", home);
	snprintf(gamingconf_path, sizeof(gamingconf_path),
		"%s/.local/nixlyos/" GAMINGCONF_NAME, home);
}

/* Clamps; garbage keeps the default. */
static void
set_zone(const struct zone_key *z, const char *value)
{
	char *end;
	long v = strtol(value, &end, 10);

	if (end == value || *end)
		return;
	*z->value = (uint32_t)(MIN(MAX(v, z->min), z->max) * z->scale);
}

static void
add_line(char *line)
{
	char *eq = strchr(line, '=');
	size_t i;

	if (line[0] == '#' || !eq)
		return;
	*eq = '\0';
	if (strcmp(line, DYNAMIC_RENDER_KEY) == 0) {
		dynamic_render_enabled = strcmp(eq + 1, "0") != 0;
		return;
	}
	if (gaze_conf_parse(line, eq + 1))
		return;
	mic_bind_parse(line, eq + 1);
	for (i = 0; i < LENGTH(zone_keys); i++)
		if (strcmp(line, zone_keys[i].key) == 0)
			set_zone(&zone_keys[i], eq + 1);
}

static void
gaming_conf_load(void)
{
	char line[256];
	FILE *fp;

	mic_bind_clear();
	gaze_conf_reset();
	dynamic_render_enabled = 1;
	vrs_zones = (struct vrs_zones){
		.fovea = FOVEA_DEFAULT_PCT * PERMILLE_PER_PCT,
		.blend = BLEND_DEFAULT_PCT * PERMILLE_PER_PCT,
		.quality = QUALITY_DEFAULT,
	};
	fp = fopen(gamingconf_path, "r");
	if (fp) {
		while (fgets(line, sizeof(line), fp)) {
			line[strcspn(line, "\n")] = '\0';
			add_line(line);
		}
		fclose(fp);
	}
	mic_bind_apply();
	nx_log(NX_INFO, "gaming.conf: %zu mic bind(s), dynamic rendering %s, "
		"fovea %u%%, blend %u%%, periphery quality %u",
		mic_bind_count(), dynamic_render_enabled ? "on" : "off",
		vrs_zones.fovea / PERMILLE_PER_PCT, vrs_zones.blend / PERMILLE_PER_PCT,
		vrs_zones.quality);
}

static int
gamingconf_readable(int fd, uint32_t mask, void *data)
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
			if (ev->len && strcmp(ev->name, GAMINGCONF_NAME) == 0)
				relevant = 1;
			p += sizeof(*ev) + ev->len;
		}
	}

	if (relevant)
		gaming_conf_load();
	return 0;
}

void
gaming_conf_setup(void)
{
	int wd;

	gamingconf_resolve_paths();
	gaming_conf_load();

	mkdir(gamingconf_dir, 0755);

	gamingconf_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (gamingconf_fd < 0) {
		nx_log(NX_ERROR, "gaming.conf: inotify_init failed: %s",
			strerror(errno));
		return;
	}

	wd = inotify_add_watch(gamingconf_fd, gamingconf_dir,
		IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
	if (wd < 0) {
		nx_log(NX_ERROR, "gaming.conf: inotify_add_watch(%s) failed: %s",
			gamingconf_dir, strerror(errno));
		close(gamingconf_fd);
		gamingconf_fd = -1;
		return;
	}

	gamingconf_source = wl_event_loop_add_fd(event_loop, gamingconf_fd,
		WL_EVENT_READABLE, gamingconf_readable, NULL);
	nx_log(NX_INFO, "gaming.conf: watching %s", gamingconf_path);
}

void
gaming_conf_cleanup(void)
{
	if (gamingconf_source) {
		wl_event_source_remove(gamingconf_source);
		gamingconf_source = NULL;
	}
	if (gamingconf_fd >= 0) {
		close(gamingconf_fd);
		gamingconf_fd = -1;
	}
	mic_bind_clear();
}

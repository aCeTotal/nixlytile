/* monitor_setup.c — screen ID boxes and drag glow, driven by nixlycc.
 *
 * While its Monitors page is open, nixlycc writes
 * ~/.local/nixlyos/monitor_overlay:
 *
 *   eDP-1 AU Optronics #1 (300Hz)
 *   DP-2 Samsung #2 (144Hz)
 *   highlight=DP-2
 *
 * One line per monitor — connector name first, then the label to show in the
 * top-left corner of that screen, so it is obvious which box in the panel is
 * which physical display. The box shows the label on the first row and the
 * connector on the second. The optional highlight line names the screen whose
 * box is being dragged right now; that screen gets a red glow that fades in
 * and stays up until the box is dropped, then fades back out.
 *
 * Deleting the file takes the whole overlay down. An inotify watch on the
 * directory keeps this in step with nixlycc.
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

#define MONOVL_FILENAME "monitor_overlay"
#define MONOVL_MAX      MAX_MONITORS
#define MONOVL_MARGIN   28    /* label box inset from the screen corner */
#define MONOVL_PADDING  14
#define MONOVL_FADE_MS  220.0f /* nothing → full glow */
#define MONOVL_GLOW_MAX 0.30f  /* alpha at full glow */
#define MONOVL_TICK_MS  16

typedef struct {
	char name[64];
	char label[128];
	struct wlr_scene_tree *tree;  /* label box */
	struct wlr_scene_rect *glow;  /* full-screen red wash */
	float glow_alpha;
	int glow_target;              /* 1 while this screen is being dragged */
} MonovlEntry;

static MonovlEntry monovl[MONOVL_MAX];
static int monovl_count;
static struct wl_event_source *monovl_anim_timer;
static char monovl_path[PATH_MAX];

static const float monovl_box_bg[4] = { 0.07f, 0.07f, 0.09f, 0.88f };

static Monitor *
monovl_monitor(const char *name)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		if (m->wlr_output && m->wlr_output->enabled &&
		    strcmp(m->wlr_output->name, name) == 0)
			return m;
	return NULL;
}

static void
monovl_drop_label(MonovlEntry *e)
{
	if (e->tree) {
		wlr_scene_node_destroy(&e->tree->node);
		e->tree = NULL;
	}
}

static void
monovl_drop_glow(MonovlEntry *e)
{
	if (e->glow) {
		wlr_scene_node_destroy(&e->glow->node);
		e->glow = NULL;
	}
	e->glow_alpha = 0.0f;
	e->glow_target = 0;
}

static void
monovl_clear(void)
{
	int i;
	for (i = 0; i < monovl_count; i++) {
		monovl_drop_label(&monovl[i]);
		monovl_drop_glow(&monovl[i]);
	}
	monovl_count = 0;
	if (monovl_anim_timer)
		wl_event_source_timer_update(monovl_anim_timer, 0);
}

/* The glow sits under the label so the label stays readable at full glow. */
static void
monovl_place_glow(MonovlEntry *e, Monitor *m)
{
	float color[4] = { 0.85f, 0.10f, 0.12f, 0.0f };

	if (!e->glow) {
		e->glow = wlr_scene_rect_create(layers[LyrOverlay],
			m->m.width, m->m.height, color);
		if (!e->glow)
			return;
	}
	wlr_scene_rect_set_size(e->glow, m->m.width, m->m.height);
	wlr_scene_node_set_position(&e->glow->node, m->m.x, m->m.y);
	color[3] = e->glow_alpha * MONOVL_GLOW_MAX;
	wlr_scene_rect_set_color(e->glow, color);
	wlr_scene_node_lower_to_bottom(&e->glow->node);
}

/* Two rows: the label from nixlycc, then the connector this screen hangs off
 * — the box is the only place that mapping is visible. */
static void
monovl_place_label(MonovlEntry *e, Monitor *m)
{
	int text_w, name_w, wide, box_w, box_h, row_h;

	monovl_drop_label(e);
	if (!statusfont.font)
		return;

	row_h = statusfont.font->height;
	text_w = status_text_width(e->label);
	name_w = status_text_width(e->name);
	if (text_w <= 0)
		return;

	wide = name_w > text_w ? name_w : text_w;
	box_w = wide + MONOVL_PADDING * 2;
	box_h = row_h * 2 + MONOVL_PADDING;

	e->tree = wlr_scene_tree_create(layers[LyrOverlay]);
	if (!e->tree)
		return;

	drawroundedrect(e->tree, 0, 0, box_w, box_h, monovl_box_bg);
	tray_menu_draw_text(e->tree, e->label, MONOVL_PADDING, 0, box_h / 2, wide);
	tray_menu_draw_text(e->tree, e->name, MONOVL_PADDING, box_h / 2, box_h / 2, wide);
	wlr_scene_node_set_position(&e->tree->node,
		m->m.x + MONOVL_MARGIN, m->m.y + MONOVL_MARGIN);
}

/* Rebuilds every label and repositions the glows — cheap enough to just redo
 * on each file change or layout change. */
static void
monovl_render(void)
{
	int i;
	for (i = 0; i < monovl_count; i++) {
		MonovlEntry *e = &monovl[i];
		Monitor *m = monovl_monitor(e->name);

		if (!m) {
			monovl_drop_label(e);
			monovl_drop_glow(e);
			continue;
		}
		monovl_place_label(e, m);
		if (e->glow || e->glow_target)
			monovl_place_glow(e, m);
	}
}

static int
monovl_anim_tick(void *data)
{
	float step = (float)MONOVL_TICK_MS / MONOVL_FADE_MS;
	int busy = 0, i;
	(void)data;

	for (i = 0; i < monovl_count; i++) {
		MonovlEntry *e = &monovl[i];
		float target = e->glow_target ? 1.0f : 0.0f;
		Monitor *m;

		if (e->glow_alpha == target) {
			if (!e->glow_target && e->glow)
				monovl_drop_glow(e); /* faded out — nothing to draw */
			continue;
		}

		if (e->glow_alpha < target) {
			e->glow_alpha += step;
			if (e->glow_alpha > target)
				e->glow_alpha = target;
		} else {
			e->glow_alpha -= step;
			if (e->glow_alpha < target)
				e->glow_alpha = target;
		}

		m = monovl_monitor(e->name);
		if (m)
			monovl_place_glow(e, m);
		busy = 1;
	}

	if (busy && monovl_anim_timer)
		wl_event_source_timer_update(monovl_anim_timer, MONOVL_TICK_MS);
	return 0;
}

static void
monovl_anim_start(void)
{
	if (!monovl_anim_timer)
		monovl_anim_timer = wl_event_loop_add_timer(event_loop,
			monovl_anim_tick, NULL);
	if (monovl_anim_timer)
		wl_event_source_timer_update(monovl_anim_timer, MONOVL_TICK_MS);
}

static void
monovl_resolve_paths(char *dir, size_t dircap, char *file, size_t filecap)
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
	snprintf(file, filecap, "%s/.local/nixlyos/" MONOVL_FILENAME, home);
}

/* Re-reads the file and brings the overlay in line with it. Also safe to call
 * after a layout change, to move the boxes with their screens. */
void
monitor_overlay_update(void)
{
	char dir[PATH_MAX];
	char line[256];
	char highlight[64] = "";
	MonovlEntry prev[MONOVL_MAX];
	int prev_count = monovl_count;
	FILE *fp;
	int i, j;

	if (!monovl_path[0])
		monovl_resolve_paths(dir, sizeof(dir), monovl_path, sizeof(monovl_path));

	fp = fopen(monovl_path, "r");
	if (!fp) {
		monovl_clear();
		return;
	}

	/* Keep the live glow state across a reload: nixlycc rewrites the whole
	 * file on every drag step, and the fade must not restart each time. */
	memcpy(prev, monovl, sizeof(prev));
	for (i = 0; i < monovl_count; i++)
		monovl_drop_label(&monovl[i]);
	monovl_count = 0;

	while (fgets(line, sizeof(line), fp)) {
		char *nl = strchr(line, '\n');
		char *sep;
		MonovlEntry *e;

		if (nl)
			*nl = '\0';
		if (!line[0])
			continue;
		if (strncmp(line, "highlight=", 10) == 0) {
			snprintf(highlight, sizeof(highlight), "%.63s", line + 10);
			continue;
		}
		if (monovl_count >= MONOVL_MAX)
			continue;

		sep = strchr(line, ' ');
		if (!sep)
			continue;
		*sep = '\0';

		e = &monovl[monovl_count++];
		memset(e, 0, sizeof(*e));
		snprintf(e->name, sizeof(e->name), "%.63s", line);
		snprintf(e->label, sizeof(e->label), "%.127s", sep + 1);
	}
	fclose(fp);

	/* Carry the glow over to the entry with the same connector; anything
	 * left in prev is a screen that vanished from the file. */
	for (i = 0; i < monovl_count; i++) {
		for (j = 0; j < prev_count; j++) {
			if (strcmp(monovl[i].name, prev[j].name) != 0)
				continue;
			monovl[i].glow = prev[j].glow;
			monovl[i].glow_alpha = prev[j].glow_alpha;
			prev[j].glow = NULL;
			break;
		}
		monovl[i].glow_target =
			(highlight[0] && strcmp(monovl[i].name, highlight) == 0);
	}
	for (j = 0; j < prev_count; j++)
		monovl_drop_glow(&prev[j]);

	monovl_render();
	monovl_anim_start();
}

/* ── inotify watch ────────────────────────────────────────────────── */

static int
monovl_watch_cb(int fd, uint32_t mask, void *data)
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
			if (ev->len && strcmp(ev->name, MONOVL_FILENAME) == 0)
				relevant = 1;
			p += sizeof(*ev) + ev->len;
		}
	}

	if (relevant)
		monitor_overlay_update();
	return 0;
}

void
setup_monitor_overlay_watch(void)
{
	char dir[PATH_MAX];

	monovl_resolve_paths(dir, sizeof(dir), monovl_path, sizeof(monovl_path));
	mkdir(dir, 0755);

	monovl_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (monovl_inotify_fd < 0) {
		wlr_log(WLR_ERROR, "monitor_overlay: inotify_init failed: %s",
			strerror(errno));
		return;
	}

	monovl_watch_wd = inotify_add_watch(monovl_inotify_fd, dir,
		IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM);
	if (monovl_watch_wd < 0) {
		wlr_log(WLR_ERROR, "monitor_overlay: inotify_add_watch(%s) failed: %s",
			dir, strerror(errno));
		close(monovl_inotify_fd);
		monovl_inotify_fd = -1;
		return;
	}

	monovl_watch_source = wl_event_loop_add_fd(event_loop,
		monovl_inotify_fd, WL_EVENT_READABLE, monovl_watch_cb, NULL);

	/* nixlycc may already be showing the page when we start. */
	monitor_overlay_update();
	wlr_log(WLR_INFO, "monitor_overlay: watching %s", dir);
}

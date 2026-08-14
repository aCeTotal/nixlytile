/*
 * Instrument screens (dcspitd).
 *
 * dcspitd opens one window per configured HDMI/DP output with the app id
 * "dcspit-screen-<connector>", drawing the instruments exactly where the user
 * placed them in dcspit's screen editor.  The compositor's job is to let that
 * window own its output while a game runs: full output geometry, no border, no
 * tiling, no keyboard focus, and above everything a game can put on screen.
 *
 * The moment the game stops — quit, alt-tab out of fullscreen, crash — the
 * window is hidden again and the output is an ordinary desktop.  Nothing about
 * the workspace on that output is touched, so "back to normal" needs no
 * restore step: the instrument node simply stops being drawn.
 */
#include "nixlytile.h"
#include "client.h"

#define INSTRUMENT_APPID "dcspit-screen-"
/* dcspitd writes this next to the other runtime sockets. */
#define VIEWPORT_PLAN "dcspit-viewports"
/* How often the plan file is re-read while a game runs. */
#define PLAN_POLL_MS 2000

/*
 * Exported cockpit displays.
 *
 * dcspitd generates a DCS MonitorSetup that draws each exported viewport at a
 * known rectangle inside the DCS window, and tells us where that rectangle
 * belongs on the instrument screen.  We mirror it: a second scene node for the
 * game's own surface, cropped to the source box and scaled onto the cell.  No
 * copy, no capture, the same frame the pilot sees.
 */
struct Mirror {
	struct wl_list link;
	struct wlr_scene_surface *scene;
};

static struct wl_list mirrors;             /* struct Mirror */
static struct wlr_scene_tree *mirror_tree;
static struct wl_event_source *plan_timer;
static time_t plan_mtime;

/* The connector an instrument window belongs to, e.g. "DP-1". */
static const char *
instrument_connector(Client *c)
{
	const char *appid = client_get_appid(c);

	if (!appid || strncmp(appid, INSTRUMENT_APPID, strlen(INSTRUMENT_APPID)))
		return NULL;
	return appid + strlen(INSTRUMENT_APPID);
}

static Monitor *
monitor_named(const char *name)
{
	Monitor *m;

	if (!name || !*name)
		return NULL;
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output && m->wlr_output->name
				&& !strcmp(m->wlr_output->name, name))
			return m;
	}
	return NULL;
}

/*
 * Instruments belong to the game, not to the desktop: no game, no takeover.
 * game_mode_active is the same signal the rest of the compositor trusts for
 * "a game is on screen", so leaving the game is the same event everywhere.
 */
int
instruments_should_show(void)
{
	return game_mode_active;
}

/* The output the game renders on; it must never show anything of ours. */
static Monitor *
game_monitor(void)
{
	return game_mode_client ? game_mode_client->mon : NULL;
}

/* Full output, no border, on top; called on adopt and whenever things move. */
static void
instruments_place(Client *c)
{
	if (!c->mon)
		return;
	c->bw = 0;
	c->geom = c->mon->m;
	wlr_scene_node_reparent(&c->scene->node, layers[LyrOverlay]);
	wlr_scene_node_raise_to_top(&c->scene->node);
	resize(c, c->geom, 1);
}

int
instruments_try_adopt(Client *c)
{
	const char *connector = instrument_connector(c);
	Monitor *m;

	if (!connector)
		return 0;
	m = monitor_named(connector);
	if (!m) {
		wlr_log(WLR_ERROR, "instruments: no output named '%s' — "
			"leaving the dcspit window to the normal layout", connector);
		return 0;
	}

	c->is_instrument = 1;
	c->isfloating = 1;
	/* Sticky: the instruments stay put when the user switches workspace. */
	c->issticky = 1;
	setmon(c, m, 0);
	instruments_place(c);
	/* instruments_update() decides visibility, including the game-screen rule. */
	wlr_scene_node_set_enabled(&c->scene->node, 0);
	instruments_update();
	wlr_log(WLR_INFO, "instruments: '%s' claims output %s", client_get_appid(c),
		m->wlr_output->name);
	return 1;
}

void
instruments_release(Client *c)
{
	if (!c || !c->is_instrument)
		return;
	c->is_instrument = 0;
	/* The output needs no restore: its workspace was never taken away. */
	if (c->mon)
		arrange(c->mon);
}

/* A screen that renders the game shows the game, nothing else. */
static int
instruments_allowed_on(Monitor *m)
{
	return m && m != game_monitor();
}

static void
mirrors_clear(void)
{
	struct Mirror *mir, *tmp;

	wl_list_for_each_safe(mir, tmp, &mirrors, link) {
		/* Destroying the node is enough; the surface itself is the game's. */
		if (mir->scene)
			wlr_scene_node_destroy(&mir->scene->buffer->node);
		wl_list_remove(&mir->link);
		free(mir);
	}
}

static void
mirror_add(Monitor *m, const char *name, int sx, int sy, int sw, int sh,
		int dx, int dy, int dw, int dh)
{
	struct wlr_surface *surface;
	struct wlr_scene_surface *mirror;
	struct wlr_fbox src;
	struct Mirror *mir;

	if (!game_mode_client || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return;
	surface = client_surface(game_mode_client);
	if (!surface)
		return;
	if (!mirror_tree) {
		mirror_tree = wlr_scene_tree_create(layers[LyrOverlay]);
		if (!mirror_tree)
			return;
	}
	mirror = wlr_scene_surface_create(mirror_tree, surface);
	if (!mirror)
		return;

	src.x = sx;
	src.y = sy;
	src.width = sw;
	src.height = sh;
	wlr_scene_buffer_set_source_box(mirror->buffer, &src);
	wlr_scene_buffer_set_dest_size(mirror->buffer, dw, dh);
	wlr_scene_node_set_position(&mirror->buffer->node, m->m.x + dx, m->m.y + dy);

	mir = calloc(1, sizeof(*mir));
	if (!mir) {
		wlr_scene_node_destroy(&mirror->buffer->node);
		return;
	}
	mir->scene = mirror;
	wl_list_insert(&mirrors, &mir->link);
	wlr_log(WLR_INFO, "instruments: mirroring %s %dx%d+%d+%d → %s +%d+%d",
		name, sw, sh, sx, sy, m->wlr_output->name, dx, dy);
}

/* dcspitd's plan file: `game W H`, `screen NAME`, `viewport NAME src .. dst ..` */
static void
mirrors_build(void)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char path[PATH_MAX];
	char line[512];
	FILE *f;
	Monitor *m = NULL;

	mirrors_clear();
	if (!runtime || !instruments_should_show() || !game_mode_client)
		return;
	snprintf(path, sizeof path, "%s/%s", runtime, VIEWPORT_PLAN);
	f = fopen(path, "r");
	if (!f)
		return;

	while (fgets(line, sizeof line, f)) {
		char name[128];
		int sx, sy, sw, sh, dx, dy, dw, dh;

		if (sscanf(line, "screen %127s", name) == 1) {
			m = monitor_named(name);
			if (m && !instruments_allowed_on(m)) {
				wlr_log(WLR_INFO, "instruments: %s renders the game, "
					"skipping its viewports", name);
				m = NULL;
			}
			continue;
		}
		if (!m)
			continue;
		if (sscanf(line, "viewport %127s src %d %d %d %d dst %d %d %d %d",
				name, &sx, &sy, &sw, &sh, &dx, &dy, &dw, &dh) == 9)
			mirror_add(m, name, sx, sy, sw, sh, dx, dy, dw, dh);
	}
	fclose(f);

	/* The cockpit displays go on top of the instrument window. */
	if (mirror_tree)
		wlr_scene_node_raise_to_top(&mirror_tree->node);
}

/* Re-read the plan when dcspitd rewrites it: layout saved, aircraft changed. */
static int
plan_poll_cb(void *data)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char path[PATH_MAX];
	struct stat st;

	if (runtime && instruments_should_show()) {
		snprintf(path, sizeof path, "%s/%s", runtime, VIEWPORT_PLAN);
		if (stat(path, &st) == 0 && st.st_mtime != plan_mtime) {
			plan_mtime = st.st_mtime;
			mirrors_build();
		}
		wl_event_source_timer_update(plan_timer, PLAN_POLL_MS);
	}
	return 0;
}

/*
 * Called when game mode flips and when outputs change.  Showing means
 * re-placing as well: a mode change or a hotplug moves the output box.
 */
void
instruments_update(void)
{
	int show = instruments_should_show();
	Client *c;

	if (!mirrors.next)
		wl_list_init(&mirrors);

	wl_list_for_each(c, &clients, link) {
		if (!c->is_instrument)
			continue;
		int visible = show && instruments_allowed_on(c->mon);
		if (visible)
			instruments_place(c);
		wlr_scene_node_set_enabled(&c->scene->node, visible);
	}

	plan_mtime = 0;
	mirrors_build();

	if (show) {
		if (!plan_timer)
			plan_timer = wl_event_loop_add_timer(event_loop, plan_poll_cb, NULL);
		if (plan_timer)
			wl_event_source_timer_update(plan_timer, PLAN_POLL_MS);
	} else if (plan_timer) {
		wl_event_source_timer_update(plan_timer, 0);
	}
}

/* An instrument window must never be counted as tiled content. */
int
instruments_owns(Monitor *m)
{
	Client *c;

	if (!instruments_should_show())
		return 0;
	wl_list_for_each(c, &clients, link) {
		if (c->is_instrument && c->mon == m)
			return 1;
	}
	return 0;
}

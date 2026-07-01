/* config_loader.c — read ~/.config/nixlytile/config.kdl and apply to
 * runtime globals.  Supports initial load and hot-reload (SIGUSR1).
 *
 * Everything that should be runtime-tweakable lives in config.h as a
 * mutable global; the loader overwrites those values from the parsed
 * KDL document.  Compile-time fallbacks remain authoritative when the
 * config file is missing or a particular field is absent.
 */

#include "nixlytile.h"
#include "client.h"
#include "config_parser.h"
#include "config_loader.h"

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

char nixlytile_config_path[PATH_MAX] = {0};

/* ── color parsing ────────────────────────────────────────────────── */

static int
hex_nibble(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + c - 'a';
	if (c >= 'A' && c <= 'F') return 10 + c - 'A';
	return -1;
}

/* Parses "#rgb", "#rgba", "#rrggbb", "#rrggbbaa" into float[4]. */
static int
parse_color(const char *s, float out[4])
{
	if (!s) return 0;
	while (*s == ' ' || *s == '\t') s++;
	if (*s == '#') s++;
	size_t n = strlen(s);
	while (n && (s[n-1] == ' ' || s[n-1] == '\t')) n--;
	int r, g, b, a = 255;
	if (n == 3 || n == 4) {
		int v[4] = {0, 0, 0, 15};
		for (size_t i = 0; i < n; i++) {
			int h = hex_nibble(s[i]);
			if (h < 0) return 0;
			v[i] = h | (h << 4);
		}
		r = v[0]; g = v[1]; b = v[2]; a = v[3];
	} else if (n == 6 || n == 8) {
		int v[4] = {0, 0, 0, 0xff};
		for (size_t i = 0; i < n / 2; i++) {
			int hi = hex_nibble(s[i*2]), lo = hex_nibble(s[i*2+1]);
			if (hi < 0 || lo < 0) return 0;
			v[i] = (hi << 4) | lo;
		}
		r = v[0]; g = v[1]; b = v[2]; a = v[3];
	} else {
		return 0;
	}
	out[0] = r / 255.0f;
	out[1] = g / 255.0f;
	out[2] = b / 255.0f;
	out[3] = a / 255.0f;
	return 1;
}

/* ── modifier / key parsing ───────────────────────────────────────── */

static uint32_t
mod_from_token(const char *tok)
{
	if (!strcasecmp(tok, "Mod") || !strcasecmp(tok, "Super") ||
	    !strcasecmp(tok, "Logo") || !strcasecmp(tok, "Win"))
		return modkey ? modkey : WLR_MODIFIER_LOGO;
	if (!strcasecmp(tok, "MonitorMod") || !strcasecmp(tok, "MonKey"))
		return monitorkey ? monitorkey : WLR_MODIFIER_CTRL;
	if (!strcasecmp(tok, "Shift")) return WLR_MODIFIER_SHIFT;
	if (!strcasecmp(tok, "Ctrl") || !strcasecmp(tok, "Control")) return WLR_MODIFIER_CTRL;
	if (!strcasecmp(tok, "Alt") || !strcasecmp(tok, "Mod1")) return WLR_MODIFIER_ALT;
	if (!strcasecmp(tok, "Caps")) return WLR_MODIFIER_CAPS;
	if (!strcasecmp(tok, "Mod2")) return WLR_MODIFIER_MOD2;
	if (!strcasecmp(tok, "Mod3")) return WLR_MODIFIER_MOD3;
	if (!strcasecmp(tok, "Mod5")) return WLR_MODIFIER_MOD5;
	return 0;
}

static unsigned int
parse_modkey_name(const char *s)
{
	if (!s) return WLR_MODIFIER_LOGO;
	if (!strcasecmp(s, "Super") || !strcasecmp(s, "Logo") || !strcasecmp(s, "Win"))
		return WLR_MODIFIER_LOGO;
	if (!strcasecmp(s, "Alt") || !strcasecmp(s, "Mod1")) return WLR_MODIFIER_ALT;
	if (!strcasecmp(s, "Ctrl") || !strcasecmp(s, "Control")) return WLR_MODIFIER_CTRL;
	if (!strcasecmp(s, "Shift")) return WLR_MODIFIER_SHIFT;
	return WLR_MODIFIER_LOGO;
}

/* Parses "Mod+Shift+H" / "Ctrl+Alt+F1" → modmask + keysym. */
static int
parse_keybind(const char *spec, uint32_t *out_mod, xkb_keysym_t *out_sym)
{
	if (!spec || !*spec) return 0;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", spec);
	uint32_t mods = 0;
	char *tok = strtok(buf, "+");
	char *prev = NULL;
	while (tok) {
		if (prev) {
			uint32_t m = mod_from_token(prev);
			if (!m) return 0;
			mods |= m;
		}
		prev = tok;
		tok = strtok(NULL, "+");
	}
	if (!prev) return 0;
	xkb_keysym_t sym = config_parse_keysym(prev);
	if (sym == XKB_KEY_NoSymbol) return 0;
	*out_mod = mods;
	*out_sym = sym;
	return 1;
}

/* ── transform parsing ────────────────────────────────────────────── */

static enum wl_output_transform
parse_transform(const char *s)
{
	if (!s) return WL_OUTPUT_TRANSFORM_NORMAL;
	if (!strcmp(s, "normal") || !strcmp(s, "0"))   return WL_OUTPUT_TRANSFORM_NORMAL;
	if (!strcmp(s, "90"))                          return WL_OUTPUT_TRANSFORM_90;
	if (!strcmp(s, "180"))                         return WL_OUTPUT_TRANSFORM_180;
	if (!strcmp(s, "270"))                         return WL_OUTPUT_TRANSFORM_270;
	if (!strcmp(s, "flipped"))                     return WL_OUTPUT_TRANSFORM_FLIPPED;
	if (!strcmp(s, "flipped-90"))                  return WL_OUTPUT_TRANSFORM_FLIPPED_90;
	if (!strcmp(s, "flipped-180"))                 return WL_OUTPUT_TRANSFORM_FLIPPED_180;
	if (!strcmp(s, "flipped-270"))                 return WL_OUTPUT_TRANSFORM_FLIPPED_270;
	return WL_OUTPUT_TRANSFORM_NORMAL;
}

/* ── enum-style libinput options ──────────────────────────────────── */

static enum libinput_config_scroll_method
parse_scroll_method(const char *s)
{
	if (!s) return LIBINPUT_CONFIG_SCROLL_2FG;
	if (!strcasecmp(s, "no-scroll") || !strcasecmp(s, "none")) return LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
	if (!strcasecmp(s, "2fg") || !strcasecmp(s, "two-finger")) return LIBINPUT_CONFIG_SCROLL_2FG;
	if (!strcasecmp(s, "edge"))                                return LIBINPUT_CONFIG_SCROLL_EDGE;
	if (!strcasecmp(s, "button") || !strcasecmp(s, "on-button-down")) return LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
	return LIBINPUT_CONFIG_SCROLL_2FG;
}

static enum libinput_config_click_method
parse_click_method(const char *s)
{
	if (!s) return LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
	if (!strcasecmp(s, "none"))         return LIBINPUT_CONFIG_CLICK_METHOD_NONE;
	if (!strcasecmp(s, "button-areas")) return LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
	if (!strcasecmp(s, "clickfinger"))  return LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
	return LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
}

static uint32_t
parse_send_events(const char *s)
{
	if (!s) return LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
	if (!strcasecmp(s, "enabled"))                       return LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
	if (!strcasecmp(s, "disabled"))                      return LIBINPUT_CONFIG_SEND_EVENTS_DISABLED;
	if (!strcasecmp(s, "disabled-on-external-mouse"))    return LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE;
	return LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
}

static enum libinput_config_accel_profile
parse_accel_profile(const char *s)
{
	if (!s) return LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
	if (!strcasecmp(s, "flat"))     return LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
	if (!strcasecmp(s, "adaptive")) return LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
	return LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
}

static enum libinput_config_tap_button_map
parse_button_map(const char *s)
{
	if (!s) return LIBINPUT_CONFIG_TAP_MAP_LRM;
	if (!strcasecmp(s, "lmr")) return LIBINPUT_CONFIG_TAP_MAP_LMR;
	return LIBINPUT_CONFIG_TAP_MAP_LRM;
}

/* ── action registry ──────────────────────────────────────────────── */

/* Argument kinds expected by each action. */
typedef enum { A_NONE, A_INT, A_UINT, A_FLOAT, A_DIR, A_TAGN, A_SPAWN } ArgKind;

typedef struct {
	const char *name;
	void (*func)(const Arg *);
	ArgKind kind;
} ActionEntry;

/* Directional helpers — value already accepted in arg.i */
static const ActionEntry actions[] = {
	{ "quit",                         quit,                         A_NONE },
	{ "killclient",                   killclient,                   A_NONE },
	{ "spawn",                        spawn,                        A_SPAWN },
	{ "togglefloating",               togglefloating,               A_NONE },
	{ "togglefullscreen",             togglefullscreen,             A_NONE },
	{ "togglestatusbar",              togglestatusbar,              A_NONE },
	{ "togglegaps",                   togglegaps,                   A_NONE },

	{ "focus-column-dir",             focus_column_dir,             A_INT },
	{ "focus-workspace-dir",          focus_workspace_dir,          A_INT },
	{ "focus-workspace-n",            focus_workspace_n,            A_INT },
	{ "focus-window-in-column-dir",   focus_window_in_column_dir,   A_INT },
	{ "focus-last-workspace",         focus_last_workspace,         A_NONE },

	{ "move-column-dir",              move_column_dir,              A_INT },
	{ "move-window-in-column-dir",    move_window_in_column_dir,    A_INT },
	{ "move-client-to-ws-dir",        move_client_to_ws_dir,        A_INT },
	{ "move-client-to-ws-n",          move_client_to_ws_n,          A_INT },

	{ "swap-window-dir",              swap_window_dir,              A_INT },
	{ "expel-window-from-column",     expel_window_from_column,     A_NONE },
	{ "switch-preset-column-width",   switch_preset_column_width,   A_NONE },
	{ "resize-column-dir",            resize_column_dir,            A_INT },
	{ "maximize-column",              maximize_column,              A_NONE },
	{ "center-column",                center_column,                A_NONE },
	{ "toggle-column-fullscreen",     toggle_column_fullscreen,     A_NONE },

	{ "focusmon",                     focusmon,                     A_DIR },
	{ "tagmon",                       tagmon,                       A_DIR },
	{ "warptomonitor",                warptomonitor,                A_DIR },
	{ "tagtomonitornum",              tagtomonitornum,              A_UINT },

	{ "view",                         view,                         A_TAGN },
	{ "tag",                          tag,                          A_TAGN },
	{ "toggleview",                   toggleview,                   A_TAGN },
	{ "toggletag",                    toggletag,                    A_TAGN },

	{ "setlayout",                    setlayout,                    A_NONE },
	{ "chvt",                         chvt,                         A_UINT },

	{ NULL, NULL, A_NONE },
};

static const ActionEntry *
find_action(const char *name)
{
	for (const ActionEntry *e = actions; e->name; e++)
		if (!strcasecmp(e->name, name)) return e;
	return NULL;
}

static unsigned int
parse_direction(const char *s, long fallback)
{
	if (!s) return (unsigned int)fallback;
	if (!strcasecmp(s, "left"))  return WLR_DIRECTION_LEFT;
	if (!strcasecmp(s, "right")) return WLR_DIRECTION_RIGHT;
	if (!strcasecmp(s, "up"))    return WLR_DIRECTION_UP;
	if (!strcasecmp(s, "down"))  return WLR_DIRECTION_DOWN;
	return (unsigned int)fallback;
}

/* ── runtime state shared with the rest of the compositor ─────────── */

/* Defined in globals.c. */
extern Rule           *runtime_rules;
extern size_t          runtime_rules_count;
extern struct xkb_rule_names runtime_xkb_rules;
extern int             runtime_xkb_rules_set;

extern char          **runtime_autostart;
extern size_t          runtime_autostart_count;
extern pid_t          *runtime_autostart_pids;

/* ── tilde expansion ──────────────────────────────────────────────── */

static char *
expand_path(const char *p)
{
	if (!p) return NULL;
	const char *home = getenv("HOME");
	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw) home = pw->pw_dir;
	}
	if (!home) home = "/";

	char out[PATH_MAX];
	if (p[0] == '~' && (p[1] == '/' || p[1] == '\0'))
		snprintf(out, sizeof(out), "%s%s", home, p + 1);
	else if (!strncmp(p, "$HOME", 5))
		snprintf(out, sizeof(out), "%s%s", home, p + 5);
	else
		snprintf(out, sizeof(out), "%s", p);
	return strdup(out);
}

/* ── apply functions ──────────────────────────────────────────────── */

static void
apply_appearance(const KdlNode *n)
{
	if (!n) return;
	long li;
	int b;
	double f;
	const char *s;

	if (kdl_prop_int(n, "gaps", &li) || kdl_arg_int(kdl_find_child(n, "gaps"), 0, &li))
		gappx = (unsigned int)li;
	if (kdl_arg_int(kdl_find_child(n, "border-px"), 0, &li))
		borderpx = (unsigned int)li;
	if (kdl_arg_bool(kdl_find_child(n, "smartgaps"), 0, &b))
		smartgaps = b;
	if (kdl_arg_bool(kdl_find_child(n, "sloppy-focus"), 0, &b))
		sloppyfocus = b;
	if (kdl_arg_bool(kdl_find_child(n, "bypass-surface-visibility"), 0, &b))
		bypass_surface_visibility = b;
	if (kdl_arg_bool(kdl_find_child(n, "gaps-enabled"), 0, &b))
		gaps = b;
	if (kdl_arg_string(kdl_find_child(n, "root-color"), 0, &s))
		parse_color(s, rootcolor);
	if (kdl_arg_string(kdl_find_child(n, "border-color"), 0, &s))
		parse_color(s, bordercolor);
	if (kdl_arg_string(kdl_find_child(n, "focus-color"), 0, &s))
		parse_color(s, focuscolor);
	if (kdl_arg_string(kdl_find_child(n, "urgent-color"), 0, &s))
		parse_color(s, urgentcolor);
	if (kdl_arg_string(kdl_find_child(n, "fullscreen-bg"), 0, &s))
		parse_color(s, fullscreen_bg);
	if (kdl_arg_float(kdl_find_child(n, "resize-factor"), 0, &f))
		resize_factor = (float)f;
	if (kdl_arg_int(kdl_find_child(n, "resize-interval-ms"), 0, &li))
		resize_interval_ms = (uint32_t)li;
	if (kdl_arg_float(kdl_find_child(n, "resize-min-pixels"), 0, &f))
		resize_min_pixels = f;
	if (kdl_arg_float(kdl_find_child(n, "resize-ratio-epsilon"), 0, &f))
		resize_ratio_epsilon = (float)f;
	if (kdl_arg_bool(kdl_find_child(n, "lock-cursor"), 0, &b))
		lock_cursor = b;
}

/* Replace xkb_rules' string fields safely (own copies, free old). */
static char *runtime_xkb_layout = NULL;
static char *runtime_xkb_model = NULL;
static char *runtime_xkb_variant = NULL;
static char *runtime_xkb_options = NULL;
static char *runtime_xkb_rules_field = NULL;

static void
replace_str(char **slot, const char *new_val)
{
	free(*slot);
	*slot = new_val ? strdup(new_val) : NULL;
}

static void
apply_input(const KdlNode *n)
{
	if (!n) return;
	long li;
	int b;
	double f;
	const char *s;

	const KdlNode *kbd = kdl_find_child(n, "keyboard");
	if (kbd) {
		struct xkb_rule_names *r = &runtime_xkb_rules;
		if (kdl_arg_string(kdl_find_child(kbd, "layout"), 0, &s))
			{ replace_str(&runtime_xkb_layout, s);  r->layout  = runtime_xkb_layout; }
		if (kdl_arg_string(kdl_find_child(kbd, "model"), 0, &s))
			{ replace_str(&runtime_xkb_model, s);   r->model   = runtime_xkb_model; }
		if (kdl_arg_string(kdl_find_child(kbd, "variant"), 0, &s))
			{ replace_str(&runtime_xkb_variant, s); r->variant = runtime_xkb_variant; }
		if (kdl_arg_string(kdl_find_child(kbd, "options"), 0, &s))
			{ replace_str(&runtime_xkb_options, s); r->options = runtime_xkb_options; }
		if (kdl_arg_string(kdl_find_child(kbd, "rules"), 0, &s))
			{ replace_str(&runtime_xkb_rules_field, s); r->rules = runtime_xkb_rules_field; }
		runtime_xkb_rules_set = 1;

		if (kdl_arg_int(kdl_find_child(kbd, "repeat-delay"), 0, &li))
			repeat_delay = (int)li;
		if (kdl_arg_int(kdl_find_child(kbd, "repeat-rate"), 0, &li))
			repeat_rate = (int)li;
	}

	const KdlNode *tp = kdl_find_child(n, "touchpad");
	if (!tp) tp = kdl_find_child(n, "pointer");
	if (tp) {
		if (kdl_arg_bool(kdl_find_child(tp, "tap"), 0, &b)) tap_to_click = b;
		if (kdl_arg_bool(kdl_find_child(tp, "tap-and-drag"), 0, &b)) tap_and_drag = b;
		if (kdl_arg_bool(kdl_find_child(tp, "drag-lock"), 0, &b)) drag_lock = b;
		if (kdl_arg_bool(kdl_find_child(tp, "natural-scroll"), 0, &b)) natural_scrolling = b;
		if (kdl_arg_bool(kdl_find_child(tp, "disable-while-typing"), 0, &b)) disable_while_typing = b;
		if (kdl_arg_bool(kdl_find_child(tp, "left-handed"), 0, &b)) left_handed = b;
		if (kdl_arg_bool(kdl_find_child(tp, "middle-button-emulation"), 0, &b)) middle_button_emulation = b;
		if (kdl_arg_string(kdl_find_child(tp, "scroll-method"), 0, &s)) scroll_method = parse_scroll_method(s);
		if (kdl_arg_string(kdl_find_child(tp, "click-method"), 0, &s)) click_method = parse_click_method(s);
		if (kdl_arg_string(kdl_find_child(tp, "send-events"), 0, &s)) send_events_mode = parse_send_events(s);
		if (kdl_arg_string(kdl_find_child(tp, "accel-profile"), 0, &s)) accel_profile = parse_accel_profile(s);
		if (kdl_arg_float(kdl_find_child(tp, "accel-speed"), 0, &f)) accel_speed = f;
		if (kdl_arg_string(kdl_find_child(tp, "button-map"), 0, &s)) button_map = parse_button_map(s);
	}
}

static void
apply_monitor(const KdlNode *n)
{
	const char *name;
	if (!kdl_arg_string(n, 0, &name)) return;
	if (runtime_monitor_count >= MAX_MONITORS) return;
	RuntimeMonitorConfig *m = &runtime_monitors[runtime_monitor_count++];
	memset(m, 0, sizeof(*m));
	snprintf(m->name, sizeof(m->name), "%s", name);
	m->enabled = 1;
	m->scale = 1.0f;
	m->mfact = 0.55f;
	m->nmaster = 1;
	m->transform = WL_OUTPUT_TRANSFORM_NORMAL;
	m->position = MON_POS_AUTO;
	m->grid_col = -1;
	m->grid_row = -1;

	int b;
	long li;
	double f;
	const char *s;

	if (kdl_arg_bool(kdl_find_child(n, "enabled"), 0, &b)) m->enabled = b;
	if (kdl_arg_float(kdl_find_child(n, "scale"), 0, &f)) m->scale = (float)f;
	if (kdl_arg_float(kdl_find_child(n, "mfact"), 0, &f)) m->mfact = (float)f;
	if (kdl_arg_int(kdl_find_child(n, "nmaster"), 0, &li)) m->nmaster = (int)li;
	if (kdl_arg_string(kdl_find_child(n, "transform"), 0, &s)) m->transform = parse_transform(s);
	if (kdl_arg_string(kdl_find_child(n, "position"), 0, &s))
		m->position = config_parse_monitor_position(s);

	/* mode "WxH@Hz" or props { width=... height=... refresh=... } */
	if (kdl_arg_string(kdl_find_child(n, "mode"), 0, &s)) {
		int w = 0, h = 0;
		float hz = 0;
		if (sscanf(s, "%dx%d@%f", &w, &h, &hz) >= 2) {
			m->width = w; m->height = h; m->refresh = hz;
		}
	}
	const KdlNode *res = kdl_find_child(n, "resolution");
	if (res) {
		if (kdl_arg_int(res, 0, &li)) m->width = (int)li;
		if (kdl_arg_int(res, 1, &li)) m->height = (int)li;
	}
	if (kdl_arg_float(kdl_find_child(n, "refresh"), 0, &f)) m->refresh = (float)f;
	const KdlNode *grid = kdl_find_child(n, "grid");
	if (grid) {
		if (kdl_arg_int(grid, 0, &li)) m->grid_col = (int)li;
		if (kdl_arg_int(grid, 1, &li)) m->grid_row = (int)li;
	}
}

static void
apply_window_rule(const KdlNode *n)
{
	const char *s;
	long li;
	int b;

	runtime_rules = realloc(runtime_rules, (runtime_rules_count + 1) * sizeof(Rule));
	Rule *r = &runtime_rules[runtime_rules_count++];
	memset(r, 0, sizeof(*r));
	r->monitor = -1;

	if (kdl_arg_string(kdl_find_child(n, "app-id"), 0, &s)) r->id = strdup(s);
	if (kdl_arg_string(kdl_find_child(n, "title"), 0, &s))  r->title = strdup(s);
	if (kdl_arg_int(kdl_find_child(n, "tags"), 0, &li))     r->tags = (uint32_t)li;
	if (kdl_arg_bool(kdl_find_child(n, "floating"), 0, &b)) r->isfloating = b;
	if (kdl_arg_int(kdl_find_child(n, "monitor"), 0, &li))  r->monitor = (int)li;
}

static void
free_runtime_rules(void)
{
	for (size_t i = 0; i < runtime_rules_count; i++) {
		free((char *)runtime_rules[i].id);
		free((char *)runtime_rules[i].title);
	}
	free(runtime_rules);
	runtime_rules = NULL;
	runtime_rules_count = 0;
}

static void
free_runtime_spawn_cmds(void)
{
	for (int i = 0; i < runtime_spawn_cmd_count; i++)
		free(runtime_spawn_cmds[i]);
	runtime_spawn_cmd_count = 0;
}

/* Add a runtime spawn command and return the const char*[] vector needed
 * by spawn().  Vector is heap-allocated (kept alive in runtime_spawn_cmds). */
static const char **
make_spawn_vec(const char *cmd)
{
	if (runtime_spawn_cmd_count + 2 >= MAX_KEYS) return NULL;
	/* Layout in runtime_spawn_cmds: vec_ptr (cast char*), then "/bin/sh", "-c", cmd-copy.
	 * Simpler: store the heap vector in one slot, and the cmd-copy in next. */
	char *cmd_copy = strdup(cmd);
	if (!cmd_copy) return NULL;
	const char **vec = calloc(4, sizeof(*vec));
	if (!vec) { free(cmd_copy); return NULL; }
	vec[0] = "/bin/sh";
	vec[1] = "-c";
	vec[2] = cmd_copy;
	vec[3] = NULL;
	runtime_spawn_cmds[runtime_spawn_cmd_count++] = cmd_copy;
	runtime_spawn_cmds[runtime_spawn_cmd_count++] = (char *)vec;
	return vec;
}

static void
apply_bind(const KdlNode *n)
{
	const char *keystr, *action;
	if (!kdl_arg_string(n, 0, &keystr)) return;
	if (!kdl_arg_string(n, 1, &action)) return;
	uint32_t mod;
	xkb_keysym_t sym;
	if (!parse_keybind(keystr, &mod, &sym)) {
		fprintf(stderr, "nixlytile: bad bind '%s' (line %d)\n", keystr, n->line);
		return;
	}
	const ActionEntry *act = find_action(action);
	if (!act) {
		fprintf(stderr, "nixlytile: unknown action '%s' (line %d)\n", action, n->line);
		return;
	}
	if (runtime_keys_count >= MAX_KEYS) {
		fprintf(stderr, "nixlytile: too many bindings (max %d)\n", MAX_KEYS);
		return;
	}
	Key *k = &runtime_keys[runtime_keys_count];
	memset(k, 0, sizeof(*k));
	k->mod = mod;
	k->keysym = sym;
	k->func = act->func;

	switch (act->kind) {
	case A_NONE: break;
	case A_INT: {
		long li = 0; kdl_arg_int(n, 2, &li);
		Arg a = { .i = (int)li };
		memcpy((void *)&k->arg, &a, sizeof(a));
		break;
	}
	case A_UINT: {
		long li = 0; kdl_arg_int(n, 2, &li);
		Arg a = { .ui = (unsigned int)li };
		memcpy((void *)&k->arg, &a, sizeof(a));
		break;
	}
	case A_FLOAT: {
		double d = 0; kdl_arg_float(n, 2, &d);
		Arg a = { .f = (float)d };
		memcpy((void *)&k->arg, &a, sizeof(a));
		break;
	}
	case A_DIR: {
		const char *ds = NULL; kdl_arg_string(n, 2, &ds);
		long li = 0; kdl_arg_int(n, 2, &li);
		Arg a = { .i = (int)parse_direction(ds, li) };
		memcpy((void *)&k->arg, &a, sizeof(a));
		break;
	}
	case A_TAGN: {
		long li = 0; kdl_arg_int(n, 2, &li);
		Arg a = { .ui = (unsigned int)(1u << li) };
		memcpy((void *)&k->arg, &a, sizeof(a));
		break;
	}
	case A_SPAWN: {
		const char *cmd = NULL; kdl_arg_string(n, 2, &cmd);
		if (!cmd) return;
		const char **vec = make_spawn_vec(cmd);
		if (!vec) return;
		Arg a = { .v = vec };
		memcpy((void *)&k->arg, &a, sizeof(a));
		break;
	}
	}
	runtime_keys_count++;
}

static void
apply_autostart(const KdlNode *n, char **collected, size_t *pn)
{
	const char *cmd;
	if (kdl_arg_string(n, 0, &cmd)) {
		collected[*pn] = strdup(cmd);
		(*pn)++;
	}
}

/* ── hot-apply implementations (no-ops when called before setup) ──── */

static void
hotapply_libinput(void)
{
	for (int i = 0; i < pointer_device_count; i++) {
		struct libinput_device *d = pointer_devices[i];
		if (!d) continue;
		if (libinput_device_config_tap_get_finger_count(d)) {
			libinput_device_config_tap_set_enabled(d, tap_to_click);
			libinput_device_config_tap_set_drag_enabled(d, tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(d, drag_lock);
			libinput_device_config_tap_set_button_map(d, button_map);
		}
		if (libinput_device_config_scroll_has_natural_scroll(d))
			libinput_device_config_scroll_set_natural_scroll_enabled(d, natural_scrolling);
		if (libinput_device_config_dwt_is_available(d))
			libinput_device_config_dwt_set_enabled(d, disable_while_typing);
		if (libinput_device_config_left_handed_is_available(d))
			libinput_device_config_left_handed_set(d, left_handed);
		if (libinput_device_config_middle_emulation_is_available(d))
			libinput_device_config_middle_emulation_set_enabled(d, middle_button_emulation);
		if (libinput_device_config_scroll_get_methods(d) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method(d, scroll_method);
		if (libinput_device_config_click_get_methods(d) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
			libinput_device_config_click_set_method(d, click_method);
		if (libinput_device_config_send_events_get_modes(d))
			libinput_device_config_send_events_set_mode(d, send_events_mode);
		if (libinput_device_config_accel_is_available(d)) {
			libinput_device_config_accel_set_profile(d, accel_profile);
			libinput_device_config_accel_set_speed(d, accel_speed);
		}
	}
}

static void
hotapply_keyboard(void)
{
	if (!kb_group || !kb_group->wlr_group) return;
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!ctx) return;
	struct xkb_rule_names names = getxkbrules();
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (km) {
		wlr_keyboard_set_keymap(&kb_group->wlr_group->keyboard, km);
		xkb_keymap_unref(km);
	}
	xkb_context_unref(ctx);
	wlr_keyboard_set_repeat_info(&kb_group->wlr_group->keyboard,
		repeat_rate, repeat_delay);
}

static void
hotapply_monitors(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output) continue;
		RuntimeMonitorConfig *cfg = find_monitor_config(m->wlr_output->name);
		struct wlr_output_state st;
		wlr_output_state_init(&st);
		if (cfg) {
			if (!cfg->enabled) {
				wlr_output_state_set_enabled(&st, 0);
				wlr_output_commit_state(m->wlr_output, &st);
				wlr_output_state_finish(&st);
				continue;
			}
			wlr_output_state_set_enabled(&st, 1);
			wlr_output_state_set_scale(&st, cfg->scale);
			wlr_output_state_set_transform(&st, cfg->transform);
			if (cfg->width > 0 && cfg->height > 0) {
				struct wlr_output_mode *mode = find_mode(m->wlr_output,
					cfg->width, cfg->height, cfg->refresh);
				if (mode) wlr_output_state_set_mode(&st, mode);
			} else {
				struct wlr_output_mode *mode = bestmode(m->wlr_output);
				if (mode) wlr_output_state_set_mode(&st, mode);
			}
			m->mfact = cfg->mfact;
			m->nmaster = cfg->nmaster;
		} else {
			/* Undefined monitor: auto highest mode + default scale. */
			wlr_output_state_set_enabled(&st, 1);
			wlr_output_state_set_scale(&st, 1.0f);
			struct wlr_output_mode *mode = bestmode(m->wlr_output);
			if (mode) wlr_output_state_set_mode(&st, mode);
		}
		if (wlr_output_test_state(m->wlr_output, &st))
			wlr_output_commit_state(m->wlr_output, &st);
		wlr_output_state_finish(&st);
	}
	/* Trigger relayout. */
	wl_signal_emit_mutable(&output_layout->events.change, output_layout);
}

static void
hotapply_appearance(void)
{
	if (root_bg) {
		wlr_scene_rect_set_color(root_bg, rootcolor);
	}
	Client *c;
	wl_list_for_each(c, &clients, link) {
		for (int i = 0; i < 4; i++) {
			if (c->border[i])
				wlr_scene_rect_set_color(c->border[i],
					c == focustop(selmon) ? focuscolor : bordercolor);
		}
	}
	arrange(selmon);
}

/* Autostart: diff old vs new lists.  Identical commands keep running;
 * removed commands are SIGTERM'd via pid table; new commands are spawned. */
static void
apply_autostart_diff(char **new_cmds, size_t new_count, int initial)
{
	if (initial) {
		runtime_autostart = calloc(new_count, sizeof(char *));
		runtime_autostart_pids = calloc(new_count, sizeof(pid_t));
		runtime_autostart_count = new_count;
		for (size_t i = 0; i < new_count; i++) {
			runtime_autostart[i] = new_cmds[i];
			runtime_autostart_pids[i] = -1; /* spawned by run() */
		}
		return;
	}

	/* Mark kept entries. */
	int *keep_old = calloc(runtime_autostart_count, sizeof(int));
	int *keep_new = calloc(new_count, sizeof(int));
	for (size_t i = 0; i < runtime_autostart_count; i++)
		for (size_t j = 0; j < new_count; j++)
			if (!keep_new[j] && !strcmp(runtime_autostart[i], new_cmds[j])) {
				keep_old[i] = 1;
				keep_new[j] = 1;
				break;
			}

	/* Kill removed. */
	for (size_t i = 0; i < runtime_autostart_count; i++) {
		if (!keep_old[i] && runtime_autostart_pids[i] > 0)
			kill(runtime_autostart_pids[i], SIGTERM);
		free(runtime_autostart[i]);
	}
	free(runtime_autostart);
	free(runtime_autostart_pids);

	runtime_autostart = calloc(new_count, sizeof(char *));
	runtime_autostart_pids = calloc(new_count, sizeof(pid_t));
	runtime_autostart_count = new_count;

	for (size_t j = 0; j < new_count; j++) {
		runtime_autostart[j] = new_cmds[j];
		runtime_autostart_pids[j] = -1;
		if (keep_new[j]) continue;
		/* Spawn newly added command via shell. */
		pid_t pid = fork();
		if (pid == 0) {
			setsid();
			execl("/bin/sh", "/bin/sh", "-c", new_cmds[j], (char *)NULL);
			_exit(127);
		} else if (pid > 0) {
			runtime_autostart_pids[j] = pid;
		}
	}
	free(keep_old);
	free(keep_new);
}

/* ── file loader ──────────────────────────────────────────────────── */

static char *
read_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	if (n < 0) { fclose(fp); return NULL; }
	rewind(fp);
	char *buf = malloc((size_t)n + 1);
	if (!buf) { fclose(fp); return NULL; }
	size_t got = fread(buf, 1, (size_t)n, fp);
	buf[got] = '\0';
	fclose(fp);
	return buf;
}

static void
resolve_config_path(char *out, size_t cap)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) {
		snprintf(out, cap, "%s/nixlytile/config.kdl", xdg);
		return;
	}
	const char *home = getenv("HOME");
	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw) home = pw->pw_dir;
	}
	if (!home) home = "/";
	snprintf(out, cap, "%s/.config/nixlytile/config.kdl", home);
}

static int
apply_doc(const KdlDoc *doc, int initial)
{
	/* Reset per-reload state. */
	runtime_keys_count = 0;
	runtime_monitor_count = 0;
	free_runtime_rules();
	free_runtime_spawn_cmds();
	runtime_xkb_rules_set = 0;
	memset(&runtime_xkb_rules, 0, sizeof(runtime_xkb_rules));

	char **autostart_cmds = NULL;
	size_t autostart_n = 0;
	size_t autostart_cap = 0;

	for (size_t i = 0; i < doc->n_roots; i++) {
		const KdlNode *n = &doc->roots[i];
		const char *s;
		long li;

		if (!strcmp(n->name, "appearance"))         apply_appearance(n);
		else if (!strcmp(n->name, "input"))         apply_input(n);
		else if (!strcmp(n->name, "monitor"))       apply_monitor(n);
		else if (!strcmp(n->name, "window-rule"))   apply_window_rule(n);
		else if (!strcmp(n->name, "bind"))          apply_bind(n);
		else if (!strcmp(n->name, "modkey")) {
			if (kdl_arg_string(n, 0, &s)) modkey = parse_modkey_name(s);
		} else if (!strcmp(n->name, "monitorkey")) {
			if (kdl_arg_string(n, 0, &s)) monitorkey = parse_modkey_name(s);
		} else if (!strcmp(n->name, "wallpaper")) {
			if (kdl_arg_string(n, 0, &s)) {
				char *exp = expand_path(s);
				if (exp) {
					snprintf(wallpaper_path, sizeof(wallpaper_path), "%s", exp);
					free(exp);
				}
			}
		} else if (!strcmp(n->name, "autostart")) {
			if (autostart_n == autostart_cap) {
				autostart_cap = autostart_cap ? autostart_cap * 2 : 8;
				autostart_cmds = realloc(autostart_cmds, autostart_cap * sizeof(char *));
			}
			apply_autostart(n, autostart_cmds, &autostart_n);
		} else if (!strcmp(n->name, "log-level")) {
			if (kdl_arg_string(n, 0, &s)) {
				if (!strcasecmp(s, "error"))       log_level = WLR_ERROR;
				else if (!strcasecmp(s, "info"))   log_level = WLR_INFO;
				else if (!strcasecmp(s, "debug"))  log_level = WLR_DEBUG;
			}
		} else if (!strcmp(n->name, "lock-cursor")) {
			int b; if (kdl_arg_bool(n, 0, &b)) lock_cursor = b;
		} else if (!strcmp(n->name, "workspaces")) {
			(void)li; /* TAGCOUNT is compile-time; informational only */
		}
	}

	/* Use runtime keybindings if any were defined. */
	if (runtime_keys_count > 0) {
		keys = runtime_keys;
		keys_count = runtime_keys_count;
	} else {
		keys = default_keys;
		keys_count = default_keys_count;
	}

	apply_autostart_diff(autostart_cmds, autostart_n, initial);
	free(autostart_cmds);
	return 1;
}

int
load_config(void)
{
	resolve_config_path(nixlytile_config_path, sizeof(nixlytile_config_path));
	char *text = read_file(nixlytile_config_path);
	if (!text) {
		fprintf(stderr, "nixlytile: no config at %s (using defaults)\n",
			nixlytile_config_path);
		return 0;
	}
	KdlDoc doc = kdl_parse(text);
	free(text);
	if (doc.err) {
		fprintf(stderr, "nixlytile: config parse error: %s\n", doc.err);
		kdl_doc_free(&doc);
		return 0;
	}
	apply_doc(&doc, /*initial=*/1);
	kdl_doc_free(&doc);
	runtime_config_loaded = 1;
	fprintf(stderr, "nixlytile: loaded %s\n", nixlytile_config_path);
	return 1;
}

int
reload_config(void)
{
	if (!nixlytile_config_path[0])
		resolve_config_path(nixlytile_config_path, sizeof(nixlytile_config_path));
	char *text = read_file(nixlytile_config_path);
	if (!text) {
		fprintf(stderr, "nixlytile: reload failed, no config at %s\n",
			nixlytile_config_path);
		return 0;
	}
	KdlDoc doc = kdl_parse(text);
	free(text);
	if (doc.err) {
		fprintf(stderr, "nixlytile: reload parse error: %s\n", doc.err);
		kdl_doc_free(&doc);
		return 0;
	}
	apply_doc(&doc, /*initial=*/0);
	kdl_doc_free(&doc);

	hotapply_keyboard();
	hotapply_libinput();
	hotapply_monitors();
	hotapply_appearance();

	fprintf(stderr, "nixlytile: reloaded %s\n", nixlytile_config_path);
	return 1;
}

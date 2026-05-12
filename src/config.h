/* Taken from https://github.com/djpohly/dwl/issues/466 */
#define COLOR(hex)    { ((hex >> 24) & 0xFF) / 255.0f, \
                        ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }

/* Runtime configurable variables - these can be overridden by config file */
/* appearance */
int sloppyfocus               = 1;  /* focus follows mouse */
int bypass_surface_visibility = 0;  /* 1 means idle inhibitors will disable idle tracking even if it's surface isn't visible  */
int smartgaps                 = 0;  /* 1 means no outer gap when there is only one window */
int gaps                      = 1;  /* 1 means gaps between windows are added */
unsigned int gappx            = 5;  /* gap pixel between windows */
unsigned int borderpx         = 1;  /* border pixel of windows */
float rootcolor[]             = COLOR(0x222222ff);
float bordercolor[]           = COLOR(0x444444ff);
float focuscolor[]            = COLOR(0x005577ff);
float urgentcolor[]           = COLOR(0xff0000ff);
unsigned int statusbar_height = 26;
unsigned int statusbar_module_spacing = 10;
unsigned int statusbar_module_padding = 8;
unsigned int statusbar_icon_text_gap = 6; /* gap between icon and text */
unsigned int statusbar_icon_text_gap_volume = 10;
unsigned int statusbar_icon_text_gap_microphone = 8;
unsigned int statusbar_icon_text_gap_cpu = 6;
unsigned int statusbar_icon_text_gap_ram = 6;
unsigned int statusbar_icon_text_gap_light = 6;
unsigned int statusbar_icon_text_gap_battery = 2; /* slightly tighter */
unsigned int statusbar_icon_text_gap_clock = 6;
unsigned int statusbar_top_gap = 3;
float statusbar_fg[]          = COLOR(0xffffffff);
float statusbar_bg[]          = COLOR(0x00000016);
float statusbar_popup_bg[]    = COLOR(0x00000080); /* ~50% */
float statusbar_volume_muted_fg[] = COLOR(0xff4c4cff);
float statusbar_mic_muted_fg[] = COLOR(0xff4c4cff);
float statusbar_tag_bg[]      = COLOR(0x00000033);
float statusbar_tag_active_bg[] = COLOR(0x1565c0ff);
float statusbar_tag_hover_bg[] = COLOR(0x66b3ffaa);
int statusbar_hover_fade_ms   = 0;
int statusbar_tray_force_rgba = 0; /* 1: force RGBA decode for all tray icons (last-resort for apps that send RGBA) */
const char *statusbar_fonts[8] = {
	"monospace:size=16:weight=Bold",
	"monospace:size=16",
	NULL, NULL, NULL, NULL, NULL, NULL
};
const char *statusbar_font_attributes = NULL;
int statusbar_font_spacing = 0;
int statusbar_font_force_color = 1;
enum fcft_subpixel statusbar_font_subpixel = FCFT_SUBPIXEL_DEFAULT;
int statusbar_workspace_padding = 8;
int statusbar_workspace_spacing = 4;
int statusbar_thumb_height = 40;
int statusbar_thumb_gap = 2;
float statusbar_thumb_window[] = COLOR(0xffffff55);
/* This conforms to the xdg-protocol. Set the alpha to zero to restore the old behavior */
float fullscreen_bg[]         = {0.1f, 0.1f, 0.1f, 1.0f}; /* You can also use glsl colors */
float resize_factor           = 0.0002f; /* Resize multiplier for mouse resizing, depends on mouse sensivity. */
uint32_t resize_interval_ms   = 8; /* Min interval between mouse-driven resize updates (ms). */
double   resize_min_pixels    = 1.0; /* Min pointer movement (px) before a new resize if within interval. */
float    resize_ratio_epsilon = 0.001f; /* Smallest ratio change that should trigger an arrange. */
int      modal_file_search_minlen = 1; /* Min chars before starting a file search */

/* window resizing */
int lock_cursor = 0;	/* 1: lock cursor, 0: don't lock */

/* tagging - TAGCOUNT must be no greater than 31 */
#define TAGCOUNT (9)

/* logging */
int log_level = WLR_DEBUG;

/* nixlytile mode */
int nixlytile_mode = 1;

/* NOTE: ALWAYS keep a rule declared even if you don't use rules (e.g leave at least one example) */
const Rule rules[] = {
	/* app_id             title       tags mask     isfloating   monitor */
	/* examples: */
	{ "Gimp_EXAMPLE",     NULL,       0,            1,           -1 }, /* Start on currently visible tags floating, not tiled */
	{ "firefox_EXAMPLE",  NULL,       1 << 8,       0,           -1 }, /* Start on ONLY tag "9" */
};

/* layout(s) */
const Layout layouts[] = {
	/* symbol     arrange function */
	{ "[]=",      tile },
	{ "><>",      NULL },    /* no layout function means floating behavior */
	{ "[M]",      monocle },
};

/* monitors */
/* (x=-1, y=-1) is reserved as an "autoconfigure" monitor position indicator
 * WARNING: negative values other than (-1, -1) cause problems with Xwayland clients
 * https://gitlab.freedesktop.org/xorg/xserver/-/issues/899
*/
/* NOTE: ALWAYS add a fallback rule, even if you are completely sure it won't be used */
const MonitorRule monrules[] = {
	/* name       mfact  nmaster scale layout       rotate/reflect                x    y */
	/* example of a HiDPI laptop monitor:
	{ "eDP-1",    0.5f,  1,      2,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1 },
	*/
	/* defaults */
	{ NULL,       0.55f, 1,      1,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1 },
};

/* keyboard */
const struct xkb_rule_names xkb_rules = {
	/* can specify fields: rules, model, layout, variant, options */
	/* example:
	.options = "ctrl:nocaps",
	*/
	.options = NULL,
};

int repeat_delay = 250;
int repeat_rate = 60;

/* Trackpad */
int tap_to_click = 1;
int tap_and_drag = 1;
int drag_lock = 1;
int natural_scrolling = 0;
int disable_while_typing = 1;
int left_handed = 0;
int middle_button_emulation = 0;
/* You can choose between:
LIBINPUT_CONFIG_SCROLL_NO_SCROLL
LIBINPUT_CONFIG_SCROLL_2FG
LIBINPUT_CONFIG_SCROLL_EDGE
LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN
*/
enum libinput_config_scroll_method scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;

/* You can choose between:
LIBINPUT_CONFIG_CLICK_METHOD_NONE
LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS
LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
*/
enum libinput_config_click_method click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;

/* You can choose between:
LIBINPUT_CONFIG_SEND_EVENTS_ENABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE
*/
uint32_t send_events_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;

/* You can choose between:
LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE
*/
enum libinput_config_accel_profile accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
double accel_speed = 0.0;

/* You can choose between:
LIBINPUT_CONFIG_TAP_MAP_LRM -- 1/2/3 finger tap maps to left/right/middle
LIBINPUT_CONFIG_TAP_MAP_LMR -- 1/2/3 finger tap maps to left/middle/right
*/
enum libinput_config_tap_button_map button_map = LIBINPUT_CONFIG_TAP_MAP_LRM;

/* If you want to use the windows key for MODKEY, use WLR_MODIFIER_LOGO */
/* Default modifier keys - used in compile-time key definitions */
#define MODKEY WLR_MODIFIER_LOGO
#define MONITORKEY WLR_MODIFIER_CTRL

/* Runtime modifier keys - can be overridden by config file */
unsigned int modkey = WLR_MODIFIER_LOGO;
unsigned int monitorkey = WLR_MODIFIER_CTRL;

#define TAGKEYS(KEY,SKEY,TAG) \
	{ MODKEY,                    KEY,            view,            {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL,  KEY,            toggleview,      {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_SHIFT, KEY,            tag,             {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,KEY,toggletag,  {.ui = 1 << TAG} }

/* Move focused tile to monitor by number (0-indexed) */
#define MONITORKEYS(KEY,MONNUM) \
	{ MONITORKEY|WLR_MODIFIER_SHIFT, KEY, tagtomonitornum, {.ui = MONNUM} }

/* helper for spawning shell commands in the pre dwm-5.0 fashion */
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

/* commands - can be overridden by config file */
const char *termcmd[] = { "foot", NULL };
const char *alacrittycmd[] = { "alacritty", NULL };
const char *btopcmd[] = { "alacritty", "-e", "btop", NULL };
/* Chromium-family browsers: force native Wayland (Ozone) instead of
 * XWayland.  XWayland-managed Chrome triggers a buffer rebuild + jumpy
 * geometry update every time the compositor commits a new configure
 * (tile resize, neighbour move, workspace switch) because XWayland
 * proxies all configures through XSetWMNormalHints and Chrome reflows.
 * Native Wayland surfaces accept configures directly and only reflow
 * when their own renderer is ready. */
const char *bravecmd[] __attribute__((unused)) = {
	"brave",
	"--ozone-platform-hint=auto",
	"--enable-features=UseOzonePlatform,WaylandWindowDecorations",
	NULL
};
const char *chromecmd[] = {
	"google-chrome-stable",
	"--ozone-platform-hint=auto",
	"--enable-features=UseOzonePlatform,WaylandWindowDecorations",
	NULL
};
const char *nixlylaunchercmd[] = { "apptoggle", NULL };
const char *menucmd[] __attribute__((unused)) = { "wmenu-run", NULL };
const char *netcmd[] = { "nm-connection-editor", NULL };
const char *pavucontrolcmd[] = { "pavucontrol", NULL };
const char *screenshotcmd[] __attribute__((unused)) = { "/bin/sh", "-c", "slurp | grim -g - - | wl-copy", NULL };
const char *thunarcmd[] = { "thunar", NULL };

/* Wallpaper path - can be overridden by config file */
char wallpaper_path[PATH_MAX] = "$HOME/.nixlyos/wallpapers/beach.jpg";

/* Startup command run when no -s is provided; closes stdin to avoid status pipe */
/* The shell that runs this command inherits its stdin from a pipe
 * fed by the compositor's stdout (see run() in nixlytile.c).  We
 * background everything except waybar, then `exec waybar` so waybar
 * inherits the pipe — that's how its dwl/tags module receives our
 * printstatus() workspace events. */
char autostart_cmd[4096] =
	"eval $(gnome-keyring-daemon --start --components=secrets,ssh,pkcs11) & "
	"thunar --daemon & "
	"swaybg -i \"$HOME/.nixlyos/wallpapers/beach.jpg\" -m fill & "
	"exec waybar";

/* Maximum number of runtime keybindings */
#define MAX_KEYS 256
#define MAX_SPAWN_CMD 512

/* Runtime spawn commands - can be set from config */
char spawn_cmd_terminal[MAX_SPAWN_CMD] = "foot";
char spawn_cmd_terminal_alt[MAX_SPAWN_CMD] = "alacritty";
char spawn_cmd_browser[MAX_SPAWN_CMD] = "brave";
char spawn_cmd_filemanager[MAX_SPAWN_CMD] = "thunar";
char spawn_cmd_launcher[MAX_SPAWN_CMD] = "wmenu-run";

/* Runtime keybindings array - populated at startup */
Key runtime_keys[MAX_KEYS];
size_t runtime_keys_count = 0;

/* Default keybindings — Niri-style: vertical workspaces, horizontal columns.
 *   Mod+H/L         focus column left/right
 *   Mod+J/K         focus workspace down/up (vertical scroll)
 *   Mod+Shift+H/L   move focused column left/right
 *   Mod+Shift+J/K   move focused client to workspace below/above
 *   Mod+Q           kill window
 *   Mod+Return      spawn terminal
 *   Mod+F           toggle fullscreen
 *   Mod+Space       toggle floating
 *   Mod+Shift+Q     quit compositor
 */
const Key default_keys[] = {
	{ MODKEY,                    XKB_KEY_Return,     spawn,          {.v = alacrittycmd} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Return,     spawn,          {.v = termcmd} },
	{ MODKEY,                    XKB_KEY_e,          spawn,          {.v = thunarcmd} },
	{ MODKEY,                    XKB_KEY_BackSpace,  spawn,          {.v = chromecmd} },
	{ MODKEY,                    XKB_KEY_p,          spawn,          {.v = nixlylaunchercmd} },

	/* Column focus (horizontal scroll within workspace) */
	{ MODKEY,                    XKB_KEY_h,          focus_column_dir,    {.i = -1} },
	{ MODKEY,                    XKB_KEY_l,          focus_column_dir,    {.i = +1} },
	{ MODKEY,                    XKB_KEY_Left,       focus_column_dir,    {.i = -1} },
	{ MODKEY,                    XKB_KEY_Right,      focus_column_dir,    {.i = +1} },

	/* Workspace focus (vertical scroll between workspaces) */
	{ MODKEY,                    XKB_KEY_k,          focus_workspace_dir, {.i = -1} },
	{ MODKEY,                    XKB_KEY_j,          focus_workspace_dir, {.i = +1} },
	{ MODKEY,                    XKB_KEY_Up,         focus_workspace_dir, {.i = -1} },
	{ MODKEY,                    XKB_KEY_Down,       focus_workspace_dir, {.i = +1} },

	/* Mod+Tab — toggle between two most recently used workspaces */
	{ MODKEY,                    XKB_KEY_Tab,        focus_last_workspace, {0} },

	/* Move column left/right */
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_H,          move_column_dir,     {.i = -1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_L,          move_column_dir,     {.i = +1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Left,       move_column_dir,     {.i = -1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Right,      move_column_dir,     {.i = +1} },

	/* Resize focused tile.  Up = grow, Down = shrink.  Both edges that
	 * touch a neighbour move outward (grow) or inward (shrink); edges
	 * at the screen border stay locked. */
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Up,         resize_column_dir,   {.i = +1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Down,       resize_column_dir,   {.i = -1} },

	/* Move client to workspace above/below (vim-style keys retained;
	 * arrows are taken over by resize above) */
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_K,          move_client_to_ws_dir, {.i = -1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_J,          move_client_to_ws_dir, {.i = +1} },

	/* Window state */
	{ MODKEY,                    XKB_KEY_q,          killclient,        {0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_C,          killclient,        {0} },
	/* Mod+F = Niri-style column-expand (just width, navigation preserved)
	 * Mod+Shift+F = real Wayland fullscreen (for games / direct scanout) */
	{ MODKEY,                    XKB_KEY_f,          toggle_column_fullscreen, {0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_F,          togglefullscreen,  {0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_space,      togglefloating,    {0} },
	{ MODKEY,                    XKB_KEY_c,          togglefloating,    {0} },
	{ MODKEY,                    XKB_KEY_b,          togglewaybar,      {0} },

	/* Multi-monitor */
	{ MODKEY,                    XKB_KEY_comma,      focusmon,       {.i = WLR_DIRECTION_LEFT} },
	{ MODKEY,                    XKB_KEY_period,     focusmon,       {.i = WLR_DIRECTION_RIGHT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_less,       tagmon,         {.i = WLR_DIRECTION_LEFT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_greater,    tagmon,         {.i = WLR_DIRECTION_RIGHT} },

	/* Numbered workspace switch (Mod+1..9 = workspace 0..8).
	 * Mod+Shift+1..9 = move focused window to that workspace. */
	{ MODKEY,                    XKB_KEY_1,          focus_workspace_n,    {.i = 0} },
	{ MODKEY,                    XKB_KEY_2,          focus_workspace_n,    {.i = 1} },
	{ MODKEY,                    XKB_KEY_3,          focus_workspace_n,    {.i = 2} },
	{ MODKEY,                    XKB_KEY_4,          focus_workspace_n,    {.i = 3} },
	{ MODKEY,                    XKB_KEY_5,          focus_workspace_n,    {.i = 4} },
	{ MODKEY,                    XKB_KEY_6,          focus_workspace_n,    {.i = 5} },
	{ MODKEY,                    XKB_KEY_7,          focus_workspace_n,    {.i = 6} },
	{ MODKEY,                    XKB_KEY_8,          focus_workspace_n,    {.i = 7} },
	{ MODKEY,                    XKB_KEY_9,          focus_workspace_n,    {.i = 8} },

	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_1,          move_client_to_ws_n,  {.i = 0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_2,          move_client_to_ws_n,  {.i = 1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_3,          move_client_to_ws_n,  {.i = 2} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_4,          move_client_to_ws_n,  {.i = 3} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_5,          move_client_to_ws_n,  {.i = 4} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_6,          move_client_to_ws_n,  {.i = 5} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_7,          move_client_to_ws_n,  {.i = 6} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_8,          move_client_to_ws_n,  {.i = 7} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_9,          move_client_to_ws_n,  {.i = 8} },

	/* Alt + H/J/K/L or arrows: move tile within current workspace.
	 * Alt+H/Left, Alt+L/Right shift the focused column horizontally.
	 * Alt+K/Up,   Alt+J/Down move the focused window within its column. */
	{ WLR_MODIFIER_ALT,          XKB_KEY_h,          move_column_dir,            {.i = -1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_l,          move_column_dir,            {.i = +1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_Left,       move_column_dir,            {.i = -1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_Right,      move_column_dir,            {.i = +1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_k,          move_window_in_column_dir,  {.i = -1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_j,          move_window_in_column_dir,  {.i = +1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_Up,         move_window_in_column_dir,  {.i = -1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_Down,       move_window_in_column_dir,  {.i = +1} },

	/* Quit compositor */
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Q,          quit,           {0} },

	/* Monitor navigation: CTRL + arrow keys to warp cursor to monitor */
	{ MONITORKEY,                XKB_KEY_Up,         warptomonitor,  {.i = WLR_DIRECTION_UP} },
	{ MONITORKEY,                XKB_KEY_Down,       warptomonitor,  {.i = WLR_DIRECTION_DOWN} },
	{ MONITORKEY,                XKB_KEY_Left,       warptomonitor,  {.i = WLR_DIRECTION_LEFT} },
	{ MONITORKEY,                XKB_KEY_Right,      warptomonitor,  {.i = WLR_DIRECTION_RIGHT} },

	/* Move tile to monitor: CTRL + Shift + monitor number */
	MONITORKEYS(XKB_KEY_exclam,     0),  /* Ctrl+Shift+1 -> monitor 0 */
	MONITORKEYS(XKB_KEY_at,         1),  /* Ctrl+Shift+2 -> monitor 1 */
	MONITORKEYS(XKB_KEY_numbersign, 2),  /* Ctrl+Shift+3 -> monitor 2 */
	MONITORKEYS(XKB_KEY_dollar,     3),  /* Ctrl+Shift+4 -> monitor 3 */

	/* Ctrl-Alt-Backspace and Ctrl-Alt-Fx used to be handled by X server */
	{ WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_Terminate_Server, quit, {0} },
	/* Ctrl-Alt-Fx is used to switch to another VT, if you don't know what a VT is
	 * do not remove them.
	 */
#define CHVT(n) { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_XF86Switch_VT_##n, chvt, {.ui = (n)} }
	CHVT(1), CHVT(2), CHVT(3), CHVT(4), CHVT(5), CHVT(6),
	CHVT(7), CHVT(8), CHVT(9), CHVT(10), CHVT(11), CHVT(12),
};

const size_t default_keys_count = LENGTH(default_keys);

/* Pointer to active keybindings (runtime or default) */
const Key *keys = default_keys;
size_t keys_count = LENGTH(default_keys);

const Button buttons[] = {
	{ MODKEY, BTN_LEFT,   moveresize,     {.ui = CurMove} },
	{ MODKEY, BTN_MIDDLE, togglefloating, {0} },
	{ MODKEY, BTN_RIGHT,  moveresize,     {.ui = CurResize} },
};

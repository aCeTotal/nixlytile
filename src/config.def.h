/* Taken from https://github.com/djpohly/dwl/issues/466 */
#define COLOR(hex)    { ((hex >> 24) & 0xFF) / 255.0f, \
                        ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }
/* appearance — match Niri config:
 *   gaps 4, border off, focus-ring width 1 #00beff active / #595959 inactive */
const int sloppyfocus               = 1;  /* focus-follows-mouse (Niri) */
const int bypass_surface_visibility = 0;
const int smartgaps                 = 0;
int gaps                            = 1;
const unsigned int gappx            = 4;  /* Niri: gaps 4 */
const unsigned int borderpx         = 1;  /* Niri: focus-ring width 1 (border off) */
const float rootcolor[]             = COLOR(0x222222ff);
const float bordercolor[]           = COLOR(0x595959ff); /* Niri inactive */
const float focuscolor[]            = COLOR(0x00beffff); /* Niri active */
const float urgentcolor[]           = COLOR(0xff0000ff);
const unsigned int statusbar_height = 26;
const unsigned int statusbar_module_spacing = 10;
const unsigned int statusbar_module_padding = 8;
const unsigned int statusbar_icon_text_gap = 6; /* gap between icon and text */
const unsigned int statusbar_icon_text_gap_volume = 10;
const unsigned int statusbar_icon_text_gap_microphone = 8;
const unsigned int statusbar_icon_text_gap_cpu = statusbar_icon_text_gap;
const unsigned int statusbar_icon_text_gap_ram = statusbar_icon_text_gap;
const unsigned int statusbar_icon_text_gap_light = statusbar_icon_text_gap;
const unsigned int statusbar_icon_text_gap_battery = 2; /* slightly tighter */
const unsigned int statusbar_icon_text_gap_clock = statusbar_icon_text_gap;
const unsigned int statusbar_top_gap = 3;
const float statusbar_fg[]          = COLOR(0xffffffff);
const float statusbar_bg[]          = COLOR(0x00000016);
const float statusbar_popup_bg[]    = COLOR(0x00000080); /* ~50% */
const float statusbar_volume_muted_fg[] = COLOR(0xff4c4cff);
const float statusbar_mic_muted_fg[] = COLOR(0xff4c4cff);
const float statusbar_tag_bg[]      = COLOR(0x00000033);
const float statusbar_tag_active_bg[] = COLOR(0x1565c0ff);
const float statusbar_tag_hover_bg[] = COLOR(0x66b3ffaa);
const int statusbar_hover_fade_ms   = 0;
const char *statusbar_fonts[] = {
	"monospace:size=16:weight=Bold",
	"monospace:size=16"
};
const char *statusbar_font_attributes = NULL;
const int statusbar_font_spacing = 0;
const int statusbar_font_force_color = 1;
const enum fcft_subpixel statusbar_font_subpixel = FCFT_SUBPIXEL_DEFAULT;
const int statusbar_workspace_padding = 8;
const int statusbar_workspace_spacing = 4;
const int statusbar_thumb_height = 40;
const int statusbar_thumb_gap = 2;
const float statusbar_thumb_window[] = COLOR(0xffffff55);
/* This conforms to the xdg-protocol. Set the alpha to zero to restore the old behavior */
const float fullscreen_bg[]         = {0.1f, 0.1f, 0.1f, 1.0f}; /* You can also use glsl colors */
const float resize_factor           = 0.0002f; /* Resize multiplier for mouse resizing, depends on mouse sensivity. */
const uint32_t resize_interval_ms   = 8; /* Min interval between mouse-driven resize updates (ms). */
const double   resize_min_pixels    = 1.0; /* Min pointer movement (px) before a new resize if within interval. */
const float    resize_ratio_epsilon = 0.001f; /* Smallest ratio change that should trigger an arrange. */

/* window resizing */
const int lock_cursor = 0;	/* 1: lock cursor, 0: don't lock */

enum Direction { DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN };

/* tagging - TAGCOUNT must be no greater than 31 */
#define TAGCOUNT (9)

/* logging */
int log_level = WLR_DEBUG;

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
	{ "|w|",      btrtile },
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

/* keyboard — match Niri input { keyboard { xkb { layout "no" } } } */
const struct xkb_rule_names xkb_rules = {
	.layout = "no",
	.options = NULL,
};

const int repeat_delay = 300;   /* Niri: repeat-delay 300 */
const int repeat_rate = 100;    /* Niri: repeat-rate 100 */

/* Trackpad — match Niri touchpad { tap; natural-scroll } */
const int tap_to_click = 1;
const int tap_and_drag = 1;
const int drag_lock = 1;
const int natural_scrolling = 1; /* Niri: natural-scroll */
const int disable_while_typing = 1;
const int left_handed = 0;
const int middle_button_emulation = 0;
/* You can choose between:
LIBINPUT_CONFIG_SCROLL_NO_SCROLL
LIBINPUT_CONFIG_SCROLL_2FG
LIBINPUT_CONFIG_SCROLL_EDGE
LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN
*/
const enum libinput_config_scroll_method scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;

/* You can choose between:
LIBINPUT_CONFIG_CLICK_METHOD_NONE
LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS
LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
*/
const enum libinput_config_click_method click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;

/* You can choose between:
LIBINPUT_CONFIG_SEND_EVENTS_ENABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE
*/
const uint32_t send_events_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;

/* You can choose between:
LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE
*/
const enum libinput_config_accel_profile accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
const double accel_speed = 0.0;

/* You can choose between:
LIBINPUT_CONFIG_TAP_MAP_LRM -- 1/2/3 finger tap maps to left/right/middle
LIBINPUT_CONFIG_TAP_MAP_LMR -- 1/2/3 finger tap maps to left/middle/right
*/
const enum libinput_config_tap_button_map button_map = LIBINPUT_CONFIG_TAP_MAP_LRM;

/* If you want to use the windows key for MODKEY, use WLR_MODIFIER_LOGO */
#define MODKEY WLR_MODIFIER_LOGO

/* helper for spawning shell commands in the pre dwm-5.0 fashion */
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

/* commands — match Niri spawn-at-startup + binds */
const char *termcmd[] = { "foot", NULL };
const char *alacrittycmd[] = { "alacritty", NULL };
const char *chromecmd[] = { "google-chrome-stable", NULL };
const char *dolphincmd[] = { "dolphin", NULL };
const char *fuzzelcmd[] = { "fuzzel", NULL };
const char *apptogglecmd[] = { "apptoggle", NULL };
const char *lockcmd[] = { "nixly-lockscreen", NULL };
const char *screenshotcmd[] = { "grimshot", "copy", "area", NULL };
const char *screenshotscreencmd[] = { "grimshot", "copy", "output", NULL };

/* Startup matches Niri spawn-at-startup list.  Waybar uses the
 * nixlytile-specific config (dwl/tags + matching style.css). */
const char *const autostart_cmd =
	"swaybg -i \"$HOME/.nixlyos/wallpapers/beach.jpg\" -m fill & "
	"\"$HOME/.local/bin/niri-set-max-mode.sh\" --watch & "
	"waybar <&- & "
	"nm-applet --indicator & "
	"blueman-applet & "
	"xwayland-satellite & "
	"sh -c 'wl-paste --type text --watch clipman store --no-persist' & "
	"sh -c 'wl-paste --primary --type text --watch clipman store --no-persist' & "
	"appd & "
	"nixly_steam -silent";

/* Niri-matching keybindings (from ~/.config/niri/config.kdl) */
const Key keys[] = {
	/* Window management */
	{ MODKEY,                    XKB_KEY_q,          killclient,        {0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Q,          quit,              {0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_space,      togglefloating,    {0} },
	{ MODKEY,                    XKB_KEY_c,          togglefloating,    {0} },
	{ MODKEY,                    XKB_KEY_f,          maximize_column,   {0} },
	{ MODKEY,                    XKB_KEY_b,          togglestatusbar,   {0} },

	/* Focus navigation */
	{ MODKEY,                    XKB_KEY_h,          focus_column_dir,            {.i = -1} },
	{ MODKEY,                    XKB_KEY_l,          focus_column_dir,            {.i = +1} },
	{ MODKEY,                    XKB_KEY_j,          focus_window_in_column_dir,  {.i = +1} },
	{ MODKEY,                    XKB_KEY_k,          focus_window_in_column_dir,  {.i = -1} },
	{ MODKEY,                    XKB_KEY_Left,       focus_column_dir,            {.i = -1} },
	{ MODKEY,                    XKB_KEY_Right,      focus_column_dir,            {.i = +1} },
	{ MODKEY,                    XKB_KEY_Up,         focus_workspace_dir,         {.i = -1} },
	{ MODKEY,                    XKB_KEY_Down,       focus_workspace_dir,         {.i = +1} },

	/* Window movement */
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_H,          move_column_dir,             {.i = -1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_L,          move_column_dir,             {.i = +1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_J,          move_window_in_column_dir,   {.i = +1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_K,          move_window_in_column_dir,   {.i = -1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Left,       move_column_dir,             {.i = -1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Right,      move_column_dir,             {.i = +1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Up,         move_window_in_column_dir,   {.i = -1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Down,       move_window_in_column_dir,   {.i = +1} },

	/* Column width / consume / expel */
	{ MODKEY,                    XKB_KEY_r,          switch_preset_column_width,  {0} },
	{ MODKEY,                    XKB_KEY_a,          swap_window_dir,             {.i = -1} },
	{ MODKEY,                    XKB_KEY_d,          swap_window_dir,             {.i = +1} },
	{ MODKEY,                    XKB_KEY_x,          expel_window_from_column,    {0} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Left,       switch_preset_column_width,  {0} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Right,      switch_preset_column_width,  {0} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Up,         center_column,               {0} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Down,       maximize_column,             {0} },

	/* Applications */
	{ MODKEY,                    XKB_KEY_Return,     spawn,          {.v = alacrittycmd} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Return,     spawn,          {.v = termcmd} },
	{ MODKEY,                    XKB_KEY_p,          spawn,          {.v = apptogglecmd} },
	{ MODKEY,                    XKB_KEY_g,          spawn,          {.v = fuzzelcmd} },
	{ MODKEY,                    XKB_KEY_i,          spawn,          {.v = fuzzelcmd} },
	{ MODKEY,                    XKB_KEY_e,          spawn,          {.v = dolphincmd} },
	{ MODKEY,                    XKB_KEY_Escape,     spawn,          {.v = lockcmd} },
	{ MODKEY,                    XKB_KEY_F12,        spawn,          {.v = lockcmd} },
	{ MODKEY,                    XKB_KEY_BackSpace,  spawn,          {.v = chromecmd} },
	{ MODKEY,                    XKB_KEY_s,          spawn,          {.v = screenshotcmd} },
	{ 0,                         XKB_KEY_Print,      spawn,          {.v = screenshotcmd} },
	{ WLR_MODIFIER_SHIFT,        XKB_KEY_Print,      spawn,          {.v = screenshotscreencmd} },

	/* Workspaces */
	{ MODKEY,                    XKB_KEY_1,          focus_workspace_n, {.i = 0} },
	{ MODKEY,                    XKB_KEY_2,          focus_workspace_n, {.i = 1} },
	{ MODKEY,                    XKB_KEY_3,          focus_workspace_n, {.i = 2} },
	{ MODKEY,                    XKB_KEY_4,          focus_workspace_n, {.i = 3} },
	{ MODKEY,                    XKB_KEY_5,          focus_workspace_n, {.i = 4} },
	{ MODKEY,                    XKB_KEY_6,          focus_workspace_n, {.i = 5} },
	{ MODKEY,                    XKB_KEY_7,          focus_workspace_n, {.i = 6} },
	{ MODKEY,                    XKB_KEY_8,          focus_workspace_n, {.i = 7} },
	{ MODKEY,                    XKB_KEY_9,          focus_workspace_n, {.i = 8} },
	{ MODKEY,                    XKB_KEY_0,          focus_workspace_n, {.i = 9} },

	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_1,          move_client_to_ws_n, {.i = 0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_2,          move_client_to_ws_n, {.i = 1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_3,          move_client_to_ws_n, {.i = 2} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_4,          move_client_to_ws_n, {.i = 3} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_5,          move_client_to_ws_n, {.i = 4} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_6,          move_client_to_ws_n, {.i = 5} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_7,          move_client_to_ws_n, {.i = 6} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_8,          move_client_to_ws_n, {.i = 7} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_9,          move_client_to_ws_n, {.i = 8} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_0,          move_client_to_ws_n, {.i = 9} },

	/* Alt + H/J/K/L or arrows: move tile within current workspace. */
	{ WLR_MODIFIER_ALT,          XKB_KEY_h,          move_column_dir,            {.i = -1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_l,          move_column_dir,            {.i = +1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_Left,       move_column_dir,            {.i = -1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_Right,      move_column_dir,            {.i = +1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_k,          move_window_in_column_dir,  {.i = -1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_j,          move_window_in_column_dir,  {.i = +1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_Up,         move_window_in_column_dir,  {.i = -1} },
	{ WLR_MODIFIER_ALT,          XKB_KEY_Down,       move_window_in_column_dir,  {.i = +1} },

	{ MODKEY,                    XKB_KEY_Tab,        focus_last_workspace, {0} },

	/* Monitor navigation */
	{ MODKEY,                    XKB_KEY_comma,      focusmon,       {.i = WLR_DIRECTION_LEFT} },
	{ MODKEY,                    XKB_KEY_period,     focusmon,       {.i = WLR_DIRECTION_RIGHT} },

	{ WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_Terminate_Server, quit, {0} },
#define CHVT(n) { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_XF86Switch_VT_##n, chvt, {.ui = (n)} }
	CHVT(1), CHVT(2), CHVT(3), CHVT(4), CHVT(5), CHVT(6),
	CHVT(7), CHVT(8), CHVT(9), CHVT(10), CHVT(11), CHVT(12),
};

const Button buttons[] = {
	{ MODKEY, BTN_LEFT,   moveresize,     {.ui = CurMove} },
	{ MODKEY, BTN_MIDDLE, togglefloating, {0} },
	{ MODKEY, BTN_RIGHT,  moveresize,     {.ui = CurResize} },
};

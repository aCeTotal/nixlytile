/* Taken from https://github.com/djpohly/dwl/issues/466 */
#define COLOR(hex)    { ((hex >> 24) & 0xFF) / 255.0f, \
                        ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }

/* Runtime configurable variables - these can be overridden by config file */
/* appearance */
static int sloppyfocus               = 1;  /* focus follows mouse */
static int bypass_surface_visibility = 0;  /* 1 means idle inhibitors will disable idle tracking even if it's surface isn't visible  */
static int smartgaps                 = 0;  /* 1 means no outer gap when there is only one window */
static int gaps                      = 1;  /* 1 means gaps between windows are added */
static unsigned int gappx            = 5;  /* gap pixel between windows */
static unsigned int borderpx         = 1;  /* border pixel of windows */
static float rootcolor[]             = COLOR(0x222222ff);
static float bordercolor[]           = COLOR(0x444444ff);
static float focuscolor[]            = COLOR(0x005577ff);
static float urgentcolor[]           = COLOR(0xff0000ff);
static unsigned int statusbar_height = 26;
static unsigned int statusbar_module_spacing = 10;
static unsigned int statusbar_module_padding = 8;
static unsigned int statusbar_icon_text_gap = 6; /* gap between icon and text */
static unsigned int statusbar_icon_text_gap_volume = 10;
static unsigned int statusbar_icon_text_gap_microphone = 8;
static unsigned int statusbar_icon_text_gap_cpu = 6;
static unsigned int statusbar_icon_text_gap_ram = 6;
static unsigned int statusbar_icon_text_gap_light = 6;
static unsigned int statusbar_icon_text_gap_battery = 2; /* slightly tighter */
static unsigned int statusbar_icon_text_gap_clock = 6;
static unsigned int statusbar_top_gap = 3;
static float statusbar_fg[]          = COLOR(0xffffffff);
static float statusbar_bg[]          = COLOR(0x00000016);
static float statusbar_popup_bg[]    = COLOR(0x00000080); /* ~50% */
static float statusbar_volume_muted_fg[] = COLOR(0xff4c4cff);
static float statusbar_mic_muted_fg[] = COLOR(0xff4c4cff);
static float statusbar_tag_bg[]      = COLOR(0x00000033);
static float statusbar_tag_active_bg[] = COLOR(0x3399ff44);
static float statusbar_tag_hover_bg[] = COLOR(0x66b3ffaa);
static int statusbar_hover_fade_ms   = 0;
static int statusbar_tray_force_rgba = 0; /* 1: force RGBA decode for all tray icons (last-resort for apps that send RGBA) */
static const char *statusbar_fonts[8] = {
	"monospace:size=16:weight=Bold",
	"monospace:size=16",
	NULL, NULL, NULL, NULL, NULL, NULL
};
static const char *statusbar_font_attributes = NULL;
static int statusbar_font_spacing = 0;
static int statusbar_font_force_color = 1;
static enum fcft_subpixel statusbar_font_subpixel = FCFT_SUBPIXEL_DEFAULT;
static int statusbar_workspace_padding = 8;
static int statusbar_workspace_spacing = 4;
static int statusbar_thumb_height = 40;
static int statusbar_thumb_gap = 2;
static float statusbar_thumb_window[] = COLOR(0xffffff55);
/* This conforms to the xdg-protocol. Set the alpha to zero to restore the old behavior */
static float fullscreen_bg[]         = {0.1f, 0.1f, 0.1f, 1.0f}; /* You can also use glsl colors */
static float resize_factor           = 0.0002f; /* Resize multiplier for mouse resizing, depends on mouse sensivity. */
static uint32_t resize_interval_ms   = 24; /* Min interval between mouse-driven resize updates (higher smooths heavy apps). */
static double   resize_min_pixels    = 3.0; /* Min pointer movement before a new resize if within interval. */
static float    resize_ratio_epsilon = 0.002f; /* Smallest ratio change that should trigger an arrange. */
static int      modal_file_search_minlen = 1; /* Min chars before starting a file search */

/* window resizing */
static int lock_cursor = 0;	/* 1: lock cursor, 0: don't lock */

enum Direction { DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN };

/* tagging - TAGCOUNT must be no greater than 31 */
#define TAGCOUNT (9)

/* logging */
static int log_level = WLR_DEBUG;

/* HTPC mode - optimized for controller/TV usage */
static int htpc_mode_enabled = 1;           /* 1: HTPC mode features available */
static int htpc_mode_autostart = 0;         /* 1: start in HTPC mode on launch */
static int htpc_mode_auto_on_controller = 1; /* 1: auto-enter HTPC mode when controller connects */
static char htpc_wallpaper_path[PATH_MAX] = "$HOME/.nixlyos/wallpapers/htpc.jpg";

/* NOTE: ALWAYS keep a rule declared even if you don't use rules (e.g leave at least one example) */
static const Rule rules[] = {
	/* app_id             title       tags mask     isfloating   monitor */
	/* examples: */
	{ "Gimp_EXAMPLE",     NULL,       0,            1,           -1 }, /* Start on currently visible tags floating, not tiled */
	{ "firefox_EXAMPLE",  NULL,       1 << 8,       0,           -1 }, /* Start on ONLY tag "9" */
};

/* layout(s) */
static const Layout layouts[] = {
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
static const MonitorRule monrules[] = {
	/* name       mfact  nmaster scale layout       rotate/reflect                x    y */
	/* example of a HiDPI laptop monitor:
	{ "eDP-1",    0.5f,  1,      2,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1 },
	*/
	/* defaults */
	{ NULL,       0.55f, 1,      1,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1 },
};

/* keyboard */
static const struct xkb_rule_names xkb_rules = {
	/* can specify fields: rules, model, layout, variant, options */
	/* example:
	.options = "ctrl:nocaps",
	*/
	.options = NULL,
};

static int repeat_delay = 250;
static int repeat_rate = 60;

/* Trackpad */
static int tap_to_click = 1;
static int tap_and_drag = 1;
static int drag_lock = 1;
static int natural_scrolling = 0;
static int disable_while_typing = 1;
static int left_handed = 0;
static int middle_button_emulation = 0;
/* You can choose between:
LIBINPUT_CONFIG_SCROLL_NO_SCROLL
LIBINPUT_CONFIG_SCROLL_2FG
LIBINPUT_CONFIG_SCROLL_EDGE
LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN
*/
static enum libinput_config_scroll_method scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;

/* You can choose between:
LIBINPUT_CONFIG_CLICK_METHOD_NONE
LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS
LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
*/
static enum libinput_config_click_method click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;

/* You can choose between:
LIBINPUT_CONFIG_SEND_EVENTS_ENABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE
*/
static uint32_t send_events_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;

/* You can choose between:
LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE
*/
static enum libinput_config_accel_profile accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
static double accel_speed = 0.0;

/* You can choose between:
LIBINPUT_CONFIG_TAP_MAP_LRM -- 1/2/3 finger tap maps to left/right/middle
LIBINPUT_CONFIG_TAP_MAP_LMR -- 1/2/3 finger tap maps to left/middle/right
*/
static enum libinput_config_tap_button_map button_map = LIBINPUT_CONFIG_TAP_MAP_LRM;

/* If you want to use the windows key for MODKEY, use WLR_MODIFIER_LOGO */
/* Default modifier keys - used in compile-time key definitions */
#define MODKEY WLR_MODIFIER_LOGO
#define MONITORKEY WLR_MODIFIER_CTRL

/* Runtime modifier keys - can be overridden by config file */
static unsigned int modkey = WLR_MODIFIER_LOGO;
static unsigned int monitorkey = WLR_MODIFIER_CTRL;

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
static const char *termcmd[] = { "foot", NULL };
static const char *alacrittycmd[] = { "alacritty", NULL };
static const char *btopcmd[] = { "alacritty", "-e", "btop", NULL };
static const char *bravecmd[] = { "brave",
	"--no-first-run",
	"--no-default-browser-check",
	"--disable-session-crashed-bubble",
	"--disable-breakpad",
	"--disable-background-networking",
	"--disable-component-update",
	"--disable-sync",
	"--disable-features=TranslateUI",
	NULL };
static const char *menucmd[] __attribute__((unused)) = { "wmenu-run", NULL };
static const char *netcmd[] = { "nm-connection-editor", NULL };
static const char *thunarcmd[] = { "thunar", NULL };

/* Wallpaper path - can be overridden by config file */
static char wallpaper_path[PATH_MAX] = "$HOME/.nixlyos/wallpapers/beach.jpg";

/* Startup command run when no -s is provided; closes stdin to avoid status pipe */
static char autostart_cmd[4096] =
	"eval $(gnome-keyring-daemon --start --components=secrets,ssh,pkcs11) & "
	"thunar --daemon & swaybg -i \"$HOME/.nixlyos/wallpapers/beach.jpg\" -m fill <&-";

/* Maximum number of runtime keybindings */
#define MAX_KEYS 256
#define MAX_SPAWN_CMD 512

/* Runtime spawn commands - can be set from config */
static char spawn_cmd_terminal[MAX_SPAWN_CMD] = "foot";
static char spawn_cmd_terminal_alt[MAX_SPAWN_CMD] = "alacritty";
static char spawn_cmd_browser[MAX_SPAWN_CMD] = "brave";
static char spawn_cmd_filemanager[MAX_SPAWN_CMD] = "thunar";
static char spawn_cmd_launcher[MAX_SPAWN_CMD] = "wmenu-run";

/* Runtime keybindings array - populated at startup */
static Key runtime_keys[MAX_KEYS];
static size_t runtime_keys_count = 0;

/* Default keybindings - used if no config overrides */
static const Key default_keys[] = {
/* Note that Shift changes certain key codes: c -> C, 2 -> at, etc. */
/* modifier                  key                 function        argument */
{ MODKEY,                    XKB_KEY_p,          modal_show,     {0} },
	{ MODKEY,                    XKB_KEY_e,          spawn,          {.v = thunarcmd} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Return,     spawn,          {.v = termcmd} },
	{ MODKEY,                    XKB_KEY_j,          focusstack,     {.i = +1} },
	{ MODKEY,                    XKB_KEY_k,          focusstack,     {.i = -1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_J,          rotate_clients, {.i = +1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_K,          rotate_clients, {.i = -1} },
	{ MODKEY,                    XKB_KEY_i,          nixpkgs_show,   {0} },
	{ MODKEY,                    XKB_KEY_d,          incnmaster,     {.i = -1} },
	{ MODKEY,                    XKB_KEY_h,          setmfact,       {.f = -0.05f} },
	{ MODKEY,                    XKB_KEY_l,          setmfact,       {.f = +0.05f} },
	{ MODKEY,                    XKB_KEY_Return,     spawn,          {.v = alacrittycmd} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Return,     zoom,           {0} },
	{ MODKEY,                    XKB_KEY_Tab,        view,           {0} },
	{ MODKEY,                    XKB_KEY_g,          togglegaps,     {0} },
	{ MODKEY,                    XKB_KEY_b,          togglestatusbar,{0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_C,          killclient,     {0} },
	{ MODKEY,                    XKB_KEY_q,          killclient,     {0} },
	{ MODKEY,                    XKB_KEY_t,          setlayout,      {.v = &layouts[0]} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_f,          setlayout,      {.v = &layouts[1]} },
	{ MODKEY,                    XKB_KEY_space,      setlayout,      {0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_space,      togglefloating, {0} },
	{ MODKEY,                    XKB_KEY_f,          togglefullscreen, {0} },
	{ MODKEY,                    XKB_KEY_F5,         togglefullscreenadaptivesync, {0} },
	{ MODKEY,                    XKB_KEY_u,          testhzosd,      {0} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_i,          setcustomhz,    {.f = 25.55f} },
	{ MODKEY,                    XKB_KEY_0,          view,           {.ui = ~0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_parenright, tag,            {.ui = ~0} },
	{ MODKEY,                    XKB_KEY_comma,      focusmon,       {.i = WLR_DIRECTION_LEFT} },
	{ MODKEY,                    XKB_KEY_period,     focusmon,       {.i = WLR_DIRECTION_RIGHT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_less,       tagmon,         {.i = WLR_DIRECTION_LEFT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_greater,    tagmon,         {.i = WLR_DIRECTION_RIGHT} },
	{ MODKEY,                    XKB_KEY_BackSpace,  spawn,          {.v = bravecmd} },
	{ MODKEY,                    XKB_KEY_Up,         focusdir,       {.ui = DIR_UP} },
	{ MODKEY,                    XKB_KEY_Down,       focusdir,       {.ui = DIR_DOWN} },
	{ MODKEY,                    XKB_KEY_Right,      focusdir,       {.ui = DIR_RIGHT} },
	{ MODKEY,                    XKB_KEY_Left,       focusdir,       {.ui = DIR_LEFT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Up,         swapclients,    {.i = DIR_UP} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Down,       swapclients,    {.i = DIR_DOWN} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Right,      swapclients,    {.i = DIR_RIGHT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Left,       swapclients,    {.i = DIR_LEFT} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Right,      setratio_h,     {.f = +0.025f} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Left,       setratio_h,     {.f = -0.025f} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Up,         setratio_v,     {.f = -0.025f} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Down,       setratio_v,     {.f = +0.025f} },
	TAGKEYS(          XKB_KEY_1, XKB_KEY_exclam,                     0),
	TAGKEYS(          XKB_KEY_2, XKB_KEY_at,                         1),
	TAGKEYS(          XKB_KEY_3, XKB_KEY_numbersign,                 2),
	TAGKEYS(          XKB_KEY_4, XKB_KEY_dollar,                     3),
	TAGKEYS(          XKB_KEY_5, XKB_KEY_percent,                    4),
	TAGKEYS(          XKB_KEY_6, XKB_KEY_asciicircum,                5),
	TAGKEYS(          XKB_KEY_7, XKB_KEY_ampersand,                  6),
	TAGKEYS(          XKB_KEY_8, XKB_KEY_asterisk,                   7),
	TAGKEYS(          XKB_KEY_9, XKB_KEY_parenleft,                  8),
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

/* Pointer to active keybindings (runtime or default) */
static const Key *keys = default_keys;
static size_t keys_count = LENGTH(default_keys);

static const Button buttons[] = {
	{ MODKEY, BTN_LEFT,   moveresize,     {.ui = CurMove} },
	{ MODKEY, BTN_MIDDLE, togglefloating, {0} },
	{ MODKEY, BTN_RIGHT,  moveresize,     {.ui = CurResize} },
};

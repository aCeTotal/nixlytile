/*
 * nixlytile - Wayland compositor
 * Shared internal header for all modules
 */
#ifndef NIXLYTILE_H
#define NIXLYTILE_H

#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <systemd/sd-bus.h>
#include <limits.h>
#include <getopt.h>
#include <libinput.h>
#include <cairo/cairo.h>
#include <librsvg/rsvg.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <math.h>
#include <glib.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <fcft/fcft.h>
#include <pixman.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <strings.h>
#include <ctype.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/backend/drm.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <xkbcommon/xkbcommon.h>
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#endif
#include <tllist.h>
#include "content-type-v1-protocol.h"

#ifndef SD_BUS_EVENT_READABLE
#define SD_BUS_EVENT_READABLE 1
#endif
#ifndef SD_BUS_EVENT_WRITABLE
#define SD_BUS_EVENT_WRITABLE 2
#endif

#include "util.h"
#include "videoplayer/videoplayer.h"

/* ── macros ────────────────────────────────────────────────────────── */
#ifndef MAX
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#endif
#ifndef MIN
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#endif
#define UNUSED __attribute__((unused))
#define CLEANMASK(mask)         (mask & ~WLR_MODIFIER_CAPS)
#define VISIBLEON(C, M)         ((M) && (C)->mon == (M) && (((C)->tags & (M)->tagset[(M)->seltags]) || (C)->issticky))
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define END(A)                  ((A) + LENGTH(A))
#ifndef TAGCOUNT
#define TAGCOUNT (9)
#endif
#define TAGMASK                 ((1u << TAGCOUNT) - 1)
#define MAX_TAGS                32
#define STATUS_FAST_MS          3000
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
#define LISTEN_STATIC(E, H)     do { struct wl_listener *_l = ecalloc(1, sizeof(*_l)); _l->notify = (H); wl_signal_add((E), _l); } while (0)
#define MODAL_MAX_RESULTS 512
#define MODAL_RESULT_LEN 256
#define MAX_TRACKS 32
#define RESUME_CACHE_FILE "/tmp/nixly-resume-cache"
#define HTPC_MENU_MAX_ITEMS 12
#define MAX_GPUS 8
#define PC_GAMING_TILE_HEIGHT 180
#define PC_GAMING_TILE_GAP 15
#define PC_GAMING_PADDING 40
#define PC_GAMING_CACHE_FILE "/.cache/nixlytile/games.cache"
#define GAMEPAD_CURSOR_INTERVAL_MS 16
#define GAMEPAD_DEADZONE 4000
#define GAMEPAD_CURSOR_SPEED 15.0
#define GAMEPAD_CURSOR_ACCEL 2.5
#define GAMEPAD_INACTIVITY_TIMEOUT_MS 10000
#define GAMEPAD_INACTIVITY_CHECK_MS 5000
#define GAMEPAD_PENDING_MAX 8
#define BT_SCAN_INTERVAL_MS 300000
#define BT_SCAN_DURATION_MS 5000
#define MAX_CPU_CORES 256
#define NIXPKGS_MAX_ENTRIES 32768
#define OSK_ROWS 5
#define OSK_COLS 12
#define OSK_DPAD_INITIAL_DELAY 400
#define OSK_DPAD_REPEAT_RATE 50

/* Streaming service URLs */
#define NRK_URL "https://nrk.no/direkte/nrk1"
#define NETFLIX_URL "https://www.netflix.com/browse"
#define VIAPLAY_URL "https://viaplay.no"
#define TV2PLAY_URL "https://play.tv2.no"
#define F1TV_URL "https://f1tv.formula1.com/detail/1000005614/f1-live"
#ifndef WLR_SILENCE
#define WLR_SILENCE (WLR_ERROR - 1)
#endif

/* ── runtime config constants ─────────────────────────────────────── */
#ifndef MAX_KEYS
#define MAX_KEYS 256
#endif
#ifndef MAX_SPAWN_CMD
#define MAX_SPAWN_CMD 512
#endif

/* ── retro gaming constants ────────────────────────────────────────── */
#define RETRO_SLIDE_DURATION_MS 200  /* Smooth slide animation duration */

/* ── media grid constants ─────────────────────────────────────────── */
#define MEDIA_GRID_PADDING 40
#define MEDIA_GRID_GAP 20
#define MEDIA_SERVER_PORT 8080
#define MEDIA_DISCOVERY_PORT 8081
#define MEDIA_DISCOVERY_MAGIC "NIXLY_DISCOVER"
#define MEDIA_DISCOVERY_RESPONSE "NIXLY_SERVER"
#define MAX_MEDIA_SERVERS 16

typedef struct {
	char url[256];
	char server_id[64];
	char server_name[128];
	int priority;
	int is_local;
	int is_configured;
} MediaServer;

extern MediaServer media_servers[MAX_MEDIA_SERVERS];
extern int media_server_count;
extern uint64_t last_discovery_attempt_ms;
extern char discovered_server_url[256];
extern int server_discovered;
extern const char *osk_layout_lower[OSK_ROWS][OSK_COLS];
extern const char *osk_layout_upper[OSK_ROWS][OSK_COLS];

#define MAX_MONITORS 16

/* ── joystick navigation constants ────────────────────────────────── */
#define JOYSTICK_NAV_INITIAL_DELAY 300  /* ms before repeat starts */
#define JOYSTICK_NAV_REPEAT_RATE 150    /* ms between repeats */

/* ── stats panel constants ────────────────────────────────────────── */
#define STATS_PANEL_ANIM_DURATION 250

/* ── game VRR constants ───────────────────────────────────────────── */
#define GAME_VRR_MIN_INTERVAL_NS (500ULL * 1000000ULL)
#define GAME_VRR_STABLE_FRAMES 30
#define GAME_VRR_FPS_DEADBAND 3.0f
#define GAME_VRR_MIN_FPS 20.0f
#define GAME_VRR_MAX_FPS 165.0f

/* ── enums ─────────────────────────────────────────────────────────── */
enum { CurNormal, CurPressed, CurMove, CurResize };
enum { XDGShell, LayerShell, X11 };
enum { LyrBg, LyrBottom, LyrTile, LyrFloat, LyrTop, LyrFS, LyrOverlay, LyrBlock, NUM_LAYERS };
enum Direction { DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN };

typedef enum {
	GAMING_SERVICE_STEAM = 0,
	GAMING_SERVICE_HEROIC,
	GAMING_SERVICE_LUTRIS,
	GAMING_SERVICE_BOTTLES,
	GAMING_SERVICE_COUNT
} GamingServiceType;

typedef enum {
	RETRO_NES = 0,
	RETRO_SNES,
	RETRO_N64,
	RETRO_GAMECUBE,
	RETRO_WII,
	RETRO_SWITCH,
	RETRO_CONSOLE_COUNT
} RetroConsole;

typedef enum {
	MEDIA_VIEW_MOVIES = 0,
	MEDIA_VIEW_TVSHOWS = 1
} MediaViewType;

typedef enum {
	DETAIL_FOCUS_INFO = 0,
	DETAIL_FOCUS_SEASONS,
	DETAIL_FOCUS_EPISODES
} DetailFocusArea;

typedef enum {
	MON_POS_MASTER = 0,
	MON_POS_LEFT,
	MON_POS_RIGHT,
	MON_POS_TOP_LEFT,
	MON_POS_TOP_RIGHT,
	MON_POS_BOTTOM_LEFT,
	MON_POS_BOTTOM_RIGHT,
	MON_POS_AUTO,
	MON_POS_COUNT
} MonitorPosition;

typedef enum {
	GPU_VENDOR_UNKNOWN = 0,
	GPU_VENDOR_INTEL,
	GPU_VENDOR_AMD,
	GPU_VENDOR_NVIDIA
} GpuVendor;

typedef enum {
	PLAYBACK_IDLE = 0,
	PLAYBACK_BUFFERING,
	PLAYBACK_PLAYING,
	PLAYBACK_ACTIVE
} PlaybackState;

typedef enum {
	OSD_MENU_NONE = 0,
	OSD_MENU_SOUND,
	OSD_MENU_SUBTITLES
} OsdMenuType;

/* ── forward declarations ──────────────────────────────────────────── */
typedef struct LayoutNode LayoutNode;
typedef struct Monitor Monitor;
typedef struct StatusBar StatusBar;
typedef struct StatusModule StatusModule;
typedef struct GameEntry GameEntry;
typedef struct MediaItem MediaItem;
typedef struct MediaSeason MediaSeason;
typedef struct GamepadDevice GamepadDevice;
typedef struct TrayMenuEntry TrayMenuEntry;
typedef struct TrayItem TrayItem;

/* ── basic types ───────────────────────────────────────────────────── */
typedef union {
	int i;
	uint32_t ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	unsigned int button;
	void (*func)(const Arg *);
	const Arg arg;
} Button;

typedef struct {
	uint32_t mod;
	xkb_keysym_t keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

typedef struct {
	const char *id;
	const char *title;
	uint32_t tags;
	int isfloating;
	int monitor;
} Rule;

typedef struct {
	const char *name;
	float mfact;
	int nmaster;
	float scale;
	const Layout *lt;
	enum wl_output_transform rr;
	int x, y;
} MonitorRule;

typedef struct {
	char name[64];
	MonitorPosition position;
	int width;
	int height;
	float refresh;
	float scale;
	float mfact;
	int nmaster;
	int enabled;
	int transform;
} RuntimeMonitorConfig;

extern RuntimeMonitorConfig runtime_monitors[MAX_MONITORS];
extern int runtime_monitor_count;
extern int monitor_master_set;

typedef struct {
	const char *name;
	void (*func)(const Arg *);
	int arg_type;
} FuncEntry;

extern const FuncEntry func_table[];
MonitorPosition config_parse_monitor_position(const char *pos);
xkb_keysym_t config_parse_keysym(const char *name);
void add_media_server(const char *url, int is_local, int is_configured);
extern const struct wlr_buffer_impl pixman_buffer_impl;

typedef struct {
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
} PointerConstraint;

typedef struct {
	struct wlr_scene_tree *scene;
	struct wlr_session_lock_v1 *lock;
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
} SessionLock;

/* ── font / glyph ─────────────────────────────────────────────────── */
struct GlyphRun {
	const struct fcft_glyph *glyph;
	int pen_x;
	uint32_t codepoint;
};

struct StatusFont {
	struct fcft_font *font;
	int ascent;
	int descent;
	int height;
};

/* ── status bar types ──────────────────────────────────────────────── */
struct StatusModule {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int width;
	int x;
	int box_x[MAX_TAGS];
	int box_w[MAX_TAGS];
	int box_tag[MAX_TAGS];
	int box_count;
	uint32_t tagmask;
	int hover_tag;
	float hover_alpha[MAX_TAGS];
};

typedef struct {
	pid_t pid;
	char name[64];
	double cpu;
	double max_single_cpu;
	int y;
	int height;
	int kill_x, kill_y, kill_w, kill_h;
	int has_kill;
} CpuProcEntry;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int width;
	int height;
	int visible;
	int hover_idx;
	int refresh_data;
	uint64_t last_fetch_ms;
	uint64_t last_render_ms;
	uint64_t suppress_refresh_until_ms;
	uint64_t hover_start_ms;
	int proc_count;
	CpuProcEntry procs[10];
} CpuPopup;

typedef struct {
	pid_t pid;
	char name[64];
	unsigned long mem_kb;
	int y;
	int height;
	int kill_x, kill_y, kill_w, kill_h;
	int has_kill;
} RamProcEntry;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int width;
	int height;
	int visible;
	int hover_idx;
	int refresh_data;
	uint64_t last_fetch_ms;
	uint64_t last_render_ms;
	uint64_t suppress_refresh_until_ms;
	uint64_t hover_start_ms;
	int proc_count;
	RamProcEntry procs[15];
} RamPopup;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int width;
	int height;
	int visible;
	int refresh_data;
	uint64_t last_fetch_ms;
	uint64_t last_render_ms;
	uint64_t suppress_refresh_until_ms;
	uint64_t hover_start_ms;
	int charging;
	double percent;
	double voltage_v;
	double power_w;
	double time_remaining_h;
} BatteryPopup;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int width;
	int height;
	int visible;
	int anchor_x;
	int anchor_y;
	int anchor_w;
	uint64_t suppress_refresh_until_ms;
	uint64_t hover_start_ms;
} NetPopup;

typedef struct {
	void (*fn)(void);
	uint64_t next_due_ms;
} StatusRefreshTask;

#define STATUS_TASKS_COUNT 9

#define FAN_MAX_DEVICES   8
#define FAN_MAX_PER_DEV   6
#define FAN_MAX_TOTAL    (FAN_MAX_DEVICES * FAN_MAX_PER_DEV)

typedef enum {
	FAN_DEV_CPU,
	FAN_DEV_CASE,
	FAN_DEV_GPU_AMD,
	FAN_DEV_GPU_NVIDIA,
	FAN_DEV_MSI_EC,
	FAN_DEV_UNKNOWN,
} FanDevType;

typedef struct {
	char label[64];
	char hwmon_path[128];
	int fan_index;
	int pwm_index;
	int has_pwm;
	int rpm;
	int pwm;
	int pwm_enable;
	int temp_mc;
	uint8_t ec_reg_rpm;
	uint8_t ec_reg_rpm_h;  /* 16-bit RPM tachometer high byte */
	uint8_t ec_reg_rpm_l;  /* 16-bit RPM tachometer low byte */
	uint8_t ec_reg_temp;
	int msi_sysfs; /* uses /sys/devices/platform/msi-ec/ */
	char msi_sysfs_dir[16]; /* "cpu" or "gpu" */
	int slider_x, slider_y;
	int slider_w, slider_h;
	int row_y, row_h;
} FanEntry;

typedef struct {
	char name[64];
	char hwmon_path[128];
	FanDevType type;
	int fan_count;
	FanEntry fans[FAN_MAX_PER_DEV];
} FanDevice;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int width;
	int height;
	int visible;
	uint64_t last_fetch_ms;
	uint64_t last_render_ms;
	uint64_t hover_start_ms;
	int dragging;
	int drag_fan_idx;
	int device_count;
	FanDevice devices[FAN_MAX_DEVICES];
	int total_fans;
	/* msi-ec system-wide controls */
	int msi_ec;
	int fan_mode;           /* 0=auto, 1=silent, 2=advanced */
	int shift_mode;         /* 0=eco, 1=comfort, 2=sport, 3=turbo */
	int cooler_boost;       /* 0=off, 1=on */
	/* hit areas for msi-ec controls */
	int fanmode_y, fanmode_h;
	int shiftmode_y, shiftmode_h;
	int boost_y, boost_h;
} FanPopup;

typedef struct TrayMenuEntry {
	int id;
	int enabled;
	int is_separator;
	int depth;
	int has_submenu;
	int toggle_type;
	int toggle_state;
	int y;
	int height;
	char label[256];
	struct wl_list link;
} TrayMenuEntry;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int width;
	int height;
	int visible;
	int x;
	int y;
	char service[128];
	char menu_path[128];
	struct wl_list entries;
} TrayMenu;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	struct wlr_scene_tree *submenu_tree;
	struct wlr_scene_tree *submenu_bg;
	int width, height;
	int submenu_width, submenu_height;
	int x, y;
	int submenu_x, submenu_y;
	int visible;
	int submenu_visible;
	int hover;
	int submenu_hover;
	int submenu_type;
	struct wl_list entries;
	struct wl_list networks;
} NetMenu;

struct StatusBar {
	struct wlr_scene_tree *tree;
	struct wlr_box area;
	StatusModule clock;
	StatusModule cpu;
	StatusModule battery;
	StatusModule net;
	StatusModule light;
	StatusModule mic;
	StatusModule volume;
	StatusModule ram;
	StatusModule tags;
	StatusModule traylabel;
	StatusModule bluetooth;
	StatusModule steam;
	StatusModule discord;
	CpuPopup cpu_popup;
	RamPopup ram_popup;
	BatteryPopup battery_popup;
	NetPopup net_popup;
	FanPopup fan_popup;
	TrayMenu tray_menu;
	NetMenu net_menu;
	StatusModule sysicons;
	StatusModule fan;
};

struct TrayItem {
	char service[128];
	char path[128];
	char label[64];
	char menu[128];
	int has_menu;
	int icon_tried;
	int icon_failed;
	int x;
	int w;
	int icon_w;
	int icon_h;
	struct wlr_buffer *icon_buf;
	struct wl_list link;
};

/* ── buffer types ──────────────────────────────────────────────────── */
struct PixmanBuffer {
	struct wlr_buffer base;
	pixman_image_t *image;
	void *data;
	uint32_t drm_format;
	int stride;
	int owns_data;
};

struct CpuSample {
	unsigned long long idle;
	unsigned long long total;
};

/* ── modal overlay ─────────────────────────────────────────────────── */
typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int visible;
	int x, y, width, height;
	int active_idx;
	char search[3][256];
	int search_len[3];
	char search_rendered[3][256];
	struct wlr_scene_tree *search_field_tree;
	struct wl_event_source *render_timer;
	int render_pending;
	int result_count[3];
	char results[3][MODAL_MAX_RESULTS][MODAL_RESULT_LEN];
	int result_entry_idx[3][MODAL_MAX_RESULTS];
	int selected[3];
	int scroll[3];
	char file_results_name[MODAL_MAX_RESULTS][128];
	char file_results_path[MODAL_MAX_RESULTS][PATH_MAX];
	time_t file_results_mtime[MODAL_MAX_RESULTS];
	pid_t file_search_pid;
	int file_search_fd;
	int file_search_fallback;
	struct wl_event_source *file_search_event;
	size_t file_search_len;
	char file_search_buf[4096];
	char file_search_last[256];
	struct wl_event_source *file_search_timer;
	char git_results_name[MODAL_MAX_RESULTS][128];
	char git_results_path[MODAL_MAX_RESULTS][PATH_MAX];
	time_t git_results_mtime[MODAL_MAX_RESULTS];
	int git_result_count;
	pid_t git_search_pid;
	int git_search_fd;
	struct wl_event_source *git_search_event;
	size_t git_search_len;
	char git_search_buf[4096];
	int git_search_done;
	char git_search_last[256];
	struct wlr_scene_tree *results_tree;
	int last_scroll;
	int last_selected;
	struct wlr_scene_rect *row_highlights[MODAL_MAX_RESULTS];
	int row_highlight_count;
} ModalOverlay;

/* ── nixpkgs ───────────────────────────────────────────────────────── */
typedef struct {
	char name[128];
	char name_lower[128];
	char version[64];
	int installed;
} NixpkgEntry;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int visible;
	int x, y, width, height;
	char search[256];
	int search_len;
	char search_rendered[256];
	struct wlr_scene_tree *search_field_tree;
	int result_count;
	int result_indices[MODAL_MAX_RESULTS];
	int selected;
	int scroll;
	struct wlr_scene_tree *results_tree;
	int last_scroll;
	int last_selected;
	struct wlr_scene_rect *row_highlights[MODAL_MAX_RESULTS];
	int row_highlight_count;
} NixpkgsOverlay;

typedef struct {
	char name[128];
	char exec[256];
	char name_lower[128];
	int used;
	int prefers_dgpu;
} DesktopEntry;

/* ── network types ─────────────────────────────────────────────────── */
typedef struct WifiNetwork {
	char ssid[128];
	int strength;
	int secure;
	struct wl_list link;
} WifiNetwork;

typedef struct VpnConnection {
	char name[128];
	char uuid[64];
	int active;
	struct wl_list link;
} VpnConnection;

/* NetMenu is embedded in StatusBar as anonymous struct */

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int visible;
	int width, height;
	char ssid[128];
	char password[256];
	int password_len;
	int cursor_pos;
	int button_hover;
	int connecting;
	int error;
	int try_saved;
	pid_t connect_pid;
	int connect_fd;
	struct wl_event_source *connect_event;
} WifiPasswordPopup;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int visible;
	int width, height;
	char title[128];
	char password[256];
	int password_len;
	int cursor_pos;
	int button_hover;
	int running;
	int error;
	char pending_cmd[1024];
	char pending_pkg[140];
	pid_t sudo_pid;
	int sudo_fd;
	struct wl_event_source *sudo_event;
	struct wl_event_source *wait_timer;
} SudoPopup;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int visible;
	int width, height;
	int sel_row, sel_col;
	int shift_active;
	int caps_lock;
	char input_buffer[1024];
	int input_len;
	int input_cursor;
	void (*callback)(const char *text, void *data);
	void *callback_data;
	struct wlr_surface *target_surface;
} OnScreenKeyboard;

/* ── gamepad types ─────────────────────────────────────────────────── */
typedef struct {
	const char *label;
	const char *command;
} GamepadMenuItem;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	struct wlr_scene_tree *dim;
	int visible;
	int x, y;
	int width, height;
	int selected;
	int item_count;
} GamepadMenu;

typedef struct AxisCalibration {
	int min, max, center;
	int flat;
} AxisCalibration;

struct GamepadDevice {
	int fd;
	char path[64];
	char name[128];
	char bluez_path[256];
	struct wl_event_source *event_source;
	struct wl_list link;
	int left_x, left_y;
	int right_x, right_y;
	AxisCalibration cal_lx, cal_ly;
	AxisCalibration cal_rx, cal_ry;
	int64_t last_activity_ms;
	int suspended;
	int grabbed;
	uint64_t connect_time_ms;
};

/* ── gaming types ──────────────────────────────────────────────────── */
struct GameEntry {
	char id[64];
	char name[256];
	char icon_path[512];
	char launch_cmd[1024];
	GamingServiceType service;
	int installed;
	int playtime_minutes;
	int is_game;
	time_t acquired_time;
	int controller_support;
	int deck_verified;
	int is_installing;
	int install_progress;
	struct wlr_buffer *icon_buf;
	int icon_w, icon_h;
	int icon_loaded;
	char launch_params_nvidia[512];
	char launch_params_amd[512];
	char launch_params_intel[512];
	int has_custom_params;
	struct GameEntry *next;
};

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	struct wlr_scene_tree *grid;
	struct wlr_scene_tree *sidebar;
	int visible;
	unsigned int view_tag;
	int width, height;
	int scroll_offset;
	int selected_idx;
	int hover_idx;
	int game_count;
	int cols;
	int service_filter;
	GameEntry *games;
	int needs_refresh;
	uint64_t last_refresh_ms;
	struct wlr_scene_tree *install_popup;
	struct wlr_scene_tree *install_dim;
	int install_popup_visible;
	int install_popup_selected;
	char install_game_id[64];
	char install_game_name[256];
	GamingServiceType install_game_service;
} PcGamingView;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *dim;
	struct wlr_scene_tree *menu_bar;
	int visible;
	unsigned int view_tag;
	int width, height;
	int selected_console;
	int target_console;
	int anim_direction;
	float slide_offset;
	uint64_t slide_start_ms;
} RetroGamingView;

/* ── media types ───────────────────────────────────────────────────── */
struct MediaItem {
	int id;
	int type;
	char title[256];
	char show_name[256];
	int season;
	int episode;
	int duration;
	int year;
	float rating;
	char poster_path[512];
	char backdrop_path[512];
	char overview[2048];
	char genres[256];
	char episode_title[256];
	char filepath[1024];
	int tmdb_total_seasons;
	int tmdb_total_episodes;
	int tmdb_episode_runtime;
	char tmdb_status[64];
	char tmdb_next_episode[16];
	int tmdb_id;
	char server_id[64];
	char server_url[256];
	int server_priority;
	struct wlr_buffer *poster_buf;
	struct wlr_buffer *backdrop_buf;
	int poster_w, poster_h;
	int backdrop_w, backdrop_h;
	int poster_loaded;
	int backdrop_loaded;
	struct MediaItem *next;
};

struct MediaSeason {
	int season;
	int episode_count;
	struct MediaSeason *next;
};

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *grid;
	struct wlr_scene_tree *detail_panel;
	int visible;
	unsigned int view_tag;
	int width, height;
	int scroll_offset;
	int selected_idx;
	int item_count;
	int cols;
	MediaItem *items;
	MediaViewType view_type;
	int needs_refresh;
	uint64_t last_refresh_ms;
	char server_url[256];
	uint32_t last_data_hash;
	int in_detail_view;
	MediaItem *detail_item;
	DetailFocusArea detail_focus;
	MediaSeason *seasons;
	int season_count;
	int selected_season_idx;
	int selected_season;
	MediaItem *episodes;
	int episode_count;
	int selected_episode_idx;
	int season_scroll_offset;
	int episode_scroll_offset;
} MediaGridView;

/* ── GPU info ──────────────────────────────────────────────────────── */
typedef struct {
	char card_path[64];
	char render_path[64];
	char driver[32];
	char pci_slot[16];
	char pci_slot_underscore[20];
	GpuVendor vendor;
	int is_discrete;
	int card_index;
	int render_index;
} GpuInfo;

/* ── resume cache ──────────────────────────────────────────────────── */
typedef struct {
	int media_id;
	double position;
} ResumeEntry;

/* ── client ────────────────────────────────────────────────────────── */
typedef struct {
	/* Must keep this field first */
	unsigned int type; /* XDGShell or X11* */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_rect *border[4];
	struct wlr_scene_tree *scene_surface;
	struct wl_list link;
	struct wl_list flink;
	struct wlr_box geom;
	struct wlr_box prev;
	struct wlr_box bounds;
	union {
		struct wlr_xdg_surface *xdg;
		struct wlr_xwayland_surface *xwayland;
	} surface;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener maximize;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener fullscreen;
	struct wl_listener set_decoration_mode;
	struct wl_listener destroy_decoration;
#ifdef XWAYLAND
	struct wl_listener activate;
	struct wl_listener associate;
	struct wl_listener minimize;
	struct wl_listener dissociate;
	struct wl_listener configure;
	struct wl_listener set_hints;
#endif
	unsigned int bw;
	uint32_t tags;
	int isfloating, isurgent, isfullscreen, issticky, was_tiled;
	uint32_t resize;
	int pending_resize_w, pending_resize_h;
	struct wlr_box old_geom;
	char *output;
	uint64_t frame_times[32];
	int frame_time_idx;
	int frame_time_count;
	float detected_video_hz;
	struct wlr_buffer *last_buffer;
	int video_detect_retries;
	int video_detect_phase;
} Client;

/* ── binary tree tiling layout node ───────────────────────────────── */
struct LayoutNode {
	int is_client_node;
	float split_ratio;
	unsigned int is_split_vertically;
	Client *client;
	LayoutNode *left;
	LayoutNode *right;
	LayoutNode *split_node;
};

/* ── keyboard / text input ─────────────────────────────────────────── */
typedef struct {
	struct wlr_keyboard_group *wlr_group;
	int nsyms;
	const xkb_keysym_t *keysyms;
	uint32_t mods;
	struct wl_event_source *key_repeat_source;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
} KeyboardGroup;

typedef struct {
	struct wlr_text_input_v3 *text_input;
	struct wl_list link;
	struct wl_listener enable;
	struct wl_listener disable;
	struct wl_listener commit;
	struct wl_listener destroy;
} TextInput;

/* ── layer surface ─────────────────────────────────────────────────── */
typedef struct {
	/* Must keep this field first */
	unsigned int type; /* LayerShell */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_tree *popups;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wl_list link;
	int mapped;
	struct wlr_layer_surface_v1 *layer_surface;

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
} LayerSurface;

/* ── monitor ───────────────────────────────────────────────────────── */
struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *fullscreen_bg;
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener request_state;
	struct wl_listener destroy_lock_surface;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_box m;
	struct wlr_box w;
	struct wl_list layers[4];
	StatusBar statusbar;
	ModalOverlay modal;
	NixpkgsOverlay nixpkgs;
	WifiPasswordPopup wifi_popup;
	SudoPopup sudo_popup;
	OnScreenKeyboard osk;
	GamepadMenu gamepad_menu;
	PcGamingView pc_gaming;
	RetroGamingView retro_gaming;
	MediaGridView movies_view;
	MediaGridView tvshows_view;
	const Layout *lt[2];
	int gaps;
	int showbar;
	unsigned int seltags;
	unsigned int sellt;
	uint32_t tagset[2];
	float mfact;
	int gamma_lut_changed;
	int nmaster;
	char ltsymbol[16];
	int asleep;
	int is_mirror;  /* This output mirrors the laptop display */
	LayoutNode *root[MAX_TAGS];
	struct wlr_output_mode *original_mode;
	int video_mode_active;
	int vrr_capable;
	int vrr_active;
	float vrr_target_hz;
	int game_vrr_active;
	float game_vrr_target_fps;
	float game_vrr_last_fps;
	uint64_t game_vrr_last_change_ns;
	int game_vrr_stable_frames;
	struct wlr_scene_tree *hz_osd_tree;
	struct wlr_scene_tree *hz_osd_bg;
	int hz_osd_visible;
	uint64_t last_frame_ns;
	uint64_t last_commit_duration_ns;
	uint64_t rolling_commit_time_ns;
	int frames_since_content_change;
	int direct_scanout_active;
	int direct_scanout_notified;
	struct wlr_scene_tree *toast_tree;
	struct wl_event_source *toast_timer;
	int toast_visible;
	struct wl_listener present;
	uint64_t last_present_ns;
	uint64_t present_interval_ns;
	uint64_t target_present_ns;
	int pending_game_frame;
	uint64_t game_frame_submit_ns;
	uint64_t game_frame_intervals[16];
	int game_frame_interval_idx;
	int game_frame_interval_count;
	float estimated_game_fps;
	int frame_pacing_active;
	uint64_t frames_presented;
	uint64_t frames_dropped;
	uint64_t frames_held;
	uint64_t total_latency_ns;
	uint64_t fps_limit_last_frame_ns;
	uint64_t fps_limit_interval_ns;
	int frame_repeat_enabled;
	int frame_repeat_count;
	int frame_repeat_current;
	int frame_repeat_candidate;     /* pending repeat count (hysteresis) */
	int frame_repeat_candidate_age; /* vblanks candidate has been stable */
	uint64_t frame_repeat_interval_ns;
	uint64_t last_game_buffer_id;
	uint64_t frames_repeated;
	int adaptive_pacing_enabled;
	float target_frame_time_ms;
	uint64_t pacing_adjustment_ns;
	int judder_score;
	uint64_t predicted_next_frame_ns;
	uint64_t frame_variance_ns;
	int frames_early;
	int frames_late;
	float prediction_accuracy;
	uint64_t last_input_ns;
	uint64_t input_to_frame_ns;
	uint64_t min_input_latency_ns;
	uint64_t max_input_latency_ns;
	int memory_pressure;
	int supports_10bit;
	int render_10bit_active;
	int max_bpc;
	int hdr_capable;
	int hdr_active;
	struct wlr_scene_tree *stats_panel_tree;
	struct wl_event_source *stats_panel_timer;
	struct wl_event_source *stats_panel_anim_timer;
	int stats_panel_visible;
	int stats_panel_target_x;
	int stats_panel_current_x;
	int stats_panel_width;
	uint64_t stats_panel_anim_start;
	int stats_panel_animating;
};

/* ── extern globals ────────────────────────────────────────────────── */

/* config variables (defined in config.c, defaults from config.h) */
extern int sloppyfocus;
extern int bypass_surface_visibility;
extern int smartgaps;
extern int gaps;
extern unsigned int gappx;
extern unsigned int borderpx;
extern float rootcolor[];
extern float bordercolor[];
extern float focuscolor[];
extern float urgentcolor[];
extern unsigned int statusbar_height;
extern unsigned int statusbar_module_spacing;
extern unsigned int statusbar_module_padding;
extern unsigned int statusbar_icon_text_gap;
extern unsigned int statusbar_icon_text_gap_volume;
extern unsigned int statusbar_icon_text_gap_microphone;
extern unsigned int statusbar_icon_text_gap_cpu;
extern unsigned int statusbar_icon_text_gap_ram;
extern unsigned int statusbar_icon_text_gap_light;
extern unsigned int statusbar_icon_text_gap_battery;
extern unsigned int statusbar_icon_text_gap_clock;
extern unsigned int statusbar_top_gap;
extern float statusbar_fg[];
extern float statusbar_bg[];
extern float statusbar_popup_bg[];
extern float statusbar_volume_muted_fg[];
extern float statusbar_mic_muted_fg[];
extern float statusbar_tag_bg[];
extern float statusbar_tag_active_bg[];
extern float statusbar_tag_hover_bg[];
extern int statusbar_hover_fade_ms;
extern int statusbar_tray_force_rgba;
extern const char *statusbar_fonts[8];
extern const char *statusbar_font_attributes;
extern int statusbar_font_spacing;
extern int statusbar_font_force_color;
extern enum fcft_subpixel statusbar_font_subpixel;
extern int statusbar_workspace_padding;
extern int statusbar_workspace_spacing;
extern int statusbar_thumb_height;
extern int statusbar_thumb_gap;
extern float statusbar_thumb_window[];
extern float fullscreen_bg[];
extern float resize_factor;
extern uint32_t resize_interval_ms;
extern double resize_min_pixels;
extern float resize_ratio_epsilon;
extern int modal_file_search_minlen;
extern int lock_cursor;
extern int log_level;
extern int nixlytile_mode;
extern char htpc_wallpaper_path[PATH_MAX];
extern const Rule rules[];
extern const size_t nrules;
extern const Layout layouts[];
extern const size_t nlayouts;
extern const MonitorRule monrules[];
extern const size_t nmonrules;
extern const struct xkb_rule_names xkb_rules;
extern int repeat_delay;
extern int repeat_rate;
extern int tap_to_click;
extern int tap_and_drag;
extern int drag_lock;
extern int natural_scrolling;
extern int disable_while_typing;
extern int left_handed;
extern int middle_button_emulation;
extern enum libinput_config_scroll_method scroll_method;
extern enum libinput_config_click_method click_method;
extern uint32_t send_events_mode;
extern enum libinput_config_accel_profile accel_profile;
extern double accel_speed;
extern enum libinput_config_tap_button_map button_map;
extern unsigned int modkey;
extern unsigned int monitorkey;
extern const Key default_keys[];
extern const size_t default_keys_count;
extern const Key *keys;
extern size_t keys_count;
extern const Button buttons[];
extern const size_t nbuttons;
extern char spawn_cmd_terminal[MAX_SPAWN_CMD];
extern char spawn_cmd_terminal_alt[MAX_SPAWN_CMD];
extern char spawn_cmd_browser[MAX_SPAWN_CMD];
extern char spawn_cmd_filemanager[MAX_SPAWN_CMD];
extern char spawn_cmd_launcher[MAX_SPAWN_CMD];
extern const char *netcmd[];
extern const char *btopcmd[];
extern Key runtime_keys[MAX_KEYS];
extern char *runtime_spawn_cmds[MAX_KEYS];
extern int runtime_spawn_cmd_count;
extern size_t runtime_keys_count;
extern char wallpaper_path[PATH_MAX];
extern char autostart_cmd[4096];

/* core compositor */
extern pid_t child_pid;
extern int locked;
extern void *exclusive_focus;
extern struct wl_display *dpy;
extern struct wl_event_loop *event_loop;
extern VideoPlayer *active_videoplayer;
extern struct wlr_backend *backend;
extern struct wlr_scene *scene;
extern struct wlr_scene_tree *layers[];
extern struct wlr_scene_tree *drag_icon;
extern struct wlr_renderer *drw;
extern struct wlr_allocator *alloc;
extern struct wlr_compositor *compositor;
extern struct wlr_session *session;
extern struct StatusFont statusfont;
extern int fcft_initialized;
extern int has_nixlytile_session_target_cached;
extern struct wlr_xdg_shell *xdg_shell;
extern struct wlr_xdg_activation_v1 *activation;
extern struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
extern struct wl_list clients;
extern struct wl_list fstack;
extern struct wlr_idle_notifier_v1 *idle_notifier;
extern struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
extern struct wlr_layer_shell_v1 *layer_shell;
extern struct wlr_output_manager_v1 *output_mgr;
extern struct wlr_content_type_manager_v1 *content_type_mgr;
extern struct wlr_tearing_control_manager_v1 *tearing_control_mgr;
extern struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
extern struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
extern struct wlr_text_input_manager_v3 *text_input_mgr;
extern struct wlr_text_input_v3 *active_text_input;
extern struct wl_list text_inputs;
extern struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
extern struct wlr_output_power_manager_v1 *power_mgr;
extern struct wlr_pointer_constraints_v1 *pointer_constraints;
extern struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
extern struct wlr_pointer_constraint_v1 *active_constraint;
extern struct wlr_cursor *cursor;
extern struct wlr_xcursor_manager *cursor_mgr;
extern struct wlr_scene_rect *root_bg;
extern struct wlr_session_lock_manager_v1 *session_lock_mgr;
extern struct wlr_scene_rect *locked_bg;
extern struct wlr_session_lock_v1 *cur_lock;
extern struct wlr_seat *seat;
extern KeyboardGroup *kb_group;
extern unsigned int cursor_mode;
extern Client *grabc;
extern int grabcx, grabcy;
extern int resize_dir_x, resize_dir_y;
extern double resize_start_ratio_v, resize_start_ratio_h;
extern int resize_use_v, resize_use_h;
extern uint32_t resize_last_time;
extern double resize_last_x, resize_last_y;
extern struct wlr_output_layout *output_layout;
extern struct wlr_box sgeom;
extern struct wl_list mons;
extern Monitor *selmon;
extern struct wlr_box resize_start_box_v;
extern struct wlr_box resize_start_box_h;
extern struct wlr_box resize_start_box_f;
extern double resize_start_x, resize_start_y;
extern LayoutNode *resize_split_node;
extern LayoutNode *resize_split_node_h;
extern uint64_t joystick_nav_last_move;
extern int joystick_nav_repeat_started;
extern int fullscreen_adaptive_sync_enabled;
extern int fps_limit_enabled;
extern int fps_limit_value;
extern int game_mode_active;
extern int game_mode_ultra;
extern Client *game_mode_client;
extern pid_t game_mode_pid;
extern int game_mode_nice_applied;
extern int game_mode_ioclass_applied;
extern int htpc_mode_active;

/* config hot-reload */
extern int config_inotify_fd;
extern int config_watch_wd;
extern char config_path_cached[PATH_MAX];
extern struct wl_event_source *config_watch_source;
extern struct wl_event_source *config_rewatch_timer;
extern int config_needs_rewatch;

/* status timers */
extern struct wl_event_source *status_timer;
extern struct wl_event_source *status_cpu_timer;
extern struct wl_event_source *status_hover_timer;
extern struct wl_event_source *cache_update_timer;
extern struct wl_event_source *nixpkgs_cache_timer;
extern int cache_update_phase;
extern StatusRefreshTask status_tasks[STATUS_TASKS_COUNT];
extern int status_rng_seeded;

/* gamepad */
extern struct wl_list gamepads;
extern int gamepad_inotify_fd;
extern int gamepad_inotify_wd;
extern struct wl_event_source *gamepad_inotify_event;
extern struct wl_event_source *gamepad_inactivity_timer;
extern char gamepad_pending_paths[][128];
extern int gamepad_pending_count;
extern struct wl_event_source *gamepad_pending_timer;
extern struct wl_event_source *gamepad_cursor_timer;

/* HTPC */
extern int htpc_page_pcgaming;
extern int htpc_page_retrogaming;
extern struct wl_event_source *retro_anim_timer;
extern const char *dgpu_programs[];
extern int htpc_page_movies;
extern int htpc_page_tvshows;
extern int htpc_page_nrk;
extern int htpc_page_netflix;
extern int htpc_page_viaplay;
extern int htpc_page_tv2play;
extern int htpc_page_f1tv;
extern int htpc_page_quit;
extern int client_download_mbps;

/* playback */
extern PlaybackState playback_state;
extern int playback_buffer_seconds;
extern int playback_buffer_progress;
extern char playback_message[512];
extern char playback_url[512];
extern int64_t playback_file_size;
extern int playback_duration;
extern int playback_is_movie;
extern uint64_t playback_start_time;
extern int playback_media_id;
extern int osd_visible;
extern uint64_t osd_show_time;
extern OsdMenuType osd_menu_open;
extern int osd_menu_selection;

/* gaming */
extern int gaming_service_enabled[];
extern const char *gaming_service_names[];
extern const char *retro_console_names[];
extern GpuInfo detected_gpus[];
extern int detected_gpu_count;
extern int discrete_gpu_idx;
extern int integrated_gpu_idx;
extern int pc_gaming_cache_inotify_fd;
extern int pc_gaming_cache_inotify_wd;
extern struct wl_event_source *pc_gaming_cache_event;

/* bluetooth */
extern struct wl_event_source *bt_scan_timer;
extern struct wl_event_source *bt_bus_event;
extern sd_bus *bt_bus;
extern int bt_scanning;
extern const char *bt_gamepad_patterns[];

/* network status */
extern pid_t public_ip_pid;
extern int public_ip_fd;
extern struct wl_event_source *public_ip_event;
extern char public_ip_buf[128];
extern size_t public_ip_len;
extern pid_t ssid_pid;
extern int ssid_fd;
extern struct wl_event_source *ssid_event;
extern char ssid_buf[256];
extern size_t ssid_len;
extern time_t ssid_last_time;

/* tray */
extern int tray_anchor_x;
extern int tray_anchor_y;
extern uint64_t tray_anchor_time_ms;
extern sd_bus *tray_bus;
extern struct wl_event_source *tray_event;
extern sd_bus_slot *tray_vtable_slot;
extern sd_bus_slot *tray_fdo_vtable_slot;
extern sd_bus_slot *tray_name_slot;
extern int tray_host_registered;
extern struct wl_list tray_items;

/* statusbar state */
extern struct CpuSample cpu_prev[];
extern int cpu_prev_count;
extern double cpu_last_percent;
extern char cpu_text[32];
extern double ram_last_mb;
extern char ram_text[32];
extern double battery_last_percent;
extern char battery_text[32];
extern double net_last_down_bps;
extern double net_last_up_bps;
extern char last_clock_render[32];
extern char last_cpu_render[32];
extern char last_ram_render[32];
extern char last_light_render[32];
extern char last_volume_render[32];
extern char last_mic_render[32];
extern char last_battery_render[32];
extern char last_net_render[64];
extern int last_clock_h, last_cpu_h, last_ram_h;
extern int last_light_h, last_volume_h, last_mic_h;
extern int last_battery_h, last_net_h;
extern int battery_path_initialized;
extern int backlight_paths_initialized;
extern uint64_t volume_last_read_speaker_ms;
extern uint64_t volume_last_read_headset_ms;
extern double volume_cached_speaker;
extern double volume_cached_headset;
extern int volume_cached_speaker_muted;
extern int volume_cached_headset_muted;
extern uint64_t mic_last_read_ms;
extern double mic_cached;
extern int mic_cached_muted;
extern uint64_t last_pointer_motion_ms;
extern const float *volume_text_color;
extern const float *mic_text_color;
extern const float *statusbar_fg_override;
extern double light_last_percent;
extern double light_cached_percent;
extern char light_text[32];
extern double mic_last_percent;
extern char mic_text[32];
extern double volume_last_speaker_percent;
extern double volume_last_headset_percent;
extern double speaker_active;
extern double speaker_stored;
extern double microphone_active;
extern double microphone_stored;
extern char volume_text[32];
extern int volume_muted;
extern int mic_muted;
extern int mic_last_color_is_muted;
extern int volume_last_color_is_muted;
extern char backlight_brightness_path[PATH_MAX];
extern char backlight_max_path[PATH_MAX];
extern int backlight_available;
extern int backlight_writable;
extern char battery_capacity_path[PATH_MAX];
extern char battery_device_dir[PATH_MAX];
extern int battery_available;
extern double cpu_last_core_percent[];
extern int cpu_core_count;

/* net */
extern pid_t wifi_scan_pid;
extern int wifi_scan_fd;
extern struct wl_event_source *wifi_scan_event;
extern struct wl_event_source *wifi_scan_timer;
extern struct wl_event_source *cpu_popup_refresh_timer;
extern struct wl_event_source *ram_popup_refresh_timer;
extern struct wl_event_source *popup_delay_timer;
extern struct wl_event_source *video_check_timer;
extern struct wl_event_source *hz_osd_timer;
extern struct wl_event_source *playback_osd_timer;
extern struct wlr_scene_tree *playback_osd_tree;
extern struct wl_event_source *pc_gaming_install_timer;
extern struct wl_event_source *game_refocus_timer;
extern struct wl_event_source *media_view_poll_timer;
extern struct wl_event_source *osk_dpad_repeat_timer;
extern int osk_dpad_held_button;
extern Monitor *osk_dpad_held_mon;
extern Client *game_refocus_client;
extern char wifi_scan_buf[8192];
extern size_t wifi_scan_len;
extern int wifi_scan_inflight;
extern unsigned int wifi_networks_generation;
extern unsigned int wifi_scan_generation;
extern int wifi_networks_accept_updates;
extern int wifi_networks_freeze_existing;
extern struct wl_list wifi_networks;
extern int wifi_networks_initialized;
extern struct wl_list vpn_connections;
extern int vpn_list_initialized;
extern pid_t vpn_scan_pid;
extern int vpn_scan_fd;
extern struct wl_event_source *vpn_scan_event;
extern char vpn_scan_buf[8192];
extern size_t vpn_scan_len;
extern int vpn_scan_inflight;
extern pid_t vpn_connect_pid;
extern int vpn_connect_fd;
extern struct wl_event_source *vpn_connect_event;
extern char vpn_connect_buf[4096];
extern size_t vpn_connect_len;
extern char vpn_pending_name[128];
extern char net_text[64];
extern char net_local_ip[64];
extern char net_public_ip[64];
extern char net_down_text[32];
extern char net_up_text[32];
extern char net_ssid[64];
extern double net_last_wifi_quality;
extern int net_link_speed_mbps;
extern char net_iface[64];
extern char net_prev_iface[64];
extern int net_is_wireless;
extern int net_available;

/* icon paths + buffers */
extern char net_icon_path[PATH_MAX];
extern char net_icon_loaded_path[PATH_MAX];
extern char cpu_icon_path[PATH_MAX];
extern char cpu_icon_loaded_path[PATH_MAX];
extern char light_icon_path[PATH_MAX];
extern char light_icon_loaded_path[PATH_MAX];
extern char ram_icon_path[PATH_MAX];
extern char ram_icon_loaded_path[PATH_MAX];
extern char battery_icon_path[PATH_MAX];
extern char battery_icon_loaded_path[PATH_MAX];
extern char mic_icon_path[PATH_MAX];
extern char mic_icon_loaded_path[PATH_MAX];
extern char volume_icon_path[PATH_MAX];
extern char volume_icon_loaded_path[PATH_MAX];
extern char clock_icon_path[PATH_MAX];
extern char clock_icon_loaded_path[PATH_MAX];
extern char bluetooth_icon_path[PATH_MAX];
extern char bluetooth_icon_loaded_path[PATH_MAX];
extern char steam_icon_path[PATH_MAX];
extern char steam_icon_loaded_path[PATH_MAX];
extern char discord_icon_path[PATH_MAX];
extern char discord_icon_loaded_path[PATH_MAX];
extern int net_icon_loaded_h, net_icon_w, net_icon_h;
extern struct wlr_buffer *net_icon_buf;
extern int clock_icon_loaded_h, clock_icon_w, clock_icon_h;
extern struct wlr_buffer *clock_icon_buf;
extern int cpu_icon_loaded_h, cpu_icon_w, cpu_icon_h;
extern struct wlr_buffer *cpu_icon_buf;
extern int light_icon_loaded_h, light_icon_w, light_icon_h;
extern struct wlr_buffer *light_icon_buf;
extern int ram_icon_loaded_h, ram_icon_w, ram_icon_h;
extern struct wlr_buffer *ram_icon_buf;
extern int battery_icon_loaded_h, battery_icon_w, battery_icon_h;
extern struct wlr_buffer *battery_icon_buf;
extern int mic_icon_loaded_h, mic_icon_w, mic_icon_h;
extern struct wlr_buffer *mic_icon_buf;
extern int volume_icon_loaded_h, volume_icon_w, volume_icon_h;
extern struct wlr_buffer *volume_icon_buf;
extern int bluetooth_icon_loaded_h, bluetooth_icon_w, bluetooth_icon_h;
extern struct wlr_buffer *bluetooth_icon_buf;
extern int bluetooth_available;
extern int steam_icon_loaded_h, steam_icon_w, steam_icon_h;
extern struct wlr_buffer *steam_icon_buf;
extern int steam_running;
extern int discord_icon_loaded_h, discord_icon_w, discord_icon_h;
extern struct wlr_buffer *discord_icon_buf;
extern int discord_running;
extern char fan_icon_path[PATH_MAX];
extern char fan_icon_loaded_path[PATH_MAX];
extern int fan_icon_loaded_h, fan_icon_w, fan_icon_h;
extern struct wlr_buffer *fan_icon_buf;
extern char fan_text[32];
extern char last_fan_render[32];
extern int last_fan_h;
extern unsigned long long net_prev_rx;
extern unsigned long long net_prev_tx;
extern struct timespec net_prev_ts;
extern int net_prev_valid;
extern time_t net_public_ip_last;

/* icon path string constants */
extern const char net_icon_no_conn[];
extern const char net_icon_eth[];
extern const char net_icon_wifi_100[];
extern const char net_icon_wifi_75[];
extern const char net_icon_wifi_50[];
extern const char net_icon_wifi_25[];
extern const char battery_icon_25[];
extern const char battery_icon_50[];
extern const char battery_icon_75[];
extern const char battery_icon_100[];
extern const char volume_icon_speaker_25[];
extern const char volume_icon_speaker_50[];
extern const char volume_icon_speaker_100[];
extern const char volume_icon_speaker_muted[];
extern const char volume_icon_headset[];
extern const char volume_icon_headset_muted[];
extern const char mic_icon_unmuted[];
extern const char mic_icon_muted[];
extern char net_icon_wifi_100_resolved[PATH_MAX];
extern char net_icon_wifi_75_resolved[PATH_MAX];
extern char net_icon_wifi_50_resolved[PATH_MAX];
extern char net_icon_wifi_25_resolved[PATH_MAX];
extern char net_icon_eth_resolved[PATH_MAX];
extern char net_icon_no_conn_resolved[PATH_MAX];

/* misc */
extern const double light_step;
extern const double volume_step;
extern const double volume_max_percent;
extern const double mic_step;
extern const double mic_max_percent;
extern char sysicons_text[64];
#define DESKTOP_ENTRIES_MAX 4096
extern DesktopEntry desktop_entries[DESKTOP_ENTRIES_MAX];
extern int desktop_entry_count;
extern int desktop_entries_loaded;
extern NixpkgEntry nixpkg_entries[];
extern int nixpkg_entry_count;
extern int nixpkg_entries_loaded;
extern char nixpkgs_cache_path[PATH_MAX];
extern struct wlr_buffer *nixpkg_ok_icon_buf;
extern int nixpkg_ok_icon_height;
extern const uint32_t cpu_popup_refresh_interval_ms;
extern const uint32_t ram_popup_refresh_interval_ms;
extern const float net_menu_row_bg[];
extern const float net_menu_row_bg_hover[];
extern const double status_icon_scale;

/* runtime fonts */
extern char *runtime_fonts[];
extern int runtime_fonts_set;

/* HTPC menu */
struct HtpcMenuItem { char label[64]; char command[256]; };
extern struct HtpcMenuItem htpc_menu_items[];
extern int htpc_menu_item_count;

/* Audio/subtitle tracks for OSD */
struct TrackInfo { int id; char title[128]; char lang[16]; int selected; };
extern struct TrackInfo audio_tracks[], subtitle_tracks[];
extern int audio_track_count;
extern int subtitle_track_count;
extern ResumeEntry resume_cache[];
extern int resume_cache_count;

/* Global event handlers */
extern struct wl_listener cursor_axis;
extern struct wl_listener cursor_button;
extern struct wl_listener cursor_frame;
extern struct wl_listener cursor_motion;
extern struct wl_listener cursor_motion_absolute;
extern struct wl_listener gpu_reset;
extern struct wl_listener layout_change;
extern struct wl_listener new_idle_inhibitor;
extern struct wl_listener new_input_device;
extern struct wl_listener new_virtual_keyboard;
extern struct wl_listener new_virtual_pointer;
extern struct wl_listener new_text_input;
extern struct wl_listener new_pointer_constraint;
extern struct wl_listener new_output;
extern struct wl_listener new_xdg_toplevel;
extern struct wl_listener new_xdg_popup;
extern struct wl_listener new_xdg_decoration;
extern struct wl_listener new_layer_surface;
extern struct wl_listener output_mgr_apply;
extern struct wl_listener output_mgr_test;
extern struct wl_listener output_power_mgr_set_mode;
extern struct wl_listener request_activate;
extern struct wl_listener request_cursor;
extern struct wl_listener request_set_psel;
extern struct wl_listener request_set_sel;
extern struct wl_listener request_set_cursor_shape;
extern struct wl_listener request_start_drag;
extern struct wl_listener start_drag;
extern struct wl_listener new_session_lock;

#ifdef XWAYLAND
extern struct wl_listener new_xwayland_surface;
extern struct wl_listener xwayland_ready;
extern struct wlr_xwayland *xwayland;
#endif

/* layermap */
extern const int layermap[];

/* ── function declarations ─────────────────────────────────────────── */

/* draw.c */
void drawrect(struct wlr_scene_tree *parent, int x, int y,
		int width, int height, const float color[static 4]);
void drawhoverrect(struct wlr_scene_tree *parent, int x, int y,
		int width, int height, const float color[static 4], float fade);
void draw_border(struct wlr_scene_tree *parent, int x, int y,
		int w, int h, int thickness, const float color[static 4]);
void drawroundedrect(struct wlr_scene_tree *parent, int x, int y,
		int width, int height, const float color[static 4]);
struct wlr_buffer *statusbar_buffer_from_argb32(const uint32_t *data, int width, int height);
struct wlr_buffer *statusbar_buffer_from_argb32_raw(const uint32_t *data, int width, int height);
struct wlr_buffer *statusbar_scaled_buffer_from_argb32(const uint32_t *data,
		int width, int height, int target_h);
struct wlr_buffer *statusbar_scaled_buffer_from_argb32_raw(const uint32_t *data,
		int width, int height, int target_h);
struct wlr_buffer *statusbar_buffer_from_glyph(const struct fcft_glyph *glyph);
struct wlr_buffer *statusbar_buffer_from_pixbuf(GdkPixbuf *pixbuf, int target_h, int *out_w, int *out_h);
struct wlr_buffer *statusbar_buffer_from_wifi100(int target_h, int *out_w, int *out_h);
void recolor_wifi100_pixbuf(GdkPixbuf *pixbuf);
int tray_load_svg_pixbuf(const char *path, int desired_h, GdkPixbuf **out_pixbuf);
int has_svg_extension(const char *path);
int pathisdir(const char *path);
int strip_symbolic_suffix(const char *name, char *out, size_t outlen);
void tray_consider_icon(const char *path, int size_hint, int desired_h,
	char *best_path, int *best_diff, int *found);
void add_icon_root_paths(const char *base, const char *themes[], size_t theme_count,
	char pathbufs[][PATH_MAX], size_t *pathcount, size_t max_paths);
int loadstatusfont(void);
void freestatusfont(void);
int status_text_width(const char *text);
int resolve_asset_path(const char *path, char *out, size_t len);
void fix_tray_argb32(uint32_t *pixels, size_t count, int use_rgba_order);

/* client.c */
void applybounds(Client *c, struct wlr_box *bbox);
void applyrules(Client *c);
void createnotify(struct wl_listener *listener, void *data);
void destroynotify(struct wl_listener *listener, void *data);
void mapnotify(struct wl_listener *listener, void *data);
void unmapnotify(struct wl_listener *listener, void *data);
void focusclient(Client *c, int lift);
Client *focustop(Monitor *m);
void focusstack(const Arg *arg);
void focusdir(const Arg *arg);
void focusmon(const Arg *arg);
void warptomonitor(const Arg *arg);
void tagtomonitornum(const Arg *arg);
void setmon(Client *c, Monitor *m, uint32_t newtags);
void killclient(const Arg *arg);
void setfloating(Client *c, int floating);
void setfullscreen(Client *c, int fullscreen);
void setsticky(Client *c, int sticky);
void togglefloating(const Arg *arg);
void togglefullscreen(const Arg *arg);
void togglefullscreenadaptivesync(const Arg *arg);
void togglesticky(const Arg *arg);
void updatetitle(struct wl_listener *listener, void *data);
void urgent(struct wl_listener *listener, void *data);
void pointerfocus(Client *c, struct wlr_surface *surface,
		double sx, double sy, uint32_t time);
void warpcursor(const Client *c);
Client *find_client_by_app_id(const char *app_id);
Client *find_discord_client(void);
void focus_or_launch_app(const char *app_id, const char *launch_cmd);
void commitnotify(struct wl_listener *listener, void *data);
void createdecoration(struct wl_listener *listener, void *data);
void destroydecoration(struct wl_listener *listener, void *data);
void requestdecorationmode(struct wl_listener *listener, void *data);
void maximizenotify(struct wl_listener *listener, void *data);
void fullscreennotify(struct wl_listener *listener, void *data);
void setpsel(struct wl_listener *listener, void *data);
void setsel(struct wl_listener *listener, void *data);
void resize(Client *c, struct wlr_box geo, int interact);
void tag(const Arg *arg);
void tagmon(const Arg *arg);
void toggletag(const Arg *arg);
void toggleview(const Arg *arg);
void view(const Arg *arg);
void zoom(const Arg *arg);
void rotate_clients(const Arg *arg);
void moveresize(const Arg *arg);
int is_video_content(Client *c);
int is_game_content(Client *c);
int is_steam_client(Client *c);
int is_steam_popup(Client *c);
int is_steam_game(Client *c);
int is_browser_client(Client *c);
int looks_like_game(Client *c);
int is_steam_cmd(const char *cmd);
int is_steam_child_process(pid_t pid);
int client_wants_tearing(Client *c);
void track_client_frame(Client *c);
float detect_video_framerate(Client *c);
int any_client_fullscreen(void);
Client *get_fullscreen_client(void);
int is_process_running(const char *name);

/* layout.c */
void arrange(Monitor *m);
int htpc_view_is_active(Monitor *m, unsigned int view_tag, int visible);
void htpc_views_update_visibility(Monitor *m);
void arrangelayer(Monitor *m, struct wl_list *list,
		struct wlr_box *usable_area, int exclusive);
void arrangelayers(Monitor *m);
void tile(Monitor *m);
void monocle(Monitor *m);
void btrtile(Monitor *m);
void setlayout(const Arg *arg);
void setmfact(const Arg *arg);
void incnmaster(const Arg *arg);
void setratio_h(const Arg *arg);
void setratio_v(const Arg *arg);
void swapclients(const Arg *arg);
void togglegaps(const Arg *arg);
void hidetagthumbnail(Monitor *m);

/* input.c */
void axisnotify(struct wl_listener *listener, void *data);
void buttonpress(struct wl_listener *listener, void *data);
void handle_pointer_button_internal(uint32_t button, uint32_t state, uint32_t time_msec);
void chvt(const Arg *arg);
void createkeyboard(struct wlr_keyboard *keyboard);
KeyboardGroup *createkeyboardgroup(void);
void destroykeyboardgroup(struct wl_listener *listener, void *data);
void inputdevice(struct wl_listener *listener, void *data);
int keybinding(uint32_t mods, xkb_keysym_t sym);
void keypress(struct wl_listener *listener, void *data);
void keypressmod(struct wl_listener *listener, void *data);
int keyrepeat(void *data);
void createpointer(struct wlr_pointer *pointer);
void createpointerconstraint(struct wl_listener *listener, void *data);
void destroypointerconstraint(struct wl_listener *listener, void *data);
void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
void cursorframe(struct wl_listener *listener, void *data);
void cursorwarptohint(void);
void motionabsolute(struct wl_listener *listener, void *data);
void motionnotify(uint32_t time, struct wlr_input_device *device, double sx,
		double sy, double sx_unaccel, double sy_unaccel);
void motionrelative(struct wl_listener *listener, void *data);
void setcursor(struct wl_listener *listener, void *data);
void setcursorshape(struct wl_listener *listener, void *data);
void requeststartdrag(struct wl_listener *listener, void *data);
void startdrag(struct wl_listener *listener, void *data);
void destroydragicon(struct wl_listener *listener, void *data);
void virtualkeyboard(struct wl_listener *listener, void *data);
void virtualpointer(struct wl_listener *listener, void *data);
void textinput(struct wl_listener *listener, void *data);
void text_input_focus_change(struct wlr_surface *old, struct wlr_surface *new);
const char *pick_resize_handle(const Client *c, double cx, double cy);
const char *resize_cursor_from_dirs(int dx, int dy);
LayoutNode *closest_split_node(LayoutNode *client_node, int want_vert,
		double pointer, double *out_ratio, struct wlr_box *out_box,
		double *out_dist);
void apply_resize_axis_choice(void);
int resize_should_update(uint32_t time);
void applyxkbdefaultsfromsystem(struct xkb_rule_names *names);
struct xkb_rule_names getxkbrules(void);
int handlestatusscroll(struct wlr_pointer_axis_event *event);
int scrollsteps(const struct wlr_pointer_axis_event *event);

/* output.c */
void createmon(struct wl_listener *listener, void *data);
void cleanupmon(struct wl_listener *listener, void *data);
void closemon(Monitor *m);
Monitor *dirtomon(enum wlr_direction dir);
Monitor *xytomon(double x, double y);
void xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny);
void rendermon(struct wl_listener *listener, void *data);
void outputpresent(struct wl_listener *listener, void *data);
void outputmgrapply(struct wl_listener *listener, void *data);
void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);
void outputmgrtest(struct wl_listener *listener, void *data);
void powermgrsetmode(struct wl_listener *listener, void *data);
void gpureset(struct wl_listener *listener, void *data);
void requestmonstate(struct wl_listener *listener, void *data);
void updatemons(struct wl_listener *listener, void *data);
void set_adaptive_sync(Monitor *m, int enabled);
void set_video_refresh_rate(Monitor *m, Client *c);
void restore_max_refresh_rate(Monitor *m);
int detect_10bit_support(Monitor *m);
int set_drm_color_properties(Monitor *m, int max_bpc);
int enable_10bit_rendering(Monitor *m);
void init_monitor_color_settings(Monitor *m);
void update_game_vrr(Monitor *m, float current_fps);
void enable_game_vrr(Monitor *m);
void disable_game_vrr(Monitor *m);
void check_fullscreen_video(void);
void schedule_video_check(uint32_t ms);
int enable_vrr_video_mode(Monitor *m, float video_hz);
void disable_vrr_video_mode(Monitor *m);
int set_custom_video_mode(Monitor *m, float exact_hz);

typedef struct {
	int method;
	struct wlr_output_mode *mode;
	int multiplier;
	float target_hz;
	float actual_hz;
	float score;
	float judder_ms;
} VideoModeCandidate;
VideoModeCandidate find_best_video_mode(Monitor *m, float video_hz);
float score_video_mode(int method, float video_hz, float display_hz, int multiplier);
float calculate_judder_ms(float video_hz, float display_hz);
void generate_cvt_mode(drmModeModeInfo *mode, int hdisplay, int vdisplay, float vrefresh);
void show_hz_osd(Monitor *m, const char *msg);
void hide_hz_osd(Monitor *m);
int hz_osd_timeout(void *data);
void testhzosd(const Arg *arg);
void setcustomhz(const Arg *arg);
void render_playback_osd(void);
void hide_playback_osd(void);
int playback_osd_timeout(void *data);
struct wlr_output_mode *bestmode(struct wlr_output *output);
RuntimeMonitorConfig *find_monitor_config(const char *name);
void calculate_monitor_position(Monitor *m, RuntimeMonitorConfig *cfg, int *out_x, int *out_y);

/* statusbar.c */
void initstatusbar(Monitor *m);
void layoutstatusbar(Monitor *m, const struct wlr_box *area,
		struct wlr_box *client_area);
void positionstatusmodules(Monitor *m);
void printstatus(void);
void renderclock(StatusModule *module, int bar_height, const char *text);
void renderlight(StatusModule *module, int bar_height, const char *text);
void rendernet(StatusModule *module, int bar_height, const char *text);
void renderbattery(StatusModule *module, int bar_height, const char *text);
void rendervolume(StatusModule *module, int bar_height, const char *text);
void rendermic(StatusModule *module, int bar_height, const char *text);
void rendercpu(StatusModule *module, int bar_height, const char *text);
void renderram(StatusModule *module, int bar_height, const char *text);
void render_icon_label(StatusModule *module, int bar_height, const char *text,
		int (*ensure_icon)(int target_h), struct wlr_buffer **icon_buf,
		int *icon_w, int *icon_h, int min_text_w, int icon_gap,
		const float text_color[static 4]);
void renderworkspaces(Monitor *m, StatusModule *module, int bar_height);
int tray_render_label(StatusModule *module, const char *text, int x, int bar_height,
		const float color[static 4]);
void rendertrayicons(Monitor *m, int bar_height);
void refreshstatusclock(void);
void refreshstatuslight(void);
void refreshstatusvolume(void);
void refreshstatusmic(void);
void refreshstatusbattery(void);
void refreshstatusnet(void);
void refreshstatuscpu(void);
void refreshstatusram(void);
void refreshstatusicons(void);
void refreshstatusfan(void);
void refreshstatustags(void);
void init_status_refresh_tasks(void);
void seed_status_rng(void);
uint32_t random_status_delay_ms(void);
double volume_last_for_type(int is_headset);
void volume_cache_store(int is_headset, double level, int muted, uint64_t now);
int status_should_render(StatusModule *module, int barh, const char *text,
		char *last_text, size_t last_len, int *last_h);
void initial_status_refresh(void);
void schedule_status_timer(void);
void schedule_next_status_refresh(void);
int status_task_hover_active(void (*fn)(void));
void trigger_status_task_now(void (*fn)(void));
void set_status_task_due(void (*fn)(void), uint64_t due_ms);
void schedule_hover_timer(void);
void togglestatusbar(const Arg *arg);
int ensure_cpu_icon_buffer(int target_h);
void drop_cpu_icon_buffer(void);
int ensure_light_icon_buffer(int target_h);
void drop_light_icon_buffer(void);
int ensure_ram_icon_buffer(int target_h);
void drop_ram_icon_buffer(void);
int ensure_battery_icon_buffer(int target_h);
void drop_battery_icon_buffer(void);
int ensure_clock_icon_buffer(int target_h);
void drop_clock_icon_buffer(void);
int ensure_mic_icon_buffer(int target_h);
void drop_mic_icon_buffer(void);
void drop_net_icon_buffer(void);
void drop_bluetooth_icon_buffer(void);
void drop_steam_icon_buffer(void);
void drop_discord_icon_buffer(void);
void init_net_icon_paths(void);
int ensure_volume_icon_buffer(int target_h);
void drop_volume_icon_buffer(void);
void rendercpupopup(Monitor *m);
void renderrampopup(Monitor *m);
void renderbatterypopup(Monitor *m);
int cpu_popup_clamped_x(Monitor *m, CpuPopup *p);
int cpu_popup_hover_index(Monitor *m, CpuPopup *p);
int cpu_proc_cmp(const void *a, const void *b);
int kill_processes_with_name(const char *name);
int cpu_proc_is_critical(pid_t pid, const char *name);
int read_top_cpu_processes(CpuPopup *p);
int cpu_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button);
int ram_popup_clamped_x(Monitor *m, RamPopup *p);
int ram_popup_hover_index(Monitor *m, RamPopup *p);
int ram_proc_cmp(const void *a, const void *b);
int read_top_ram_processes(RamPopup *p);
int ram_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button);
void read_battery_info(BatteryPopup *p);
int battery_popup_clamped_x(Monitor *m, BatteryPopup *p);
void updatetaghover(Monitor *m, double cx, double cy);
void updatenethover(Monitor *m, double cx, double cy);
void updatecpuhover(Monitor *m, double cx, double cy);
void updateramhover(Monitor *m, double cx, double cy);
void updatebatteryhover(Monitor *m, double cx, double cy);

/* fancontrol.c */
void fan_scan_hwmon(FanPopup *p);
void fan_read_all(FanPopup *p);
void fan_write_pwm(FanEntry *f, int pwm);
void fan_set_manual(FanEntry *f);
void fan_set_auto(FanEntry *f);
void renderfanpopup(Monitor *m);
int fan_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button);
void fan_popup_handle_drag(Monitor *m, double cx, double cy);
void updatefanhover(Monitor *m, double cx, double cy);
void renderfan(StatusModule *module, int bar_height, const char *text);
int ensure_fan_icon_buffer(int target_h);
void drop_fan_icon_buffer(void);
int updatestatuscpu(void *data);
int updatestatusclock(void *data);
int updatehoverfade(void *data);
int readcpustats(struct CpuSample *out, int maxcount);
int readmeminfo(unsigned long long *total_kb, unsigned long long *avail_kb);
int set_backlight_percent(double percent);
int set_backlight_relative(double delta_percent);
int set_pipewire_volume(double percent);
int set_pipewire_mute(int mute);
int toggle_pipewire_mute(void);
int pipewire_sink_is_headset(void);
int set_pipewire_mic_volume(double percent);
int set_pipewire_mic_mute(int mute);
int toggle_pipewire_mic_mute(void);
double pipewire_mic_volume_percent(void);
double pipewire_volume_percent(int *is_headset_out);
void volume_invalidate_cache(int is_headset);
double net_bytes_to_rate(unsigned long long cur, unsigned long long prev,
		double elapsed);
double ramused_mb(void);
double cpuaverage(void);
double battery_percent(void);
double backlight_percent(void);
int findbacklightdevice(char *brightness_path, size_t brightness_len,
		char *max_path, size_t max_len);
int findbatterydevice(char *capacity_path, size_t capacity_len);
int findbluetoothdevice(void);
int cpu_popup_refresh_timeout(void *data);
void schedule_cpu_popup_refresh(uint32_t ms);
int ram_popup_refresh_timeout(void *data);
void schedule_ram_popup_refresh(uint32_t ms);
int popup_delay_timeout(void *data);
void schedule_popup_delay(uint32_t ms);

/* tray.c */
int tray_bus_event(int fd, uint32_t mask, void *data);
int tray_method_register_item(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
int tray_method_register_host(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
int tray_property_get_registered(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);
int tray_property_get_host_registered(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);
int tray_property_get_protocol_version(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);
int tray_item_load_icon(TrayItem *it);
void tray_emit_host_registered(void);
void tray_menu_clear(TrayMenu *menu);
void tray_menu_hide(Monitor *m);
void tray_menu_hide_all(void);
int tray_item_get_menu_path(TrayItem *it);
int tray_menu_open_at(Monitor *m, TrayItem *it, int icon_x);
void tray_menu_render(Monitor *m);
void tray_menu_draw_text(struct wlr_scene_tree *tree, const char *text, int x, int y, int row_h);
TrayMenuEntry *tray_menu_entry_at(Monitor *m, int lx, int ly);
int tray_menu_send_event(TrayMenu *menu, TrayMenuEntry *entry, uint32_t time_msec);
int tray_menu_parse_node(sd_bus_message *msg, TrayMenu *menu, int depth, int max_depth);
int tray_menu_parse_node_body(sd_bus_message *msg, TrayMenu *menu, int depth, int max_depth);
void tray_sanitize_label(const char *src, char *dst, size_t len);
int tray_search_item_path(const char *service, const char *start_path,
		char *out, size_t outlen, int depth);
int tray_find_item_path(const char *service, char *path, size_t pathlen);
void tray_add_item(const char *service, const char *path, int emit_signals);
void tray_scan_existing_items(void);
void tray_update_icons_text(void);
void tray_remove_item(const char *service);
int tray_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
void tray_init(void);
TrayItem *tray_first_item(void);
void tray_item_activate(TrayItem *it, int button, int context_menu, int x, int y);

/* network.c */
void net_menu_hide_all(void);
void net_menu_open(Monitor *m);
void net_menu_render(Monitor *m);
void net_menu_submenu_render(Monitor *m);
void net_menu_update_hover(Monitor *m, double cx, double cy);
void wifi_networks_clear(void);
void request_wifi_scan(void);
void wifi_scan_plan_rescan(void);
int wifi_scan_event_cb(int fd, uint32_t mask, void *data);
int wifi_scan_timer_cb(void *data);
void wifi_scan_finish(void);
void connect_wifi_ssid(const char *ssid);
void transfer_status_menus(Monitor *from, Monitor *to);
int net_menu_handle_click(Monitor *m, int lx, int ly, uint32_t button);
void vpn_connections_clear(void);
VpnConnection *vpn_connection_at_index(int idx);
WifiNetwork *wifi_network_at_index(int idx);
int vpn_scan_event_cb(int fd, uint32_t mask, void *data);
void vpn_connect(const char *name);
int vpn_connect_event_cb(int fd, uint32_t mask, void *data);
void connect_wifi_with_prompt(const char *ssid, int secure);
void request_public_ip_async_ex(int force);
void request_public_ip_async(void);
void stop_public_ip_fetch(void);
void request_ssid_async(const char *iface);
void stop_ssid_fetch(void);
int public_ip_event_cb(int fd, uint32_t mask, void *data);
int ssid_event_cb(int fd, uint32_t mask, void *data);

/* popup.c */
void wifi_popup_show(Monitor *m, const char *ssid);
void wifi_popup_hide(Monitor *m);
void wifi_popup_hide_all(void);
void wifi_popup_render(Monitor *m);
int wifi_popup_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym);
int wifi_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button);
Monitor *wifi_popup_visible_monitor(void);
void wifi_popup_connect(Monitor *m);
void wifi_try_saved_connect(Monitor *m, const char *ssid);
int wifi_popup_connect_cb(int fd, uint32_t mask, void *data);
void sudo_popup_show(Monitor *m, const char *title, const char *cmd, const char *pkg_name);
void sudo_popup_hide(Monitor *m);
void sudo_popup_hide_all(void);
void sudo_popup_render(Monitor *m);
int sudo_popup_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym);
Monitor *sudo_popup_visible_monitor(void);
void sudo_popup_execute(Monitor *m);
int sudo_popup_cb(int fd, uint32_t mask, void *data);
int sudo_popup_wait_timer(void *data);
void osk_show(Monitor *m, struct wlr_surface *target);
void osk_hide(Monitor *m);
void osk_hide_all(void);
void osk_render(Monitor *m);
int osk_handle_button(Monitor *m, int button, int value);
void osk_dpad_repeat_start(Monitor *m, int button);
void osk_dpad_repeat_stop(void);
Monitor *osk_visible_monitor(void);
void osk_send_key(Monitor *m);
void osk_send_backspace(Monitor *m);
void osk_send_text(const char *text);
int toast_hide_timer(void *data);
void toast_show(Monitor *m, const char *message, int duration_ms);

/* launcher.c */
void modal_show(const Arg *arg);
void modal_show_files(const Arg *arg);
void modal_show_git(const Arg *arg);
void modal_render(Monitor *m);
void modal_render_results(Monitor *m);
void modal_update_selection(Monitor *m);
void modal_hide(Monitor *m);
void modal_hide_all(void);
Monitor *modal_visible_monitor(void);
void modal_layout_metrics(int *btn_h, int *field_h, int *line_h, int *pad);
int modal_max_visible_lines(Monitor *m);
void modal_ensure_selection_visible(Monitor *m);
int modal_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym);
void modal_file_search_clear_results(Monitor *m);
void modal_file_search_stop(Monitor *m);
void modal_file_search_start_mode(Monitor *m, int fallback);
void modal_file_search_start(Monitor *m);
int modal_file_search_event(int fd, uint32_t mask, void *data);
int modal_file_search_debounce_cb(void *data);
void modal_file_search_schedule(Monitor *m);
void modal_git_search_clear_results(Monitor *m);
void modal_git_search_stop(Monitor *m);
void modal_git_search_start(Monitor *m);
int modal_git_search_event(int fd, uint32_t mask, void *data);
void git_cache_update_start(void);
void file_cache_update_start(void);
void modal_truncate_to_width(const char *src, char *dst, size_t len, int max_px);
void shorten_path_display(const char *full, char *out, size_t len);
int desktop_entry_cmp_used(const void *a, const void *b);
int modal_match_name(const char *haystack, const char *needle);
void modal_update_results(Monitor *m);
void modal_render_search_field(Monitor *m);
int modal_render_timer_cb(void *data);
void modal_schedule_render(Monitor *m);
void modal_prewarm(Monitor *m);
void ensure_desktop_entries_loaded(void);

/* nixpkgs.c */
void nixpkgs_show(const Arg *arg);
void nixpkgs_hide(Monitor *m);
void nixpkgs_hide_all(void);
Monitor *nixpkgs_visible_monitor(void);
void nixpkgs_render(Monitor *m);
void nixpkgs_render_results(Monitor *m);
void nixpkgs_update_selection(Monitor *m);
void nixpkgs_update_results(Monitor *m);
int nixpkgs_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym);
void nixpkgs_install_selected(Monitor *m);
void load_nixpkgs_cache(void);
void load_installed_packages(void);
void ensure_nixpkgs_cache_loaded(void);
int ensure_nixpkg_ok_icon(int height);
void nixpkgs_cache_update_start(void);
int nixpkgs_cache_timer_cb(void *data);
void schedule_nixpkgs_cache_timer(void);
int cache_update_timer_cb(void *data);
void schedule_cache_update_timer(void);

/* gaming.c */
GameEntry *pc_gaming_merge_sorted(GameEntry *a, GameEntry *b);
void pc_gaming_show(Monitor *m);
void pc_gaming_hide(Monitor *m);
void pc_gaming_hide_all(void);
Monitor *pc_gaming_visible_monitor(void);
void pc_gaming_render(Monitor *m);
void pc_gaming_refresh_games(Monitor *m);
void pc_gaming_free_games(Monitor *m);
void pc_gaming_scan_steam(Monitor *m);
void pc_gaming_scan_heroic(Monitor *m);
int pc_gaming_handle_button(Monitor *m, int button, int value);
int pc_gaming_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym);
void pc_gaming_launch_game(Monitor *m);
void pc_gaming_scroll(Monitor *m, int delta);
void pc_gaming_install_popup_show(Monitor *m, GameEntry *g);
void pc_gaming_install_popup_hide(Monitor *m);
void pc_gaming_install_popup_render(Monitor *m);
int pc_gaming_install_popup_handle_button(Monitor *m, int button, int value);
void pc_gaming_load_game_icon(GameEntry *g, int target_w, int target_h);
void pc_gaming_cache_update_start(void);
int pc_gaming_cache_inotify_cb(int fd, uint32_t mask, void *data);
void pc_gaming_cache_watch_setup(void);
void retro_gaming_show(Monitor *m);
void retro_gaming_hide(Monitor *m);
void retro_gaming_hide_all(void);
Monitor *retro_gaming_visible_monitor(void);
void retro_gaming_render(Monitor *m);
int retro_gaming_handle_button(Monitor *m, int button, int value);
int retro_gaming_animate(void *data);
void detect_gpus(void);
int should_use_dgpu(const char *cmd);
void set_dgpu_env(void);
void set_steam_env(void);

/* media.c */
MediaGridView *media_get_view(Monitor *m, MediaViewType type);
const char *get_media_server_url(void);
int get_all_media_servers(MediaServer **servers);
int parse_media_json(const char *buffer, const char *server_url, MediaItem **out_items, int max_items);
void media_view_show(Monitor *m, MediaViewType type);
void media_view_hide(Monitor *m, MediaViewType type);
void media_view_hide_all(void);
void media_view_render(Monitor *m, MediaViewType type);
void media_view_render_detail(Monitor *m, MediaViewType type);
int media_view_refresh(Monitor *m, MediaViewType type);
int media_view_poll_timer_cb(void *data);
void media_view_free_items(MediaGridView *view);
void media_view_free_seasons(MediaGridView *view);
void media_view_free_episodes(MediaGridView *view);
void media_view_fetch_seasons(MediaGridView *view, const char *show_name);
void media_view_fetch_episodes(MediaGridView *view, const char *show_name, int season);
int json_extract_int(const char *json, const char *key);
int json_extract_string(const char *json, const char *key, char *out, size_t out_size);
float json_extract_float(const char *json, const char *key);
int media_view_handle_button(Monitor *m, MediaViewType type, int button, int value);
int media_view_handle_key(Monitor *m, MediaViewType type, xkb_keysym_t sym);
void media_view_scroll(Monitor *m, MediaViewType type, int delta);
void media_view_load_poster(MediaItem *item, int target_w, int target_h);
Monitor *media_view_visible_monitor(void);

/* gamepad.c */
void gamepad_menu_show(Monitor *m);
void gamepad_menu_hide(Monitor *m);
void gamepad_menu_hide_all(void);
Monitor *gamepad_menu_visible_monitor(void);
void gamepad_menu_render(Monitor *m);
int gamepad_menu_handle_button(Monitor *m, int button, int value);
int gamepad_menu_handle_click(Monitor *m, int cx, int cy, uint32_t button);
void gamepad_menu_select(Monitor *m);
void gamepad_device_add(const char *path);
void gamepad_device_remove(const char *path);
int gamepad_event_cb(int fd, uint32_t mask, void *data);
int gamepad_inotify_cb(int fd, uint32_t mask, void *data);
void gamepad_scan_devices(void);
void gamepad_setup(void);
void gamepad_cleanup(void);
int gamepad_cursor_timer_cb(void *data);
int gamepad_pending_timer_cb(void *data);
int handle_playback_osd_input(int button);
void gamepad_update_cursor(void);
int gamepad_inactivity_timer_cb(void *data);
void gamepad_turn_off_led_sysfs(GamepadDevice *gp);
void gamepad_suspend(GamepadDevice *gp);
void gamepad_resume(GamepadDevice *gp);
int gamepad_any_monitor_active(void);
void gamepad_grab(GamepadDevice *gp);
void gamepad_ungrab(GamepadDevice *gp);
void gamepad_update_grab_state(void);
int gamepad_should_grab(void);

/* bluetooth.c */
int bt_bus_event_cb(int fd, uint32_t mask, void *data);
void bt_controller_setup(void);
void bt_controller_cleanup(void);
int bt_scan_timer_cb(void *data);
void bt_start_discovery(void);
int bt_get_objects_disconnect_cb(sd_bus_message *reply, void *userdata, sd_bus_error *error);
void bt_stop_discovery(void);
int bt_is_gamepad_name(const char *name);
void bt_pair_device(const char *path);
void bt_connect_device(const char *path);
void bt_trust_device(const char *path);
int bt_device_signal_cb(sd_bus_message *m, void *userdata, sd_bus_error *error);

/* htpc.c */
void htpc_mode_enter(void);
void htpc_mode_exit(void);
void htpc_mode_toggle(const Arg *arg);
void htpc_menu_build(void);
void update_game_mode(void);
void steam_set_ge_proton_default(void);
void cec_switch_to_active_source(void);
void steam_launch_bigpicture(void);
void steam_kill(void);
void live_tv_kill(void);
int game_refocus_timer_cb(void *data);
void schedule_game_refocus(Client *c, uint32_t ms);
void gamepanel(const Arg *arg);
Monitor *stats_panel_visible_monitor(void);
int stats_panel_handle_key(Monitor *m, xkb_keysym_t sym);

/* config.c */
void load_config(void);
void reload_config(void);
int config_watch_handler(int fd, uint32_t mask, void *data);
void setup_config_watch(void);
void init_keybindings(void);
void config_expand_path(const char *src, char *dst, size_t dstlen);

/* layer.c */
void createlayersurface(struct wl_listener *listener, void *data);
void destroylayersurfacenotify(struct wl_listener *listener, void *data);
void commitlayersurfacenotify(struct wl_listener *listener, void *data);
void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
void createpopup(struct wl_listener *listener, void *data);
void commitpopup(struct wl_listener *listener, void *data);
void createlocksurface(struct wl_listener *listener, void *data);
void destroylocksurface(struct wl_listener *listener, void *data);
void destroylock(SessionLock *lock, int unlocked);
void destroysessionlock(struct wl_listener *listener, void *data);
void locksession(struct wl_listener *listener, void *data);
void unlocksession(struct wl_listener *listener, void *data);
void createidleinhibitor(struct wl_listener *listener, void *data);
void destroyidleinhibitor(struct wl_listener *listener, void *data);
void checkidleinhibitor(struct wlr_surface *exclude);

/* nixlytile.c (core) */
void setup(void);
void run(const char *startup_cmd);
void cleanup(void);
void cleanuplisteners(void);
void handlesig(int signo);
void spawn(const Arg *arg);
void quit(const Arg *arg);
uint64_t get_time_ns(void);
uint64_t monotonic_msec(void);
void ensure_shell_env(void);
void apply_startup_defaults(void);
int has_nixlytile_session_target(void);
int node_contains_client(LayoutNode *node, Client *c);
int subtree_bounds(LayoutNode *node, Monitor *m, struct wlr_box *out);
LayoutNode *ancestor_split(LayoutNode *node, int want_vert);

/* btrtile.c */
void btrtile_insert(LayoutNode **root, Client *c, Monitor *m);
void btrtile_remove(LayoutNode **root, Client *c);
void remove_client(Monitor *m, Client *c);
void destroy_tree(Monitor *m);
void init_tree(Monitor *m);
void btrtile_apply(LayoutNode *root, struct wlr_box area, Monitor *m);
LayoutNode *btrtile_find(LayoutNode *root, Client *c);
LayoutNode *btrtile_focus_dir(LayoutNode *root, Client *current, int dir);
void btrtile_swap(LayoutNode *a, LayoutNode *b);
void btrtile_free(LayoutNode *root);
int btrtile_count(LayoutNode *root);
void btrtile_set_ratio(LayoutNode *node, float ratio);
LayoutNode **get_current_root(Monitor *m);
LayoutNode *find_client_node(LayoutNode *node, Client *c);
int insert_client(Monitor *m, Client *focused_client, Client *new_client);
void insert_client_at(Monitor *m, Client *target, Client *new_client, double cx, double cy);
Client *xytoclient(double x, double y);
void start_tile_drag(Monitor *m, Client *c);
void end_tile_drag(void);
int same_column(Monitor *m, Client *c1, Client *c2);
void swap_columns(Monitor *m, Client *c1, Client *c2);
int can_move_tile(Monitor *m, Client *source, Client *target);
void swap_tiles_in_tree(Monitor *m, Client *c1, Client *c2);
extern int resizing_from_mouse;
extern int drag_was_alone_in_column;

/* XWayland */
#ifdef XWAYLAND
void activatex11(struct wl_listener *listener, void *data);
void associatex11(struct wl_listener *listener, void *data);
void configurex11(struct wl_listener *listener, void *data);
void createnotifyx11(struct wl_listener *listener, void *data);
void dissociatex11(struct wl_listener *listener, void *data);
void minimizenotify(struct wl_listener *listener, void *data);
void sethints(struct wl_listener *listener, void *data);
void xwaylandready(struct wl_listener *listener, void *data);
#endif

/* client.h helpers (inline) */
#include "client.h"

#endif /* NIXLYTILE_H */

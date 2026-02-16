/* globals.c - Global variable definitions */
#include "nixlytile.h"
#include "client.h"

/* variables */
pid_t child_pid = -1;
int locked;
void *exclusive_focus;

/* Config hot-reload */
int config_inotify_fd = -1;
int config_watch_wd = -1;
char config_path_cached[PATH_MAX] = {0};
struct wl_event_source *config_watch_source = NULL;
struct wl_event_source *config_rewatch_timer = NULL;
int config_needs_rewatch = 0;
struct wl_display *dpy;
struct wl_event_loop *event_loop;  /* Non-static: accessed by videoplayer */
VideoPlayer *active_videoplayer = NULL;
struct wlr_backend *backend;
struct wlr_scene *scene;
struct wlr_scene_tree *layers[NUM_LAYERS];
struct wlr_scene_tree *drag_icon;
/* Map from ZWLR_LAYER_SHELL_* constants to Lyr* enum */
const int layermap[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };
struct wlr_renderer *drw;
struct wlr_allocator *alloc;
struct wlr_compositor *compositor;
struct wlr_session *session;
struct StatusFont statusfont;
int fcft_initialized;
int has_nixlytile_session_target_cached = -1;

struct wlr_xdg_shell *xdg_shell;
struct wlr_xdg_activation_v1 *activation;
struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
struct wl_list clients; /* tiling order */
struct wl_list fstack;  /* focus order */
struct wlr_idle_notifier_v1 *idle_notifier;
struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
struct wlr_layer_shell_v1 *layer_shell;
struct wlr_output_manager_v1 *output_mgr;
struct wlr_content_type_manager_v1 *content_type_mgr;
struct wlr_tearing_control_manager_v1 *tearing_control_mgr;
struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
struct wlr_text_input_manager_v3 *text_input_mgr;
struct wlr_text_input_v3 *active_text_input;
struct wl_list text_inputs; /* TextInput.link */
struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
struct wlr_output_power_manager_v1 *power_mgr;

struct wlr_pointer_constraints_v1 *pointer_constraints;
struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
struct wlr_pointer_constraint_v1 *active_constraint;

struct wlr_cursor *cursor;
struct wlr_xcursor_manager *cursor_mgr;

struct wlr_scene_rect *root_bg;
struct wlr_session_lock_manager_v1 *session_lock_mgr;
struct wlr_scene_rect *locked_bg;
struct wlr_session_lock_v1 *cur_lock;

struct wlr_seat *seat;
KeyboardGroup *kb_group;
unsigned int cursor_mode;
Client *grabc;
int grabcx, grabcy; /* client-relative */
int resize_dir_x, resize_dir_y;
double resize_start_ratio_v, resize_start_ratio_h;
int resize_use_v, resize_use_h;
uint32_t resize_last_time;
double resize_last_x, resize_last_y;

struct wlr_output_layout *output_layout;
struct wlr_box sgeom;
struct wl_list mons;
Monitor *selmon;

struct wlr_box resize_start_box_v;
struct wlr_box resize_start_box_h;
struct wlr_box resize_start_box_f;
double resize_start_x, resize_start_y;
LayoutNode *resize_split_node;
LayoutNode *resize_split_node_h;
int fullscreen_adaptive_sync_enabled = 1;
int fps_limit_enabled = 0;        /* 1 if FPS limiter is active */
int fps_limit_value = 60;         /* FPS limit value (default 60) */
int game_mode_active = 0; /* Set when any client is fullscreen - pauses background tasks */
int game_mode_ultra = 0;  /* Ultra game mode - maximum performance, minimal latency */
Client *game_mode_client = NULL;  /* The fullscreen game client */
pid_t game_mode_pid = 0;  /* PID of fullscreen game process */
int game_mode_nice_applied = 0;  /* 1 if we changed nice value */
int game_mode_ioclass_applied = 0;  /* 1 if we changed IO priority */
int htpc_mode_active = 0; /* HTPC mode - hides statusbar, stops background tasks */
struct wl_event_source *status_timer;
struct wl_event_source *status_cpu_timer;
struct wl_event_source *status_hover_timer;
struct wl_event_source *cache_update_timer;
struct wl_event_source *nixpkgs_cache_timer;
int cache_update_phase = 0; /* 0=git, 1=file, 2=gaming, then restart */
StatusRefreshTask status_tasks[STATUS_TASKS_COUNT] = {
	{ refreshstatuscpu, 0 },
	{ refreshstatusram, 0 },
	{ refreshstatuslight, 0 },
	{ refreshstatusmic, 0 },
	{ refreshstatusvolume, 0 },
	{ refreshstatusbattery, 0 },
	{ refreshstatusnet, 0 },
	{ refreshstatusicons, 0 },
	{ refreshstatusfan, 0 },
};
int status_rng_seeded;

const float net_menu_row_bg[4] = {0.15f, 0.15f, 0.15f, 1.0f};
const float net_menu_row_bg_hover[4] = {0.25f, 0.25f, 0.25f, 1.0f};
const double status_icon_scale = 2.0;

/* Gamepad controller support */
struct wl_list gamepads;  /* GamepadDevice.link */
int gamepad_inotify_fd = -1;
int gamepad_inotify_wd = -1;
struct wl_event_source *gamepad_inotify_event = NULL;
struct wl_event_source *gamepad_inactivity_timer = NULL;

/* Pending gamepad devices (for delayed add after inotify) */
char gamepad_pending_paths[GAMEPAD_PENDING_MAX][128];
int gamepad_pending_count = 0;
struct wl_event_source *gamepad_pending_timer = NULL;

/* HTPC menu page toggles (0=disabled, 1=enabled) */
int htpc_page_pcgaming = 1;
int htpc_page_retrogaming = 1;
int htpc_page_movies = 1;
int htpc_page_tvshows = 1;
int htpc_page_nrk = 1;
int htpc_page_netflix = 1;
int htpc_page_viaplay = 1;
int htpc_page_tv2play = 1;
int htpc_page_f1tv = 1;
int htpc_page_quit = 1;

/* Client network bandwidth for media streaming (Mbps) */
int client_download_mbps = 100;  /* Default 100 Mbps */

/* Media playback state */

/* OSD bar menu selection */

PlaybackState playback_state = PLAYBACK_IDLE;
int playback_buffer_seconds = 0;    /* Seconds to buffer before playback */
int playback_buffer_progress = 0;   /* Current buffer progress (0-100) */
char playback_message[512] = "";    /* Message to display during buffering */
char playback_url[512] = "";        /* URL to play */
int64_t playback_file_size = 0;     /* File size in bytes */
int playback_duration = 0;          /* Duration in seconds */
int playback_is_movie = 0;          /* 1 for movie, 0 for episode */
uint64_t playback_start_time = 0;   /* When buffering started */

/* Media playback state */
int playback_media_id = 0;          /* Current media ID for resume */

/* OSD control bar */
int osd_visible = 0;                /* 1 if OSD bar is visible */
uint64_t osd_show_time = 0;         /* When OSD was last shown */
OsdMenuType osd_menu_open = OSD_MENU_NONE;
int osd_menu_selection = 0;         /* Selected item in open menu */

/* Audio/subtitle tracks for OSD display */
struct TrackInfo audio_tracks[MAX_TRACKS], subtitle_tracks[MAX_TRACKS];
int audio_track_count = 0;
int subtitle_track_count = 0;

/* Resume positions cache */
ResumeEntry resume_cache[256];
int resume_cache_count = 0;

/* Streaming service URLs */

/* Built HTPC menu based on enabled pages */
struct HtpcMenuItem htpc_menu_items[HTPC_MENU_MAX_ITEMS];
int htpc_menu_item_count = 0;


/* PC Gaming service configuration */
int gaming_service_enabled[GAMING_SERVICE_COUNT] = {1, 1, 1, 1}; /* All enabled by default */
const char *gaming_service_names[] = {"Steam", "Heroic", "Lutris", "Bottles"};
/* Game tile size - aspect ratio based on Steam headers (460x215) */

/* GPU detection and management */


GpuInfo detected_gpus[MAX_GPUS];
int detected_gpu_count = 0;
int discrete_gpu_idx = -1;   /* Index of preferred discrete GPU, -1 if none */
int integrated_gpu_idx = -1; /* Index of integrated GPU, -1 if none */

/* GPU-specific game launch parameters */
#include "game_launch_params.h"

/* PC gaming cache file watcher for realtime updates */
int pc_gaming_cache_inotify_fd = -1;
int pc_gaming_cache_inotify_wd = -1;
struct wl_event_source *pc_gaming_cache_event = NULL;

/* Gamepad joystick cursor control */
struct wl_event_source *gamepad_cursor_timer = NULL;

/* Bluetooth controller auto-pairing */
struct wl_event_source *bt_scan_timer = NULL;
struct wl_event_source *bt_bus_event = NULL;
sd_bus *bt_bus = NULL;
int bt_scanning = 0;

/* Known gamepad name patterns for auto-pairing */
const char *bt_gamepad_patterns[] = {
	"Xbox",
	"DualSense",
	"DualShock",
	"PS5",
	"PS4",
	"PlayStation",
	"8BitDo",
	"Pro Controller",   /* Nintendo Switch Pro */
	"Joy-Con",
	"Wireless Controller",
	"Game Controller",
	"Gamepad",
	NULL
};

pid_t public_ip_pid = -1;
int public_ip_fd = -1;
struct wl_event_source *public_ip_event = NULL;
char public_ip_buf[128];
size_t public_ip_len = 0;
pid_t ssid_pid = -1;
int ssid_fd = -1;
struct wl_event_source *ssid_event = NULL;
char ssid_buf[256];
size_t ssid_len = 0;
time_t ssid_last_time = 0;
int tray_anchor_x = -1;
int tray_anchor_y = -1;
uint64_t tray_anchor_time_ms;
struct CpuSample cpu_prev[MAX_CPU_CORES];
int cpu_prev_count;
double cpu_last_percent = -1.0;
char cpu_text[32] = "--%";
double ram_last_mb = -1.0;
char ram_text[32] = "--";
double battery_last_percent = -1.0;
char battery_text[32] = "--%";
double net_last_down_bps = -1.0;
double net_last_up_bps = -1.0;
char last_clock_render[32];
char last_cpu_render[32];
char last_ram_render[32];
char last_light_render[32];
char last_volume_render[32];
char last_mic_render[32];
char last_battery_render[32];
char last_net_render[64];
int last_clock_h;
int last_cpu_h;
int last_ram_h;
int last_light_h;
int last_volume_h;
int last_mic_h;
int last_battery_h;
int last_net_h;
int battery_path_initialized;
int backlight_paths_initialized;
uint64_t volume_last_read_speaker_ms;
uint64_t volume_last_read_headset_ms;
double volume_cached_speaker = -1.0;
double volume_cached_headset = -1.0;
int volume_cached_speaker_muted = -1;
int volume_cached_headset_muted = -1;
uint64_t mic_last_read_ms;
double mic_cached = -1.0;
int mic_cached_muted = -1;
uint64_t last_pointer_motion_ms;
const float *volume_text_color = NULL; /* set at runtime; defaults to statusbar_fg */
const float *mic_text_color = NULL;
const float *statusbar_fg_override = NULL;
pid_t wifi_scan_pid = -1;
int wifi_scan_fd = -1;
struct wl_event_source *wifi_scan_event = NULL;
struct wl_event_source *wifi_scan_timer = NULL;
struct wl_event_source *cpu_popup_refresh_timer = NULL;
struct wl_event_source *ram_popup_refresh_timer = NULL;
struct wl_event_source *popup_delay_timer = NULL;
struct wl_event_source *video_check_timer = NULL;
struct wl_event_source *hz_osd_timer = NULL;
struct wl_event_source *playback_osd_timer = NULL;
struct wlr_scene_tree *playback_osd_tree = NULL;
struct wl_event_source *pc_gaming_install_timer = NULL;
struct wl_event_source *game_refocus_timer = NULL;
struct wl_event_source *media_view_poll_timer = NULL;
struct wl_event_source *osk_dpad_repeat_timer = NULL;
int osk_dpad_held_button = 0;  /* BTN_DPAD_UP/DOWN/LEFT/RIGHT or 0 if none */
Monitor *osk_dpad_held_mon = NULL;
Client *game_refocus_client = NULL;
char wifi_scan_buf[8192];
size_t wifi_scan_len = 0;
int wifi_scan_inflight;
unsigned int wifi_networks_generation;
unsigned int wifi_scan_generation;
int wifi_networks_accept_updates;
int wifi_networks_freeze_existing;
struct wl_list wifi_networks; /* WifiNetwork */
int wifi_networks_initialized;
struct wl_list vpn_connections; /* VpnConnection */
int vpn_list_initialized;
pid_t vpn_scan_pid = -1;
int vpn_scan_fd = -1;
struct wl_event_source *vpn_scan_event;
char vpn_scan_buf[8192];
size_t vpn_scan_len;
int vpn_scan_inflight;
pid_t vpn_connect_pid = -1;
int vpn_connect_fd = -1;
struct wl_event_source *vpn_connect_event;
char vpn_connect_buf[4096];
size_t vpn_connect_len;
char vpn_pending_name[128];
char net_text[64] = "Net: --";
char net_local_ip[64] = "--";
char net_public_ip[64] = "--";
char net_down_text[32] = "--";
char net_up_text[32] = "--";
char net_ssid[64] = "--";
double net_last_wifi_quality = -1.0;
int net_link_speed_mbps = -1;
char net_iface[64] = {0};
char net_prev_iface[64] = {0};
int net_is_wireless;
int net_available;
char net_icon_path[PATH_MAX] = "images/svg/no_connection.svg";
char net_icon_loaded_path[PATH_MAX];
char cpu_icon_path[PATH_MAX] = "images/svg/cpu.svg";
char cpu_icon_loaded_path[PATH_MAX];
const uint32_t cpu_popup_refresh_interval_ms = 1000;
const uint32_t ram_popup_refresh_interval_ms = 1000;
char light_icon_path[PATH_MAX] = "images/svg/light.svg";
char light_icon_loaded_path[PATH_MAX];
char ram_icon_path[PATH_MAX] = "images/svg/ram.svg";
char ram_icon_loaded_path[PATH_MAX];
char battery_icon_path[PATH_MAX] = "images/svg/battery-100.svg";
char battery_icon_loaded_path[PATH_MAX];
char mic_icon_path[PATH_MAX] = "images/svg/microphone.svg";
char mic_icon_loaded_path[PATH_MAX];
char volume_icon_path[PATH_MAX] = "images/svg/speaker_100.svg";
char volume_icon_loaded_path[PATH_MAX];
const char net_icon_no_conn[] = "images/svg/no_connection.svg";
const char net_icon_eth[] = "images/svg/ethernet.svg";
const char net_icon_wifi_100[] = "images/svg/wifi_100.svg";
const char net_icon_wifi_75[] = "images/svg/wifi_75.svg";
const char net_icon_wifi_50[] = "images/svg/wifi_50.svg";
const char net_icon_wifi_25[] = "images/svg/wifi_25.svg";
const char battery_icon_25[] = "images/svg/battery-25.svg";
const char battery_icon_50[] = "images/svg/battery-50.svg";
const char battery_icon_75[] = "images/svg/battery-75.svg";
const char battery_icon_100[] = "images/svg/battery-100.svg";
const char volume_icon_speaker_25[] = "images/svg/speaker_25.svg";
const char volume_icon_speaker_50[] = "images/svg/speaker_50.svg";
const char volume_icon_speaker_100[] = "images/svg/speaker_100.svg";
const char volume_icon_speaker_muted[] = "images/svg/speaker_muted.svg";
const char volume_icon_headset[] = "images/svg/headset.svg";
const char volume_icon_headset_muted[] = "images/svg/headset_muted.svg";
const char mic_icon_unmuted[] = "images/svg/microphone.svg";
const char mic_icon_muted[] = "images/svg/microphone_muted.svg";
char net_icon_wifi_100_resolved[PATH_MAX];
char net_icon_wifi_75_resolved[PATH_MAX];
char net_icon_wifi_50_resolved[PATH_MAX];
char net_icon_wifi_25_resolved[PATH_MAX];
char net_icon_eth_resolved[PATH_MAX];
char net_icon_no_conn_resolved[PATH_MAX];
char clock_icon_path[PATH_MAX] = "images/svg/clock.svg";
char clock_icon_loaded_path[PATH_MAX];
int net_icon_loaded_h;
int net_icon_w;
int net_icon_h;
struct wlr_buffer *net_icon_buf;
int clock_icon_loaded_h;
int clock_icon_w;
int clock_icon_h;
struct wlr_buffer *clock_icon_buf;
int cpu_icon_loaded_h;
int cpu_icon_w;
int cpu_icon_h;
struct wlr_buffer *cpu_icon_buf;
int light_icon_loaded_h;
int light_icon_w;
int light_icon_h;
struct wlr_buffer *light_icon_buf;
int ram_icon_loaded_h;
int ram_icon_w;
int ram_icon_h;
struct wlr_buffer *ram_icon_buf;
int battery_icon_loaded_h;
int battery_icon_w;
int battery_icon_h;
struct wlr_buffer *battery_icon_buf;
int mic_icon_loaded_h;
int mic_icon_w;
int mic_icon_h;
struct wlr_buffer *mic_icon_buf;
int volume_icon_loaded_h;
int volume_icon_w;
int volume_icon_h;
struct wlr_buffer *volume_icon_buf;
char bluetooth_icon_path[PATH_MAX] = "images/svg/bluetooth.svg";
char bluetooth_icon_loaded_path[PATH_MAX];
int bluetooth_icon_loaded_h;
int bluetooth_icon_w;
int bluetooth_icon_h;
struct wlr_buffer *bluetooth_icon_buf;
int bluetooth_available;
char steam_icon_path[PATH_MAX] = "images/svg/steam.svg";
char steam_icon_loaded_path[PATH_MAX];
int steam_icon_loaded_h;
int steam_icon_w;
int steam_icon_h;
struct wlr_buffer *steam_icon_buf;
int steam_running;
char discord_icon_path[PATH_MAX] = "images/svg/discord.svg";
char discord_icon_loaded_path[PATH_MAX];
int discord_icon_loaded_h;
int discord_icon_w;
int discord_icon_h;
struct wlr_buffer *discord_icon_buf;
int discord_running;
char fan_icon_path[PATH_MAX] = "images/svg/fan.svg";
char fan_icon_loaded_path[PATH_MAX];
int fan_icon_loaded_h;
int fan_icon_w;
int fan_icon_h;
struct wlr_buffer *fan_icon_buf;
char fan_text[32] = "--";
char last_fan_render[32];
int last_fan_h;
unsigned long long net_prev_rx;
unsigned long long net_prev_tx;
struct timespec net_prev_ts;
int net_prev_valid;
time_t net_public_ip_last;
sd_bus *tray_bus;
struct wl_event_source *tray_event;
sd_bus_slot *tray_vtable_slot;
sd_bus_slot *tray_fdo_vtable_slot;
sd_bus_slot *tray_name_slot;
int tray_host_registered;
struct wl_list tray_items;
double light_last_percent = -1.0;
double light_cached_percent = -1.0;
char light_text[32] = "--%";
double mic_last_percent = 80.0;
char mic_text[32] = "--%";
double volume_last_speaker_percent = 70.0;
double volume_last_headset_percent = 70.0;
double speaker_active = 70.0;
double speaker_stored = 70.0;
double microphone_active = 80.0;
double microphone_stored = 80.0;
char volume_text[32] = "--%";
int volume_muted = -1;
int mic_muted = -1;
int mic_last_color_is_muted = -1;
int volume_last_color_is_muted = -1;
char backlight_brightness_path[PATH_MAX];
char backlight_max_path[PATH_MAX];
int backlight_available;
int backlight_writable;
char battery_capacity_path[PATH_MAX];
char battery_device_dir[PATH_MAX];  /* e.g. /sys/class/power_supply/BAT0 */
int battery_available;
const double light_step = 5.0;
const double volume_step = 3.0;
const double volume_max_percent = 150.0;
const double mic_step = 3.0;
const double mic_max_percent = 150.0;
double cpu_last_core_percent[MAX_CPU_CORES];
int cpu_core_count;
char sysicons_text[64] = "Tray";
DesktopEntry desktop_entries[DESKTOP_ENTRIES_MAX];
int desktop_entry_count = 0;
int desktop_entries_loaded = 0;

/* Nixpkgs package cache */
NixpkgEntry nixpkg_entries[NIXPKGS_MAX_ENTRIES];
int nixpkg_entry_count = 0;
int nixpkg_entries_loaded = 0;
char nixpkgs_cache_path[PATH_MAX] = "";

/* Installed package check icon */
struct wlr_buffer *nixpkg_ok_icon_buf = NULL;
int nixpkg_ok_icon_height = 0;

/* global event handlers */
struct wl_listener cursor_axis = {.notify = axisnotify};
struct wl_listener cursor_button = {.notify = buttonpress};
struct wl_listener cursor_frame = {.notify = cursorframe};
struct wl_listener cursor_motion = {.notify = motionrelative};
struct wl_listener cursor_motion_absolute = {.notify = motionabsolute};
struct wl_listener gpu_reset = {.notify = gpureset};
struct wl_listener layout_change = {.notify = updatemons};
struct wl_listener new_idle_inhibitor = {.notify = createidleinhibitor};
struct wl_listener new_input_device = {.notify = inputdevice};
struct wl_listener new_virtual_keyboard = {.notify = virtualkeyboard};
struct wl_listener new_virtual_pointer = {.notify = virtualpointer};
struct wl_listener new_text_input = {.notify = textinput};
struct wl_listener new_pointer_constraint = {.notify = createpointerconstraint};
struct wl_listener new_output = {.notify = createmon};
struct wl_listener new_xdg_toplevel = {.notify = createnotify};
struct wl_listener new_xdg_popup = {.notify = createpopup};
struct wl_listener new_xdg_decoration = {.notify = createdecoration};
struct wl_listener new_layer_surface = {.notify = createlayersurface};
struct wl_listener output_mgr_apply = {.notify = outputmgrapply};
struct wl_listener output_mgr_test = {.notify = outputmgrtest};
struct wl_listener output_power_mgr_set_mode = {.notify = powermgrsetmode};
struct wl_listener request_activate = {.notify = urgent};
struct wl_listener request_cursor = {.notify = setcursor};
struct wl_listener request_set_psel = {.notify = setpsel};
struct wl_listener request_set_sel = {.notify = setsel};
struct wl_listener request_set_cursor_shape = {.notify = setcursorshape};
struct wl_listener request_start_drag = {.notify = requeststartdrag};
struct wl_listener start_drag = {.notify = startdrag};
struct wl_listener new_session_lock = {.notify = locksession};

#ifdef XWAYLAND
struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
struct wl_listener xwayland_ready = {.notify = xwaylandready};
struct wlr_xwayland *xwayland;
#endif

#ifndef WLR_SILENCE
#define WLR_SILENCE (WLR_ERROR - 1)
#endif

/* configuration, allows nested code to access above variables */

/* Runtime font storage - config values are copied here so we can modify them */
char *runtime_fonts[8];
int runtime_fonts_set = 0;

/* attempt to encapsulate suck into one file */

/* configuration, allows nested code to access above variables */
#include "config.h"

const size_t nrules = LENGTH(rules);
const size_t nlayouts = LENGTH(layouts);
const size_t nmonrules = LENGTH(monrules);
const size_t nbuttons = LENGTH(buttons);

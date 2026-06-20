/* globals.c - Global variable definitions */
#include "nixlytile.h"
#include "client.h"

/* variables */
FILE *log_file = NULL;
FILE *debug_log_file = NULL;
int log_stderr_fd = -1; /* saved original stderr for die() */
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
int monconf_inotify_fd = -1;
int monconf_watch_wd = -1;
char monconf_path_cached[PATH_MAX] = {0};
struct wl_event_source *monconf_watch_source = NULL;
struct wl_event_source *monitor_setup_timer = NULL;
int monovl_inotify_fd = -1;
int monovl_watch_wd = -1;
struct wl_event_source *monovl_watch_source = NULL;
struct wl_display *dpy;
struct wl_event_loop *event_loop;
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
struct wl_list closing_anims; /* ClosingAnim list — close-anim snapshots */
struct wlr_idle_notifier_v1 *idle_notifier;
struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
struct wlr_layer_shell_v1 *layer_shell;
struct wlr_output_manager_v1 *output_mgr;
struct wlr_content_type_manager_v1 *content_type_mgr;
struct wlr_tearing_control_manager_v1 *tearing_control_mgr;
struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
struct wlr_tablet_manager_v2 *tablet_v2_mgr;
struct wlr_text_input_manager_v3 *text_input_mgr;
struct wlr_text_input_v3 *active_text_input;
struct wl_list text_inputs; /* TextInput.link */
struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
struct wlr_output_power_manager_v1 *power_mgr;
struct wlr_color_manager_v1 *color_mgr;
struct wlr_color_representation_manager_v1 *color_repr_mgr;
struct wlr_keyboard_shortcuts_inhibit_manager_v1 *kb_shortcuts_inhibit_mgr;
struct wlr_pointer_gestures_v1 *pointer_gestures;
struct wlr_xdg_foreign_registry *foreign_registry;
struct wlr_xdg_foreign_v2 *xdg_foreign;
struct wlr_xdg_wm_dialog_v1 *xdg_dialog_mgr;
struct wlr_ext_image_copy_capture_manager_v1 *image_copy_capture_mgr;
struct wlr_security_context_manager_v1 *security_ctx_mgr;
struct wlr_xdg_system_bell_v1 *system_bell;
struct wlr_ext_foreign_toplevel_list_v1 *foreign_toplevel_list;
struct wlr_ext_data_control_manager_v1 *ext_data_control_mgr;
struct wlr_fixes *protocol_fixes;

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
pid_t retro_session_pid = 0;  /* PID of retroarch (or other retro emulator) we launched; tree excluded from game mode */
int game_mode_nice_applied = 0;  /* 1 if we changed nice value */
int game_mode_ioclass_applied = 0;  /* 1 if we changed IO priority */
int game_mode_oom_applied = 0;  /* 1 if we changed OOM score */
int game_mode_governor_applied = 0;  /* 1 if we changed CPU governor */
int compositor_rt_applied = 0;  /* 1 if compositor has RT scheduling */
int fan_boost_active = 0; /* 1 if GPU fan boost is active during gaming */
int fan_thermal_active = 0; /* 1 if thermal fan management controls fans */
struct wl_event_source *fan_thermal_timer = NULL;
pid_t frozen_pids[4096];    /* PIDs frozen during game mode */
int frozen_pid_count = 0;   /* Number of frozen PIDs */
int game_mode_swappiness_applied = 0; /* 1 if swappiness was changed */
int game_mode_affinity_applied = 0;  /* 1 if CPU affinity was changed */
int game_mode_raw_input_applied = 0; /* 1 if pointer accel was disabled */
struct libinput_device *pointer_devices[MAX_POINTER_DEVICES];
int pointer_device_count = 0;
/* statusbar / launcher / nixpkgs / gamepad globals removed */

/* GPU detection and management */


GpuInfo detected_gpus[MAX_GPUS];
int detected_gpu_count = 0;
int discrete_gpu_idx = -1;   /* Index of preferred discrete GPU, -1 if none */
int integrated_gpu_idx = -1; /* Index of integrated GPU, -1 if none */
int nvidia_render_primary = 0; /* reverseSync: dGPU renders session, iGPU outputs */

/* CPU cursor buffer for Nvidia HW cursor plane */
struct CpuCursorBuffer *cpu_cursor_buf = NULL;
int cpu_cursor_active = 0;
int dgpu_render_fd = -1;     /* Held open to prevent dGPU D3cold/runtime suspend */
int g_explicit_sync_ok = 0;  /* 1 = DRM syncobj timeline manager active */
struct wl_event_source *dgpu_power_watchdog = NULL;


/* network / launcher / tray / fan / battery / volume / brightness / desktop entries removed */

/* Screenshot */
int screenshot_mode = 0;

/* Diagnostics logging */
int diag_log_fd = -1;
int audio_log_fd = -1;
int error_log_fd = -1;
int game_log_fd = -1;
struct wl_event_source *diag_timer = NULL;

/* nixpkgs cache + ok icon removed */

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
struct wl_listener new_kb_shortcuts_inhibitor = {.notify = newkbshortcutsinhibitor};

#ifdef XWAYLAND
struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
struct wl_listener xwayland_ready = {.notify = xwaylandready};
struct wlr_xwayland *xwayland;

xcb_atom_t atom_steam_game;
xcb_atom_t atom_steam_overlay;
xcb_atom_t atom_steam_bigpicture;
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

/* Runtime overrides loaded from ~/.config/nixlytile/config.kdl */
Rule  *runtime_rules = NULL;
size_t runtime_rules_count = 0;

struct xkb_rule_names runtime_xkb_rules = {0};
int                   runtime_xkb_rules_set = 0;

char **runtime_autostart = NULL;
size_t runtime_autostart_count = 0;
pid_t *runtime_autostart_pids = NULL;
int    runtime_config_loaded = 0;

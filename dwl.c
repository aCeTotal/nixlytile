/*
 * See LICENSE file for copyright and license details.
 */
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
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
#include <math.h>
#include <glib.h>
#include <drm_fourcc.h>
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
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
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

#ifndef SD_BUS_EVENT_READABLE
#define SD_BUS_EVENT_READABLE 1
#endif
#ifndef SD_BUS_EVENT_WRITABLE
#define SD_BUS_EVENT_WRITABLE 2
#endif

#include "util.h"

/* macros */
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
#define TAGMASK                 ((1u << TAGCOUNT) - 1)
#define MAX_TAGS                32
#define STATUS_FAST_MS          3000
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
#define LISTEN_STATIC(E, H)     do { struct wl_listener *_l = ecalloc(1, sizeof(*_l)); _l->notify = (H); wl_signal_add((E), _l); } while (0)

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

/* enums */
enum { CurNormal, CurPressed, CurMove, CurResize }; /* cursor */
enum { XDGShell, LayerShell, X11 }; /* client types */
enum { LyrBg, LyrBottom, LyrTile, LyrFloat, LyrTop, LyrFS, LyrOverlay, LyrBlock, NUM_LAYERS }; /* scene layers */
#define MODAL_MAX_RESULTS 512
#define MODAL_RESULT_LEN 256

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

typedef struct LayoutNode LayoutNode;
typedef struct Monitor Monitor;
typedef struct StatusBar StatusBar;
typedef struct StatusModule StatusModule;
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
	int proc_count;
	CpuProcEntry procs[10];
} CpuPopup;

typedef struct {
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *bg;
	int visible;
	int x, y, width, height;
	int active_idx;
	char search[3][256];
	int search_len[3];
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
} ModalOverlay;

typedef struct {
	char name[128];
	char exec[256];
	char name_lower[128];
	int used;
} DesktopEntry;

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
} NetPopup;

typedef struct {
	void (*fn)(void);
	uint64_t next_due_ms;
} StatusRefreshTask;

static const float net_menu_row_bg[4] = {0.15f, 0.15f, 0.15f, 1.0f};
static const float net_menu_row_bg_hover[4] = {0.25f, 0.25f, 0.25f, 1.0f};
static const double status_icon_scale = 2.0;

typedef struct WifiNetwork {
	char ssid[128];
	int strength;
	int secure;
	struct wl_list link;
} WifiNetwork;

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
	struct wl_list entries;   /* main menu entries */
	struct wl_list networks;  /* WifiNetwork list */
} NetMenu;

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
	CpuPopup cpu_popup;
	NetPopup net_popup;
	TrayMenu tray_menu;
	NetMenu net_menu;
	StatusModule sysicons;
};

typedef struct TrayItem {
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
} TrayItem;

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

static void
drawrect(struct wlr_scene_tree *parent, int x, int y,
		int width, int height, const float color[static 4])
{
	struct wlr_scene_rect *r;
	float col[4];

	if (!parent || width <= 0 || height <= 0)
		return;

	col[0] = color[0];
	col[1] = color[1];
	col[2] = color[2];
	col[3] = color[3];

	r = wlr_scene_rect_create(parent, width, height, col);
	if (r)
		wlr_scene_node_set_position(&r->node, x, y);
}

static void
drawhoverrect(struct wlr_scene_tree *parent, int x, int y,
		int width, int height, const float color[static 4], float fade)
{
	struct wlr_scene_rect *r;
	float col[4];

	if (!parent || width <= 0 || height <= 0)
		return;

	if (fade < 0.0f)
		fade = 0.0f;
	if (fade > 1.0f)
		fade = 1.0f;

	col[0] = color[0];
	col[1] = color[1];
	col[2] = color[2];
	col[3] = color[3] * fade;

	r = wlr_scene_rect_create(parent, width, height, col);
	if (r)
		wlr_scene_node_set_position(&r->node, x, y);
}

static void UNUSED
drawroundedrect(struct wlr_scene_tree *parent, int x, int y,
        int width, int height, const float color[static 4])
{
	int radius, yy, inset_top, inset_bottom, inset, start, h, w;
	struct wlr_scene_rect *r;

	if (!parent || width <= 0 || height <= 0)
		return;

	radius = MIN(4, MIN(width, height) / 4);

	yy = 0;
	while (yy < height) {
		inset_top = radius ? MAX(0, radius - yy) : 0;
		inset_bottom = radius ? MAX(0, radius - ((height - 1) - yy)) : 0;
		inset = MAX(inset_top, inset_bottom);
		start = yy;

		while (yy < height) {
			inset_top = radius ? MAX(0, radius - yy) : 0;
			inset_bottom = radius ? MAX(0, radius - ((height - 1) - yy)) : 0;
			if (MAX(inset_top, inset_bottom) != inset)
				break;
			yy++;
		}

		h = yy - start;
		w = width - 2 * inset;
		if (h > 0 && w > 0) {
			r = wlr_scene_rect_create(parent, w, h, color);
			if (r)
				wlr_scene_node_set_position(&r->node, x + inset, y + start);
		}
	}
}
typedef struct {
	/* Must keep this field first */
	unsigned int type; /* XDGShell or X11* */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_rect *border[4]; /* top, bottom, left, right */
	struct wlr_scene_tree *scene_surface;
	struct wl_list link;
	struct wl_list flink;
	struct wlr_box geom; /* layout-relative, includes border */
	struct wlr_box prev; /* layout-relative, includes border */
	struct wlr_box bounds; /* only width and height are used */
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
	uint32_t resize; /* configure serial of a pending resize */
	int pending_resize_w, pending_resize_h; /* last requested size while pending */
	struct wlr_box old_geom;
	char *output;
} Client;

typedef struct {
	uint32_t mod;
	xkb_keysym_t keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	struct wlr_keyboard_group *wlr_group;

	int nsyms;
	const xkb_keysym_t *keysyms; /* invalid if nsyms == 0 */
	uint32_t mods; /* invalid if nsyms == 0 */
	struct wl_event_source *key_repeat_source;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
} KeyboardGroup;

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

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *fullscreen_bg; /* See createmon() for info */
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener request_state;
	struct wl_listener destroy_lock_surface;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_box m; /* monitor area, layout-relative */
	struct wlr_box w; /* window area, layout-relative */
	struct wl_list layers[4]; /* LayerSurface.link */
	StatusBar statusbar;
	ModalOverlay modal;
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
	LayoutNode *root;
};

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
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
} PointerConstraint;

typedef struct {
	const char *id;
	const char *title;
	uint32_t tags;
	int isfloating;
	int monitor;
} Rule;

typedef struct {
	struct wlr_scene_tree *scene;

	struct wlr_session_lock_v1 *lock;
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
} SessionLock;

/* function declarations */
static void applybounds(Client *c, struct wlr_box *bbox);
static void applyrules(Client *c);
static void arrange(Monitor *m);
static void arrangelayer(Monitor *m, struct wl_list *list,
		struct wlr_box *usable_area, int exclusive);
static void arrangelayers(Monitor *m);
static void axisnotify(struct wl_listener *listener, void *data);
static void buttonpress(struct wl_listener *listener, void *data);
static void chvt(const Arg *arg);
static void checkidleinhibitor(struct wlr_surface *exclude);
static void cleanup(void);
static void cleanupmon(struct wl_listener *listener, void *data);
static void cleanuplisteners(void);
static void closemon(Monitor *m);
static void commitlayersurfacenotify(struct wl_listener *listener, void *data);
static void commitnotify(struct wl_listener *listener, void *data);
static void commitpopup(struct wl_listener *listener, void *data);
static void createdecoration(struct wl_listener *listener, void *data);
static void createidleinhibitor(struct wl_listener *listener, void *data);
static void createkeyboard(struct wlr_keyboard *keyboard);
static KeyboardGroup *createkeyboardgroup(void);
static void createlayersurface(struct wl_listener *listener, void *data);
static void createlocksurface(struct wl_listener *listener, void *data);
static void createmon(struct wl_listener *listener, void *data);
static void createnotify(struct wl_listener *listener, void *data);
static void createpointer(struct wlr_pointer *pointer);
static void createpointerconstraint(struct wl_listener *listener, void *data);
static void createpopup(struct wl_listener *listener, void *data);
static void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
static void cursorframe(struct wl_listener *listener, void *data);
static void cursorwarptohint(void);
static void destroydecoration(struct wl_listener *listener, void *data);
static void destroydragicon(struct wl_listener *listener, void *data);
static void destroyidleinhibitor(struct wl_listener *listener, void *data);
static void destroylayersurfacenotify(struct wl_listener *listener, void *data);
static void destroylock(SessionLock *lock, int unlocked);
static void destroylocksurface(struct wl_listener *listener, void *data);
static void destroynotify(struct wl_listener *listener, void *data);
static void destroypointerconstraint(struct wl_listener *listener, void *data);
static void destroysessionlock(struct wl_listener *listener, void *data);
static void destroykeyboardgroup(struct wl_listener *listener, void *data);
static Monitor *dirtomon(enum wlr_direction dir);
static void focusclient(Client *c, int lift);
static void focusmon(const Arg *arg);
static void focusstack(const Arg *arg);
static Client *focustop(Monitor *m);
static void fullscreennotify(struct wl_listener *listener, void *data);
static void gpureset(struct wl_listener *listener, void *data);
static void handlesig(int signo);
static void initstatusbar(Monitor *m);
static void incnmaster(const Arg *arg);
static void inputdevice(struct wl_listener *listener, void *data);
static int keybinding(uint32_t mods, xkb_keysym_t sym);
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static int keyrepeat(void *data);
static void killclient(const Arg *arg);
static void layoutstatusbar(Monitor *m, const struct wlr_box *area,
		struct wlr_box *client_area);
static void locksession(struct wl_listener *listener, void *data);
static void mapnotify(struct wl_listener *listener, void *data);
static void maximizenotify(struct wl_listener *listener, void *data);
static void monocle(Monitor *m);
static void motionabsolute(struct wl_listener *listener, void *data);
static void motionnotify(uint32_t time, struct wlr_input_device *device, double sx,
		double sy, double sx_unaccel, double sy_unaccel);
static void motionrelative(struct wl_listener *listener, void *data);
static void moveresize(const Arg *arg);
static void outputmgrapply(struct wl_listener *listener, void *data);
static void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);
static void outputmgrtest(struct wl_listener *listener, void *data);
static void pointerfocus(Client *c, struct wlr_surface *surface,
		double sx, double sy, uint32_t time);
static void printstatus(void);
static void powermgrsetmode(struct wl_listener *listener, void *data);
static void quit(const Arg *arg);
static struct wlr_buffer *statusbar_buffer_from_argb32(const uint32_t *data, int width, int height);
static int ensure_cpu_icon_buffer(int target_h);
static void drop_cpu_icon_buffer(void);
static int ensure_light_icon_buffer(int target_h);
static void drop_light_icon_buffer(void);
static int ensure_ram_icon_buffer(int target_h);
static void drop_ram_icon_buffer(void);
static int ensure_battery_icon_buffer(int target_h);
static void drop_battery_icon_buffer(void);
static int ensure_clock_icon_buffer(int target_h);
static void drop_clock_icon_buffer(void);
static int ensure_mic_icon_buffer(int target_h);
static void drop_mic_icon_buffer(void);
static int ensure_volume_icon_buffer(int target_h);
static void drop_volume_icon_buffer(void);
static void renderclock(StatusModule *module, int bar_height, const char *text);
static void renderlight(StatusModule *module, int bar_height, const char *text);
static void rendernet(StatusModule *module, int bar_height, const char *text);
static void renderbattery(StatusModule *module, int bar_height, const char *text);
static void rendermon(struct wl_listener *listener, void *data);
static void rendervolume(StatusModule *module, int bar_height, const char *text);
static void rendermic(StatusModule *module, int bar_height, const char *text);
static void render_icon_label(StatusModule *module, int bar_height, const char *text,
		int (*ensure_icon)(int target_h), struct wlr_buffer **icon_buf,
		int *icon_w, int *icon_h, int min_text_w, int icon_gap,
		const float text_color[static 4]);
static void renderworkspaces(Monitor *m, StatusModule *module, int bar_height);
static void freestatusfont(void);
static int loadstatusfont(void);
static int status_text_width(const char *text);
static int tray_render_label(StatusModule *module, const char *text, int x, int bar_height,
		const float color[static 4]);
static void positionstatusmodules(Monitor *m);
static void refreshstatusclock(void);
static void refreshstatuslight(void);
static void refreshstatusvolume(void);
static void refreshstatusmic(void);
static void refreshstatusbattery(void);
static void refreshstatusnet(void);
static void request_public_ip_async(void);
static void stop_public_ip_fetch(void);
static void request_ssid_async(const char *iface);
static void stop_ssid_fetch(void);
static void refreshstatusicons(void);
static void refreshstatustags(void);
static void init_status_refresh_tasks(void);
static int status_should_render(StatusModule *module, int barh, const char *text,
		char *last_text, size_t last_len, int *last_h);
static void initial_status_refresh(void);
static void net_menu_hide_all(void);
static void net_menu_open(Monitor *m);
static void net_menu_render(Monitor *m);
static void net_menu_submenu_render(Monitor *m);
static void net_menu_update_hover(Monitor *m, double cx, double cy);
static void wifi_networks_clear(void);
static void request_wifi_scan(void);
static void wifi_scan_plan_rescan(void);
static int wifi_scan_event_cb(int fd, uint32_t mask, void *data);
static void connect_wifi_ssid(const char *ssid);
static void connect_wifi_with_prompt(const char *ssid, int secure);
static int status_task_hover_active(void (*fn)(void));
static void trigger_status_task_now(void (*fn)(void));
static int public_ip_event_cb(int fd, uint32_t mask, void *data);
static int ssid_event_cb(int fd, uint32_t mask, void *data);
static void updatetaghover(Monitor *m, double cx, double cy);
static void updatenethover(Monitor *m, double cx, double cy);
static int tray_bus_event(int fd, uint32_t mask, void *data);
static int tray_method_register_item(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int tray_method_register_host(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int tray_property_get_registered(sd_bus *bus, const char *path, const char *interface,
        const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);
static int tray_property_get_host_registered(sd_bus *bus, const char *path, const char *interface,
        const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);
static int tray_property_get_protocol_version(sd_bus *bus, const char *path, const char *interface,
        const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error);
static int tray_item_load_icon(TrayItem *it);
static int tray_render_label(StatusModule *module, const char *text, int x, int bar_height,
		const float color[static 4]);
static void tray_emit_host_registered(void);
static void tray_menu_clear(TrayMenu *menu);
static void tray_menu_hide(Monitor *m);
static void tray_menu_hide_all(void);
static int tray_item_get_menu_path(TrayItem *it);
static int tray_menu_open_at(Monitor *m, TrayItem *it, int icon_x);
static void tray_menu_render(Monitor *m);
static TrayMenuEntry *tray_menu_entry_at(Monitor *m, int lx, int ly);
static int tray_menu_send_event(TrayMenu *menu, TrayMenuEntry *entry, uint32_t time_msec);
static int tray_menu_parse_node(sd_bus_message *msg, TrayMenu *menu, int depth, int max_depth);
static int tray_menu_parse_node_body(sd_bus_message *msg, TrayMenu *menu, int depth, int max_depth);
static void tray_sanitize_label(const char *src, char *dst, size_t len);
static int tray_search_item_path(const char *service, const char *start_path,
		char *out, size_t outlen, int depth);
static int tray_find_item_path(const char *service, char *path, size_t pathlen);
static void tray_add_item(const char *service, const char *path, int emit_signals);
static void tray_scan_existing_items(void);
static void tray_update_icons_text(void);
static void tray_remove_item(const char *service);
static void rendertrayicons(Monitor *m, int bar_height);
static int tray_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static int resolve_asset_path(const char *path, char *out, size_t len);
static void tray_init(void);
static TrayItem *tray_first_item(void);
static void tray_item_activate(TrayItem *it, int button, int context_menu, int x, int y);
static void requestdecorationmode(struct wl_listener *listener, void *data);
static void requeststartdrag(struct wl_listener *listener, void *data);
static void requestmonstate(struct wl_listener *listener, void *data);
static void resize(Client *c, struct wlr_box geo, int interact);
static void run(const char *startup_cmd);
static void set_adaptive_sync(Monitor *m, int enabled);
static void setcursor(struct wl_listener *listener, void *data);
static void setcursorshape(struct wl_listener *listener, void *data);
static void setfloating(Client *c, int floating);
static void setfullscreen(Client *c, int fullscreen);
static void setsticky(Client *c, int sticky);
static void setlayout(const Arg *arg);
static void setmfact(const Arg *arg);
static void setmon(Client *c, Monitor *m, uint32_t newtags);
static void setpsel(struct wl_listener *listener, void *data);
static void setsel(struct wl_listener *listener, void *data);
static void setup(void);
static void spawn(const Arg *arg);
static void startdrag(struct wl_listener *listener, void *data);
static void tag(const Arg *arg);
static void tagmon(const Arg *arg);
static void tile(Monitor *m);
static void togglefloating(const Arg *arg);
static void togglefullscreen(const Arg *arg);
static void togglefullscreenadaptivesync(const Arg *arg);
static void togglesticky(const Arg *arg);
static void togglegaps(const Arg *arg);
static void togglestatusbar(const Arg *arg);
static void toggletag(const Arg *arg);
static void toggleview(const Arg *arg);
static void modal_show(const Arg *arg);
static void modal_render(Monitor *m);
static void modal_hide(Monitor *m);
static void modal_hide_all(void);
static Monitor *modal_visible_monitor(void);
static void modal_layout_metrics(int *btn_h, int *field_h, int *line_h, int *pad);
static int modal_max_visible_lines(Monitor *m);
static void modal_ensure_selection_visible(Monitor *m);
static int modal_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym);
static void modal_file_search_clear_results(Monitor *m);
static void modal_file_search_stop(Monitor *m);
static void modal_file_search_start_mode(Monitor *m, int fallback);
static void modal_file_search_start(Monitor *m);
static int modal_file_search_event(int fd, uint32_t mask, void *data);
static void modal_truncate_to_width(const char *src, char *dst, size_t len, int max_px);
static void shorten_path_display(const char *full, char *out, size_t len);
static int desktop_entry_cmp_used(const void *a, const void *b);
static int __attribute__((unused)) modal_match_name(const char *haystack, const char *needle);
static void modal_update_results(Monitor *m);
static void modal_prewarm(Monitor *m);
static void ensure_desktop_entries_loaded(void);
static void ensure_shell_env(void);
static int cpu_popup_refresh_timeout(void *data);
static void schedule_cpu_popup_refresh(uint32_t ms);
static uint64_t monotonic_msec(void);
static void rendercpu(StatusModule *module, int bar_height, const char *text);
static void rendercpupopup(Monitor *m);
static int cpu_popup_clamped_x(Monitor *m, CpuPopup *p);
static int cpu_popup_hover_index(Monitor *m, CpuPopup *p);
static int cpu_proc_cmp(const void *a, const void *b);
static int kill_processes_with_name(const char *name);
static int cpu_proc_is_critical(pid_t pid, const char *name);
static int read_top_cpu_processes(CpuPopup *p);
static int cpu_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button);
static void renderram(StatusModule *module, int bar_height, const char *text);
static void refreshstatuscpu(void);
static void refreshstatusram(void);
static int handlestatusscroll(struct wlr_pointer_axis_event *event);
static int scrollsteps(const struct wlr_pointer_axis_event *event);
static int updatestatuscpu(void *data);
static int updatestatusclock(void *data);
static int updatehoverfade(void *data);
static void schedule_hover_timer(void);
static int readcpustats(struct CpuSample *out, int maxcount);
static int readmeminfo(unsigned long long *total_kb, unsigned long long *avail_kb);
static int set_backlight_percent(double percent);
static int set_backlight_relative(double delta_percent);
static int set_pipewire_volume(double percent);
static int set_pipewire_mute(int mute);
static int toggle_pipewire_mute(void);
static int pipewire_sink_is_headset(void);
static int set_pipewire_mic_volume(double percent);
static int set_pipewire_mic_mute(int mute);
static int toggle_pipewire_mic_mute(void);
static double pipewire_mic_volume_percent(void);
static void updatecpuhover(Monitor *m, double cx, double cy);
static void set_status_task_due(void (*fn)(void), uint64_t due_ms);
static double net_bytes_to_rate(unsigned long long cur, unsigned long long prev,
		double elapsed);
static void fix_tray_argb32(uint32_t *pixels, size_t count, int use_rgba_order);
static double ramused_mb(void);
static double cpuaverage(void);
static double battery_percent(void);
static struct xkb_rule_names getxkbrules(void);
static double backlight_percent(void);
static double pipewire_volume_percent(int *is_headset_out);
static int findbacklightdevice(char *brightness_path, size_t brightness_len,
		char *max_path, size_t max_len);
static int set_backlight_relative(double delta_percent);
static void apply_startup_defaults(void);
static int findbatterydevice(char *capacity_path, size_t capacity_len);
static void schedule_status_timer(void);
static void schedule_next_status_refresh(void);
static struct wlr_buffer *statusbar_scaled_buffer_from_argb32_raw(const uint32_t *data,
		int width, int height, int target_h);
static struct wlr_buffer *statusbar_buffer_from_argb32_raw(const uint32_t *data, int width, int height);
static struct wlr_buffer *statusbar_buffer_from_glyph(const struct fcft_glyph *glyph);
static struct wlr_buffer *statusbar_scaled_buffer_from_argb32(const uint32_t *data,
		int width, int height, int target_h);
static void unlocksession(struct wl_listener *listener, void *data);
static void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
static void unmapnotify(struct wl_listener *listener, void *data);
static void updatemons(struct wl_listener *listener, void *data);
static void updatetitle(struct wl_listener *listener, void *data);
static void urgent(struct wl_listener *listener, void *data);
static void view(const Arg *arg);
static void virtualkeyboard(struct wl_listener *listener, void *data);
static void virtualpointer(struct wl_listener *listener, void *data);
static void warpcursor(const Client *c);
static const char *pick_resize_handle(const Client *c, double cx, double cy);
static const char *resize_cursor_from_dirs(int dx, int dy);
static LayoutNode *closest_split_node(LayoutNode *client_node, int want_vert,
        double pointer, double *out_ratio, struct wlr_box *out_box,
        double *out_dist);
static void apply_resize_axis_choice(void);
static int resize_should_update(uint32_t time);
static Monitor *xytomon(double x, double y);
static void xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny);
static void zoom(const Arg *arg);
static void rotate_clients(const Arg *arg);
static int __attribute__((unused)) node_contains_client(LayoutNode *node, Client *c);
static int subtree_bounds(LayoutNode *node, Monitor *m, struct wlr_box *out);
static __attribute__((unused)) LayoutNode *ancestor_split(LayoutNode *node, int want_vert);
static struct wlr_output_mode *bestmode(struct wlr_output *output);
static void btrtile(Monitor *m);
static void setratio_h(const Arg *arg);
static void setratio_v(const Arg *arg);
static void swapclients(const Arg *arg);

/* variables */
static pid_t child_pid = -1;
static int locked;
static void *exclusive_focus;
static struct wl_display *dpy;
static struct wl_event_loop *event_loop;
static struct wlr_backend *backend;
static struct wlr_scene *scene;
static struct wlr_scene_tree *layers[NUM_LAYERS];
static struct wlr_scene_tree *drag_icon;
/* Map from ZWLR_LAYER_SHELL_* constants to Lyr* enum */
static const int layermap[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };
static struct wlr_renderer *drw;
static struct wlr_allocator *alloc;
static struct wlr_compositor *compositor;
static struct wlr_session *session;
static struct StatusFont statusfont;
static int fcft_initialized;
static int has_dwl_session_target_cached = -1;
static void hidetagthumbnail(Monitor *m);

static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xdg_activation_v1 *activation;
static struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
static struct wl_list clients; /* tiling order */
static struct wl_list fstack;  /* focus order */
static struct wlr_idle_notifier_v1 *idle_notifier;
static struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_output_manager_v1 *output_mgr;
static struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
static struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
static struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
static struct wlr_output_power_manager_v1 *power_mgr;

static struct wlr_pointer_constraints_v1 *pointer_constraints;
static struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
static struct wlr_pointer_constraint_v1 *active_constraint;

static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;

static struct wlr_scene_rect *root_bg;
static struct wlr_session_lock_manager_v1 *session_lock_mgr;
static struct wlr_scene_rect *locked_bg;
static struct wlr_session_lock_v1 *cur_lock;

static struct wlr_seat *seat;
static KeyboardGroup *kb_group;
static unsigned int cursor_mode;
static Client *grabc;
static int grabcx, grabcy; /* client-relative */
static int resize_dir_x, resize_dir_y;
static double resize_start_ratio_v, resize_start_ratio_h;
static int resize_use_v, resize_use_h;
static uint32_t resize_last_time;
static double resize_last_x, resize_last_y;

static struct wlr_output_layout *output_layout;
static struct wlr_box sgeom;
static struct wl_list mons;
static Monitor *selmon;

static struct wlr_box resize_start_box_v;
static struct wlr_box resize_start_box_h;
static struct wlr_box resize_start_box_f;
static double resize_start_x, resize_start_y;
static LayoutNode *resize_split_node;
static LayoutNode *resize_split_node_h;
static int fullscreen_adaptive_sync_enabled = 1;
static struct wl_event_source *status_timer;
static struct wl_event_source *status_cpu_timer;
static struct wl_event_source *status_hover_timer;
static StatusRefreshTask status_tasks[] = {
	{ refreshstatuscpu, 0 },
	{ refreshstatusram, 0 },
	{ refreshstatuslight, 0 },
	{ refreshstatusmic, 0 },
	{ refreshstatusvolume, 0 },
	{ refreshstatusbattery, 0 },
	{ refreshstatusnet, 0 },
	{ refreshstatusicons, 0 },
};
static int status_rng_seeded;
static pid_t public_ip_pid = -1;
static int public_ip_fd = -1;
static struct wl_event_source *public_ip_event = NULL;
static char public_ip_buf[128];
static size_t public_ip_len = 0;
static pid_t ssid_pid = -1;
static int ssid_fd = -1;
static struct wl_event_source *ssid_event = NULL;
static char ssid_buf[256];
static size_t ssid_len = 0;
static time_t ssid_last_time = 0;
static int tray_anchor_x = -1;
static int tray_anchor_y = -1;
static uint64_t tray_anchor_time_ms;
#define MAX_CPU_CORES 256
static struct CpuSample cpu_prev[MAX_CPU_CORES];
static int cpu_prev_count;
static double cpu_last_percent = -1.0;
static char cpu_text[32] = "--%";
static double ram_last_mb = -1.0;
static char ram_text[32] = "--";
static double battery_last_percent = -1.0;
static char battery_text[32] = "--%";
static double net_last_down_bps = -1.0;
static double net_last_up_bps = -1.0;
static char last_clock_render[32];
static char last_cpu_render[32];
static char last_ram_render[32];
static char last_light_render[32];
static char last_volume_render[32];
static char last_mic_render[32];
static char last_battery_render[32];
static char last_net_render[64];
static int last_clock_h;
static int last_cpu_h;
static int last_ram_h;
static int last_light_h;
static int last_volume_h;
static int last_mic_h;
static int last_battery_h;
static int last_net_h;
static int battery_path_initialized;
static int backlight_paths_initialized;
static uint64_t volume_last_read_speaker_ms;
static uint64_t volume_last_read_headset_ms;
static double volume_cached_speaker = -1.0;
static double volume_cached_headset = -1.0;
static int volume_cached_speaker_muted = -1;
static int volume_cached_headset_muted = -1;
static uint64_t mic_last_read_ms;
static double mic_cached = -1.0;
static int mic_cached_muted = -1;
static uint64_t last_pointer_motion_ms;
static const float *volume_text_color = NULL; /* set at runtime; defaults to statusbar_fg */
static const float *mic_text_color = NULL;
static const float *statusbar_fg_override = NULL;
static pid_t wifi_scan_pid = -1;
static int wifi_scan_fd = -1;
static struct wl_event_source *wifi_scan_event = NULL;
static struct wl_event_source *wifi_scan_timer = NULL;
static struct wl_event_source *cpu_popup_refresh_timer = NULL;
static char wifi_scan_buf[8192];
static size_t wifi_scan_len = 0;
static int wifi_scan_inflight;
static unsigned int wifi_networks_generation;
static unsigned int wifi_scan_generation;
static int wifi_networks_accept_updates;
static int wifi_networks_freeze_existing;
static struct wl_list wifi_networks; /* WifiNetwork */
static int wifi_networks_initialized;
static char net_text[64] = "Net: --";
static char net_local_ip[64] = "--";
static char net_public_ip[64] = "--";
static char net_down_text[32] = "--";
static char net_up_text[32] = "--";
static char net_ssid[64] = "--";
static double net_last_wifi_quality = -1.0;
static int net_link_speed_mbps = -1;
static char net_iface[64] = {0};
static char net_prev_iface[64] = {0};
static int net_is_wireless;
static int net_available;
static char net_icon_path[PATH_MAX] = "images/svg/no_connection.svg";
static char net_icon_loaded_path[PATH_MAX];
static char cpu_icon_path[PATH_MAX] = "images/svg/cpu.svg";
static char cpu_icon_loaded_path[PATH_MAX];
static const uint32_t cpu_popup_refresh_interval_ms = 10000;
static char light_icon_path[PATH_MAX] = "images/svg/light.svg";
static char light_icon_loaded_path[PATH_MAX];
static char ram_icon_path[PATH_MAX] = "images/svg/ram.svg";
static char ram_icon_loaded_path[PATH_MAX];
static char battery_icon_path[PATH_MAX] = "images/svg/battery-100.svg";
static char battery_icon_loaded_path[PATH_MAX];
static char mic_icon_path[PATH_MAX] = "images/svg/microphone.svg";
static char mic_icon_loaded_path[PATH_MAX];
static char volume_icon_path[PATH_MAX] = "images/svg/speaker_100.svg";
static char volume_icon_loaded_path[PATH_MAX];
static const char net_icon_no_conn[] = "images/svg/no_connection.svg";
static const char net_icon_eth[] = "images/svg/ethernet.svg";
static const char net_icon_wifi_100[] = "images/svg/wifi_100.svg";
static const char net_icon_wifi_75[] = "images/svg/wifi_75.svg";
static const char net_icon_wifi_50[] = "images/svg/wifi_50.svg";
static const char net_icon_wifi_25[] = "images/svg/wifi_25.svg";
static const char battery_icon_25[] = "images/svg/battery-25.svg";
static const char battery_icon_50[] = "images/svg/battery-50.svg";
static const char battery_icon_75[] = "images/svg/battery-75.svg";
static const char battery_icon_100[] = "images/svg/battery-100.svg";
static const char volume_icon_speaker_25[] = "images/svg/speaker_25.svg";
static const char volume_icon_speaker_50[] = "images/svg/speaker_50.svg";
static const char volume_icon_speaker_100[] = "images/svg/speaker_100.svg";
static const char volume_icon_speaker_muted[] = "images/svg/speaker_muted.svg";
static const char volume_icon_headset[] = "images/svg/headset.svg";
static const char volume_icon_headset_muted[] = "images/svg/headset_muted.svg";
static const char mic_icon_unmuted[] = "images/svg/microphone.svg";
static const char mic_icon_muted[] = "images/svg/microphone_muted.svg";
static char net_icon_wifi_100_resolved[PATH_MAX];
static char net_icon_wifi_75_resolved[PATH_MAX];
static char net_icon_wifi_50_resolved[PATH_MAX];
static char net_icon_wifi_25_resolved[PATH_MAX];
static char net_icon_eth_resolved[PATH_MAX];
static char net_icon_no_conn_resolved[PATH_MAX];
static char clock_icon_path[PATH_MAX] = "images/svg/clock.svg";
static char clock_icon_loaded_path[PATH_MAX];
static int net_icon_loaded_h;
static int net_icon_w;
static int net_icon_h;
static struct wlr_buffer *net_icon_buf;
static int clock_icon_loaded_h;
static int clock_icon_w;
static int clock_icon_h;
static struct wlr_buffer *clock_icon_buf;
static int cpu_icon_loaded_h;
static int cpu_icon_w;
static int cpu_icon_h;
static struct wlr_buffer *cpu_icon_buf;
static int light_icon_loaded_h;
static int light_icon_w;
static int light_icon_h;
static struct wlr_buffer *light_icon_buf;
static int ram_icon_loaded_h;
static int ram_icon_w;
static int ram_icon_h;
static struct wlr_buffer *ram_icon_buf;
static int battery_icon_loaded_h;
static int battery_icon_w;
static int battery_icon_h;
static struct wlr_buffer *battery_icon_buf;
static int mic_icon_loaded_h;
static int mic_icon_w;
static int mic_icon_h;
static struct wlr_buffer *mic_icon_buf;
static int volume_icon_loaded_h;
static int volume_icon_w;
static int volume_icon_h;
static struct wlr_buffer *volume_icon_buf;
static unsigned long long net_prev_rx;
static unsigned long long net_prev_tx;
static struct timespec net_prev_ts;
static int net_prev_valid;
static time_t net_public_ip_last;
static sd_bus *tray_bus;
static struct wl_event_source *tray_event;
static sd_bus_slot *tray_vtable_slot;
static sd_bus_slot *tray_fdo_vtable_slot;
static sd_bus_slot *tray_name_slot;
static int tray_host_registered;
static struct wl_list tray_items;
static double light_last_percent = -1.0;
static double light_cached_percent = -1.0;
static char light_text[32] = "--%";
static double mic_last_percent = 80.0;
static char mic_text[32] = "--%";
static double volume_last_speaker_percent = 70.0;
static double volume_last_headset_percent = 70.0;
static double speaker_active = 70.0;
static double speaker_stored = 70.0;
static double microphone_active = 80.0;
static double microphone_stored = 80.0;
static char volume_text[32] = "--%";
static int volume_muted = -1;
static int mic_muted = -1;
static int mic_last_color_is_muted = -1;
static int volume_last_color_is_muted = -1;
static char backlight_brightness_path[PATH_MAX];
static char backlight_max_path[PATH_MAX];
static int backlight_available;
static int backlight_writable;
static char battery_capacity_path[PATH_MAX];
static int battery_available;
static const double light_step = 5.0;
static const double volume_step = 3.0;
static const double volume_max_percent = 150.0;
static const double mic_step = 3.0;
static const double mic_max_percent = 150.0;
static double cpu_last_core_percent[MAX_CPU_CORES];
static int cpu_core_count;
static char sysicons_text[64] = "Tray";
static DesktopEntry desktop_entries[4096];
static int desktop_entry_count = 0;
static int desktop_entries_loaded = 0;

/* global event handlers */
static struct wl_listener cursor_axis = {.notify = axisnotify};
static struct wl_listener cursor_button = {.notify = buttonpress};
static struct wl_listener cursor_frame = {.notify = cursorframe};
static struct wl_listener cursor_motion = {.notify = motionrelative};
static struct wl_listener cursor_motion_absolute = {.notify = motionabsolute};
static struct wl_listener gpu_reset = {.notify = gpureset};
static struct wl_listener layout_change = {.notify = updatemons};
static struct wl_listener new_idle_inhibitor = {.notify = createidleinhibitor};
static struct wl_listener new_input_device = {.notify = inputdevice};
static struct wl_listener new_virtual_keyboard = {.notify = virtualkeyboard};
static struct wl_listener new_virtual_pointer = {.notify = virtualpointer};
static struct wl_listener new_pointer_constraint = {.notify = createpointerconstraint};
static struct wl_listener new_output = {.notify = createmon};
static struct wl_listener new_xdg_toplevel = {.notify = createnotify};
static struct wl_listener new_xdg_popup = {.notify = createpopup};
static struct wl_listener new_xdg_decoration = {.notify = createdecoration};
static struct wl_listener new_layer_surface = {.notify = createlayersurface};
static struct wl_listener output_mgr_apply = {.notify = outputmgrapply};
static struct wl_listener output_mgr_test = {.notify = outputmgrtest};
static struct wl_listener output_power_mgr_set_mode = {.notify = powermgrsetmode};
static struct wl_listener request_activate = {.notify = urgent};
static struct wl_listener request_cursor = {.notify = setcursor};
static struct wl_listener request_set_psel = {.notify = setpsel};
static struct wl_listener request_set_sel = {.notify = setsel};
static struct wl_listener request_set_cursor_shape = {.notify = setcursorshape};
static struct wl_listener request_start_drag = {.notify = requeststartdrag};
static struct wl_listener start_drag = {.notify = startdrag};
static struct wl_listener new_session_lock = {.notify = locksession};

#ifdef XWAYLAND
static void activatex11(struct wl_listener *listener, void *data);
static void associatex11(struct wl_listener *listener, void *data);
static void configurex11(struct wl_listener *listener, void *data);
static void createnotifyx11(struct wl_listener *listener, void *data);
static void dissociatex11(struct wl_listener *listener, void *data);
static void minimizenotify(struct wl_listener *listener, void *data);
static void sethints(struct wl_listener *listener, void *data);
static void xwaylandready(struct wl_listener *listener, void *data);
static struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
static struct wl_listener xwayland_ready = {.notify = xwaylandready};
static struct wlr_xwayland *xwayland;
#endif

#ifndef WLR_SILENCE
#define WLR_SILENCE (WLR_ERROR - 1)
#endif

/* configuration, allows nested code to access above variables */
#include "config.h"

/* attempt to encapsulate suck into one file */
#include "client.h"
#include "btrtile.c"

static int
has_dwl_session_target(void)
{
	const char *paths[] = {
		"/etc/systemd/user/dwl-session.target",
		"/usr/lib/systemd/user/dwl-session.target",
		"/lib/systemd/user/dwl-session.target",
		NULL
	};

	if (has_dwl_session_target_cached >= 0)
		return has_dwl_session_target_cached;

	for (int i = 0; paths[i]; i++) {
		if (access(paths[i], F_OK) == 0) {
			has_dwl_session_target_cached = 1;
			return 1;
		}
	}
	has_dwl_session_target_cached = 0;
	return 0;
}

static void
hidetagthumbnail(Monitor *m)
{
	(void)m;
}

/* function implementations */
void
applybounds(Client *c, struct wlr_box *bbox)
{
	/* set minimum possible */
	c->geom.width = MAX(1 + 2 * (int)c->bw, c->geom.width);
	c->geom.height = MAX(1 + 2 * (int)c->bw, c->geom.height);

	if (c->geom.x >= bbox->x + bbox->width)
		c->geom.x = bbox->x + bbox->width - c->geom.width;
	if (c->geom.y >= bbox->y + bbox->height)
		c->geom.y = bbox->y + bbox->height - c->geom.height;
	if (c->geom.x + c->geom.width <= bbox->x)
		c->geom.x = bbox->x;
	if (c->geom.y + c->geom.height <= bbox->y)
		c->geom.y = bbox->y;
}

void
applyrules(Client *c)
{
	/* rule matching */
	const char *appid, *title;
	uint32_t newtags = 0;
	int i;
	const Rule *r;
	Monitor *mon = selmon, *m;

	appid = client_get_appid(c);
	title = client_get_title(c);

	for (r = rules; r < END(rules); r++) {
		if ((!r->title || strstr(title, r->title))
				&& (!r->id || strstr(appid, r->id))) {
			c->isfloating = r->isfloating;
			newtags |= r->tags;
			i = 0;
			wl_list_for_each(m, &mons, link) {
				if (r->monitor == i++)
					mon = m;
			}
		}
	}

	if (mon) {
		c->geom.x = (mon->w.width - c->geom.width) / 2 + mon->m.x;
		c->geom.y = (mon->w.height - c->geom.height) / 2 + mon->m.y;
	}

	c->isfloating |= client_is_float_type(c);
	setmon(c, mon, newtags);
}

static void
pixman_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
	struct PixmanBuffer *buf = wl_container_of(wlr_buffer, buf, base);

	if (!buf)
		return;
	if (buf->image)
		pixman_image_unref(buf->image);
	if (buf->owns_data && buf->data)
		free(buf->data);
	free(buf);
}

static bool
pixman_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride)
{
	struct PixmanBuffer *buf = wl_container_of(wlr_buffer, buf, base);

	(void)flags;
	if (!buf || !data || !format || !stride)
		return false;

	*data = buf->data;
	*format = buf->drm_format;
	*stride = buf->stride;
	return true;
}

static void
pixman_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
	(void)wlr_buffer;
}

static bool
pixman_buffer_get_dmabuf(struct wlr_buffer *wlr_buffer,
		struct wlr_dmabuf_attributes *attribs)
{
	(void)wlr_buffer;
	(void)attribs;
	return false;
}

static bool
pixman_buffer_get_shm(struct wlr_buffer *wlr_buffer,
		struct wlr_shm_attributes *attribs)
{
	(void)wlr_buffer;
	(void)attribs;
	return false;
}

static const struct wlr_buffer_impl pixman_buffer_impl = {
	.destroy = pixman_buffer_destroy,
	.get_dmabuf = pixman_buffer_get_dmabuf,
	.get_shm = pixman_buffer_get_shm,
	.begin_data_ptr_access = pixman_buffer_begin_data_ptr_access,
	.end_data_ptr_access = pixman_buffer_end_data_ptr_access,
};

static struct wlr_buffer *
statusbar_buffer_from_glyph(const struct fcft_glyph *glyph)
{
	pixman_image_t *dst, *solid = NULL;
	struct PixmanBuffer *buf;
	pixman_color_t col;
	uint32_t *data;
	int width, height, stride, force_color;

	if (!glyph || !glyph->pix)
		return NULL;

	width = glyph->width;
	height = glyph->height;
	if (width <= 0 || height <= 0)
		return NULL;

	stride = width * 4;
	data = ecalloc(height, stride);
	if (!data)
		return NULL;

	dst = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height,
			data, stride);
	if (!dst) {
		free(data);
		return NULL;
	}

	force_color = statusbar_font_force_color || pixman_image_get_format(glyph->pix) == PIXMAN_a8;
	if (force_color) {
		const float *fg = statusbar_fg_override ? statusbar_fg_override : statusbar_fg;
		col.alpha = (uint16_t)lroundf(fg[3] * 65535.0f);
		col.red = (uint16_t)lroundf(fg[0] * 65535.0f);
		col.green = (uint16_t)lroundf(fg[1] * 65535.0f);
		col.blue = (uint16_t)lroundf(fg[2] * 65535.0f);
		solid = pixman_image_create_solid_fill(&col);
		pixman_image_composite32(PIXMAN_OP_SRC, solid, glyph->pix, dst,
				0, 0, 0, 0, 0, 0, width, height);
	} else {
		pixman_image_composite32(PIXMAN_OP_SRC, glyph->pix, NULL, dst,
				0, 0, 0, 0, 0, 0, width, height);
	}

	if (solid)
		pixman_image_unref(solid);

	buf = ecalloc(1, sizeof(*buf));
	if (!buf) {
		pixman_image_unref(dst);
		free(data);
		return NULL;
	}

	buf->image = dst;
	buf->data = data;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, width, height);

	return &buf->base;
}

static void
clearstatusmodule(StatusModule *module)
{
	struct wlr_scene_node *node, *tmp;

	if (!module || !module->tree)
		return;

	wl_list_for_each_safe(node, tmp, &module->tree->children, link) {
		if (module->bg && node == &module->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}
}

static void
updatemodulebg(StatusModule *module, int width, int height,
		const float color[static 4])
{
	struct wlr_scene_node *node, *tmp;
	int radius, y, inset_top, inset_bottom, inset, start, h, w;
	struct wlr_scene_rect *r;

	if (!module || !module->tree || width <= 0 || height <= 0)
		return;

	if (!module->bg && !(module->bg = wlr_scene_tree_create(module->tree)))
		return;
	wlr_scene_node_set_enabled(&module->bg->node, 1);
	wlr_scene_node_set_position(&module->bg->node, 0, 0);

	wl_list_for_each_safe(node, tmp, &module->bg->children, link)
		wlr_scene_node_destroy(node);

	radius = MIN(4, MIN(width, height) / 4);

	y = 0;
	while (y < height) {
		inset_top = radius ? MAX(0, radius - y) : 0;
		inset_bottom = radius ? MAX(0, radius - ((height - 1) - y)) : 0;
		inset = MAX(inset_top, inset_bottom);
		start = y;

		while (y < height) {
			inset_top = radius ? MAX(0, radius - y) : 0;
			inset_bottom = radius ? MAX(0, radius - ((height - 1) - y)) : 0;
			if (MAX(inset_top, inset_bottom) != inset)
				break;
			y++;
		}

		h = y - start;
		w = width - 2 * inset;
		if (w < 0)
			w = 0;

		if (h > 0 && w > 0) {
			r = wlr_scene_rect_create(module->bg, w, h, color);
			if (r)
				wlr_scene_node_set_position(&r->node, inset, start);
		}
	}

	wlr_scene_node_lower_to_bottom(&module->bg->node);
}

static void
renderclock(StatusModule *module, int bar_height, const char *text)
{
	render_icon_label(module, bar_height, text,
			ensure_clock_icon_buffer, &clock_icon_buf, &clock_icon_w, &clock_icon_h,
			0, statusbar_icon_text_gap_clock, statusbar_fg);
}

static void
render_icon_label(StatusModule *module, int bar_height, const char *text,
		int (*ensure_icon)(int target_h), struct wlr_buffer **icon_buf,
		int *icon_w, int *icon_h, int min_text_w, int icon_gap,
		const float text_color[static 4])
{
	int padding = statusbar_module_padding;
	int text_w = 0;
	int x;
	int iw = 0, ih = 0;
	int target_h;
	int scaled_target_h;
	struct wlr_scene_buffer *scene_buf;
	const float *fg = text_color ? text_color : statusbar_fg;

	if (!module || !module->tree)
		return;

	clearstatusmodule(module);

	if (bar_height <= 0) {
		module->width = 0;
		return;
	}

	target_h = bar_height - 2 * padding;
	if (target_h <= 0)
		target_h = bar_height;
	scaled_target_h = target_h;
	if (status_icon_scale > 0.0) {
		double scaled = (double)target_h * status_icon_scale;
		if (scaled > (double)INT_MAX)
			scaled = (double)INT_MAX;
		scaled_target_h = (int)lround(scaled);
		if (scaled_target_h <= 0)
			scaled_target_h = target_h;
	}

	if (ensure_icon && icon_buf && icon_w && icon_h
			&& ensure_icon(scaled_target_h) == 0
			&& *icon_buf && *icon_w > 0 && *icon_h > 0) {
		iw = *icon_w;
		ih = *icon_h;
	}

	if (text && *text)
		text_w = status_text_width(text);
	if (min_text_w > text_w)
		text_w = min_text_w;
	if (icon_gap <= 0)
		icon_gap = statusbar_icon_text_gap;

	module->width = 2 * padding + text_w;
	if (iw > 0)
		module->width += iw + (text_w > 0 ? icon_gap : padding);
	if (module->width < 2 * padding)
		module->width = 2 * padding;

	updatemodulebg(module, module->width, bar_height, statusbar_bg);
	x = padding;

	if (iw > 0 && icon_buf && *icon_buf) {
		scene_buf = wlr_scene_buffer_create(module->tree, NULL);
		if (scene_buf) {
			int icon_y = (bar_height - ih) / 2;
			wlr_scene_buffer_set_buffer(scene_buf, *icon_buf);
			wlr_scene_node_set_position(&scene_buf->node, x, icon_y);
		}
		x += iw + (text_w > 0 ? icon_gap : padding);
	}

	if (text_w > 0)
		tray_render_label(module, text, x, bar_height, fg);
}

static int
status_text_width(const char *text)
{
	int pen_x = 0;
	uint32_t prev_cp = 0;

	if (!text || !*text)
		return 0;

	if (!statusfont.font)
		return (int)strlen(text) * 8;

	for (int i = 0; text[i]; i++) {
		long kern_x = 0, kern_y = 0;
		uint32_t cp = (unsigned char)text[i];
		const struct fcft_glyph *glyph;

		if (prev_cp)
			fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
		pen_x += (int)kern_x;

		glyph = fcft_rasterize_char_utf32(statusfont.font, cp,
				statusbar_font_subpixel);
		if (glyph && glyph->pix) {
			pen_x += glyph->advance.x;
			if (text[i + 1])
				pen_x += statusbar_font_spacing;
		}
		prev_cp = cp;
	}

	return pen_x;
}

static int
tray_render_label(StatusModule *module, const char *text, int x, int bar_height,
		const float color[static 4])
{
	tll(struct GlyphRun) glyphs = tll_init();
	const struct fcft_glyph *glyph;
	struct wlr_scene_buffer *scene_buf;
	struct wlr_buffer *buffer;
	uint32_t prev_cp = 0;
	int pen_x = 0;
	int min_y = INT_MAX, max_y = INT_MIN;
	int text_width, text_height, origin_y;
	size_t i;
	const float *prev_fg = statusbar_fg_override;

	if (!module || !module->tree || !text || !*text || bar_height <= 0)
		return 0;

	statusbar_fg_override = color;
	if (!statusfont.font) {
		int w = status_text_width(text);
		if (w <= 0)
			w = statusbar_font_spacing;
		statusbar_fg_override = prev_fg;
		return w;
	}

	for (i = 0; text[i]; i++) {
		long kern_x = 0, kern_y = 0;
		uint32_t cp = (unsigned char)text[i];

		if (prev_cp)
			fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
		pen_x += (int)kern_x;

		glyph = fcft_rasterize_char_utf32(statusfont.font, cp,
				statusbar_font_subpixel);
		if (glyph && glyph->pix) {
			tll_push_back(glyphs, ((struct GlyphRun){
				.glyph = glyph,
				.pen_x = pen_x,
				.codepoint = cp,
			}));
			if (-glyph->y < min_y)
				min_y = -glyph->y;
			if (-glyph->y + glyph->height > max_y)
				max_y = -glyph->y + glyph->height;
			pen_x += glyph->advance.x;
			if (text[i + 1])
				pen_x += statusbar_font_spacing;
		}
		prev_cp = cp;
	}

	if (tll_length(glyphs) == 0) {
		tll_free(glyphs);
		statusbar_fg_override = prev_fg;
		return 0;
	}

	text_width = status_text_width(text);
	text_height = max_y - min_y;
	origin_y = (bar_height - text_height) / 2 - min_y;

	tll_foreach(glyphs, it) {
		glyph = it->item.glyph;
		buffer = statusbar_buffer_from_glyph(glyph);
		if (!buffer)
			continue;

		scene_buf = wlr_scene_buffer_create(module->tree, NULL);
		if (scene_buf) {
			wlr_scene_buffer_set_buffer(scene_buf, buffer);
			wlr_scene_node_set_position(&scene_buf->node,
					x + it->item.pen_x + glyph->x,
					origin_y - glyph->y);
		}
		wlr_buffer_drop(buffer);
	}

	tll_free(glyphs);
	statusbar_fg_override = prev_fg;
	return text_width;
}

static void
rendercpu(StatusModule *module, int bar_height, const char *text)
{
	render_icon_label(module, bar_height, text,
			ensure_cpu_icon_buffer, &cpu_icon_buf, &cpu_icon_w, &cpu_icon_h,
			0, statusbar_icon_text_gap_cpu, statusbar_fg);
}

static void
renderlight(StatusModule *module, int bar_height, const char *text)
{
	if (!module || !module->tree) {
		return;
	}

	if (!backlight_available) {
		clearstatusmodule(module);
		module->width = 0;
		wlr_scene_node_set_enabled(&module->tree->node, 0);
		return;
	}

	render_icon_label(module, bar_height, text,
			ensure_light_icon_buffer, &light_icon_buf, &light_icon_w, &light_icon_h,
			0, statusbar_icon_text_gap_light, statusbar_fg);
}

static void
rendernet(StatusModule *module, int bar_height, const char *text)
{
	/* Net text module is disabled; keep node hidden */
	if (module && module->tree) {
		clearstatusmodule(module);
		module->width = 0;
		wlr_scene_node_set_enabled(&module->tree->node, 0);
	}
}

static void
renderbattery(StatusModule *module, int bar_height, const char *text)
{
	if (!battery_available) {
		if (module && module->tree) {
			clearstatusmodule(module);
			module->width = 0;
			wlr_scene_node_set_enabled(&module->tree->node, 0);
		}
		return;
	}

	render_icon_label(module, bar_height, text,
			ensure_battery_icon_buffer, &battery_icon_buf, &battery_icon_w, &battery_icon_h,
			status_text_width("100%"), statusbar_icon_text_gap_battery, statusbar_fg);
}

static void
renderram(StatusModule *module, int bar_height, const char *text)
{
	render_icon_label(module, bar_height, text,
			ensure_ram_icon_buffer, &ram_icon_buf, &ram_icon_w, &ram_icon_h,
			0, statusbar_icon_text_gap_ram, statusbar_fg);
}

static void
rendervolume(StatusModule *module, int bar_height, const char *text)
{
	render_icon_label(module, bar_height, text,
			ensure_volume_icon_buffer, &volume_icon_buf, &volume_icon_w, &volume_icon_h,
			status_text_width("100%"), statusbar_icon_text_gap_volume, volume_text_color);
}

static void
rendermic(StatusModule *module, int bar_height, const char *text)
{
	render_icon_label(module, bar_height, text,
			ensure_mic_icon_buffer, &mic_icon_buf, &mic_icon_w, &mic_icon_h,
			status_text_width("100%"), statusbar_icon_text_gap_microphone, mic_text_color);
}

static int
pathisdir(const char *path)
{
	struct stat st;

	return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int
pathisfile(const char *path)
{
	struct stat st;

	return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int
resolve_asset_path(const char *path, char *out, size_t len)
{
	static char exe_dir[PATH_MAX];
	static int exe_dir_cached = 0;
	char cand[PATH_MAX];
	const char *envdir;

	if (!path || !*path || !out || len == 0)
		return -1;

	if (path[0] == '/') {
		if (pathisfile(path)) {
			snprintf(out, len, "%s", path);
			return 0;
		}
		return -1;
	}

	if (pathisfile(path)) {
		snprintf(out, len, "%s", path);
		return 0;
	}

	envdir = getenv("DWL_ICON_DIR");
	if (envdir && *envdir) {
		if (snprintf(cand, sizeof(cand), "%s/%s", envdir, path) < (int)sizeof(cand)
				&& pathisfile(cand)) {
			snprintf(out, len, "%s", cand);
			return 0;
		}
	}

	if (!exe_dir_cached) {
		char *slash;
		ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
		if (n > 0 && (size_t)n < sizeof(exe_dir)) {
			exe_dir[n] = '\0';
			slash = strrchr(exe_dir, '/');
			if (slash)
				*slash = '\0';
			else
				exe_dir[0] = '\0';
		} else {
			exe_dir[0] = '\0';
		}
		exe_dir_cached = 1;
	}

	if (exe_dir[0]) {
		if (snprintf(cand, sizeof(cand), "%s/%s", exe_dir, path) < (int)sizeof(cand)
				&& pathisfile(cand)) {
			snprintf(out, len, "%s", cand);
			return 0;
		}
		if (snprintf(cand, sizeof(cand), "%s/../share/dwl/%s", exe_dir, path) < (int)sizeof(cand)
				&& pathisfile(cand)) {
			snprintf(out, len, "%s", cand);
			return 0;
		}
	}

	return -1;
}

static int
has_svg_extension(const char *path)
{
	size_t len;

	if (!path || !*path)
		return 0;

	len = strlen(path);
	if (len >= 4 && strcasecmp(path + len - 4, ".svg") == 0)
		return 1;
	if (len >= 5 && strcasecmp(path + len - 5, ".svgz") == 0)
		return 1;
	return 0;
}

static int
strip_symbolic_suffix(const char *name, char *out, size_t outlen)
{
	size_t len, suffix = strlen("-symbolic");

	if (!name || !out || outlen == 0)
		return 0;

	len = strlen(name);
	if (len > suffix && strcmp(name + len - suffix, "-symbolic") == 0) {
		size_t copylen = len - suffix;
		if (copylen >= outlen)
			copylen = outlen - 1;
		memcpy(out, name, copylen);
		out[copylen] = '\0';
		return 1;
	}
	return 0;
}

static void
add_icon_root_paths(const char *base, const char *themes[], size_t theme_count,
		char pathbufs[][PATH_MAX], size_t *pathcount, size_t max_paths)
{
	if (!base || !*base || !themes || !pathbufs || !pathcount)
		return;

	for (size_t i = 0; i < theme_count && *pathcount < max_paths; i++) {
		char themed[PATH_MAX];
		snprintf(themed, sizeof(themed), "%s/%s", base, themes[i]);
		snprintf(pathbufs[*pathcount], PATH_MAX, "%s", themed);
		(*pathcount)++;
	}

	if (*pathcount < max_paths) {
		snprintf(pathbufs[*pathcount], PATH_MAX, "%s", base);
		(*pathcount)++;
	}
}

static GdkPixbuf *
pixbuf_from_cairo_surface(cairo_surface_t *surface, int width, int height)
{
	GdkPixbuf *pixbuf;
	guchar *dst;
	const uint8_t *src;
	int dst_stride, src_stride;

	if (!surface || width <= 0 || height <= 0)
		return NULL;
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
		return NULL;

	cairo_surface_flush(surface);
	src = cairo_image_surface_get_data(surface);
	src_stride = cairo_image_surface_get_stride(surface);
	if (!src)
		return NULL;

	pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, 1, 8, width, height);
	if (!pixbuf)
		return NULL;
	dst = gdk_pixbuf_get_pixels(pixbuf);
	dst_stride = gdk_pixbuf_get_rowstride(pixbuf);

	for (int y = 0; y < height; y++) {
		const uint32_t *srow = (const uint32_t *)(src + (size_t)y * (size_t)src_stride);
		guchar *drow = dst + (size_t)y * (size_t)dst_stride;
		for (int x = 0; x < width; x++) {
			uint32_t s = srow[x];
			uint8_t a = (uint8_t)(s >> 24);
			uint8_t r = (uint8_t)(s >> 16);
			uint8_t g = (uint8_t)(s >> 8);
			uint8_t b = (uint8_t)s;

			if (a > 0) {
				/* Un-premultiply */
				r = (uint8_t)((((uint32_t)r) * 255u + a / 2u) / a);
				g = (uint8_t)((((uint32_t)g) * 255u + a / 2u) / a);
				b = (uint8_t)((((uint32_t)b) * 255u + a / 2u) / a);
			} else {
				r = g = b = 0;
			}

			drow[(size_t)x * 4] = r;
			drow[(size_t)x * 4 + 1] = g;
			drow[(size_t)x * 4 + 2] = b;
			drow[(size_t)x * 4 + 3] = a;
		}
	}

	return pixbuf;
}

static void
tray_consider_icon(const char *path, int size_hint, int desired_h,
		char *best_path, int *best_diff, int *found)
{
	int diff;

	if (!path || !*path || !best_path || !best_diff || !found)
		return;
	if (!pathisfile(path) || access(path, R_OK) != 0)
		return;

	if (size_hint > 0 && desired_h > 0)
		diff = abs(size_hint - desired_h);
	else if (desired_h > 0 && size_hint <= 0)
		diff = desired_h;
	else
		diff = 0;

	if (!*found || diff < *best_diff) {
		*best_diff = diff;
		snprintf(best_path, PATH_MAX, "%s", path);
		*found = 1;
	}
}

static struct wlr_buffer *
statusbar_buffer_from_pixbuf(GdkPixbuf *pixbuf, int target_h, int *out_w, int *out_h)
{
	GdkPixbuf *scaled = NULL;
	guchar *pixels;
	int w, h, nchan, stride;
	uint8_t *argb = NULL;
	size_t bufsize;
	struct wlr_buffer *buf = NULL;

	if (!pixbuf)
		return NULL;

	w = gdk_pixbuf_get_width(pixbuf);
	h = gdk_pixbuf_get_height(pixbuf);
	if (w <= 0 || h <= 0)
		goto out;

	if (target_h > 0 && h > target_h) {
		int target_w = (int)lround(((double)w * (double)target_h) / (double)h);
		if (target_w <= 0)
			target_w = 1;
		scaled = gdk_pixbuf_scale_simple(pixbuf, target_w, target_h, GDK_INTERP_BILINEAR);
		if (scaled) {
			g_object_unref(pixbuf);
			pixbuf = scaled;
			w = target_w;
			h = target_h;
		}
	}

	nchan = gdk_pixbuf_get_n_channels(pixbuf);
	if (nchan < 3)
		goto out;
	stride = gdk_pixbuf_get_rowstride(pixbuf);
	pixels = gdk_pixbuf_get_pixels(pixbuf);
	if (!pixels)
		goto out;

	bufsize = (size_t)w * (size_t)h * 4;
	argb = calloc(1, bufsize);
	if (!argb)
		goto out;

	for (int y = 0; y < h; y++) {
		const guchar *row = pixels + (size_t)y * (size_t)stride;
		for (int x = 0; x < w; x++) {
			const guchar *p = row + (size_t)x * (size_t)nchan;
			uint8_t r = p[0];
			uint8_t g = p[1];
			uint8_t b = p[2];
			uint8_t a = (nchan >= 4 && gdk_pixbuf_get_has_alpha(pixbuf)) ? p[3] : 255;
			size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 4;
			argb[idx] = a;
			argb[idx + 1] = r;
			argb[idx + 2] = g;
			argb[idx + 3] = b;
		}
	}

	buf = statusbar_buffer_from_argb32((const uint32_t *)argb, w, h);
	if (buf) {
		if (out_w)
			*out_w = w;
		if (out_h)
			*out_h = h;
	}

out:
	free(argb);
	g_object_unref(pixbuf);
	return buf;
}

static void
recolor_wifi100_pixbuf(GdkPixbuf *pixbuf)
{
	int w, h, nchan, stride;
	guchar *pixels;
	double fx, fy;
	int has_alpha;
	struct Rect {
		double x, y, w, h;
		uint8_t r, g, b;
	};
	static const struct Rect rects[] = {
		{ 6, 38, 10, 20, 0x82, 0xCC, 0x00 }, /* bar 1 main */
		{ 14, 38,  2, 20, 0x5E, 0x9E, 0x00 }, /* bar 1 stripe */
		{ 20, 30, 10, 28, 0x82, 0xCC, 0x00 }, /* bar 2 main */
		{ 28, 30,  2, 28, 0x5E, 0x9E, 0x00 }, /* bar 2 stripe */
		{ 34, 22, 10, 36, 0x82, 0xCC, 0x00 }, /* bar 3 main */
		{ 42, 22,  2, 36, 0x5E, 0x9E, 0x00 }, /* bar 3 stripe */
		{ 48, 14, 10, 44, 0x82, 0xCC, 0x00 }, /* bar 4 main */
		{ 56, 14,  2, 44, 0x5E, 0x9E, 0x00 }, /* bar 4 stripe */
	};

	if (!pixbuf)
		return;
	nchan = gdk_pixbuf_get_n_channels(pixbuf);
	if (nchan < 3)
		return;
	w = gdk_pixbuf_get_width(pixbuf);
	h = gdk_pixbuf_get_height(pixbuf);
	if (w <= 0 || h <= 0)
		return;
	stride = gdk_pixbuf_get_rowstride(pixbuf);
	pixels = gdk_pixbuf_get_pixels(pixbuf);
	if (!pixels)
		return;
	has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
	fx = (double)w / 64.0;
	fy = (double)h / 64.0;

	for (size_t i = 0; i < LENGTH(rects); i++) {
		int rx = (int)lround(rects[i].x * fx);
		int ry = (int)lround(rects[i].y * fy);
		int rw = (int)lround(rects[i].w * fx);
		int rh = (int)lround(rects[i].h * fy);
		if (rx < 0) rx = 0;
		if (ry < 0) ry = 0;
		if (rw < 1) rw = 1;
		if (rh < 1) rh = 1;
		if (rx + rw > w) rw = w - rx;
		if (ry + rh > h) rh = h - ry;
		for (int y = ry; y < ry + rh; y++) {
			guchar *row = pixels + (size_t)y * (size_t)stride;
			for (int x = rx; x < rx + rw; x++) {
				guchar *p = row + (size_t)x * (size_t)nchan;
				p[0] = rects[i].r;
				p[1] = rects[i].g;
				p[2] = rects[i].b;
				if (has_alpha && nchan >= 4)
					p[3] = 255;
			}
		}
	}
}

static struct wlr_buffer *
statusbar_buffer_from_wifi100(int target_h, int *out_w, int *out_h)
{
	struct {
		int x, y, w, h;
		uint8_t r, g, b;
	} bars[] = {
		{6, 38, 10, 20, 0x82, 0xCC, 0x00}, /* bar 1 main */
		{14, 38, 2, 20, 0x5E, 0x9E, 0x00}, /* bar 1 stripe */
		{20, 30, 10, 28, 0x82, 0xCC, 0x00}, /* bar 2 main */
		{28, 30, 2, 28, 0x5E, 0x9E, 0x00}, /* bar 2 stripe */
		{34, 22, 10, 36, 0x82, 0xCC, 0x00}, /* bar 3 main */
		{42, 22, 2, 36, 0x5E, 0x9E, 0x00}, /* bar 3 stripe */
		{48, 14, 10, 44, 0x82, 0xCC, 0x00}, /* bar 4 main */
		{56, 14, 2, 44, 0x5E, 0x9E, 0x00}, /* bar 4 stripe */
	};
	const int base_w = 64;
	const int base_h = 64;
	int target_w, target_h_final;
	uint32_t *buf = NULL;
	struct wlr_buffer *wb = NULL;

	target_h_final = (target_h > 0) ? target_h : base_h;
	if (target_h_final <= 0)
		return NULL;
	target_w = (int)lround(((double)base_w * (double)target_h_final) / (double)base_h);
	if (target_w <= 0)
		target_w = 1;

	buf = calloc(1, (size_t)base_w * (size_t)base_h * sizeof(uint32_t));
	if (!buf)
		return NULL;

	for (size_t i = 0; i < LENGTH(bars); i++) {
		int rx = bars[i].x;
		int ry = bars[i].y;
		int rw = bars[i].w;
		int rh = bars[i].h;
		if (rx < 0) rx = 0;
		if (ry < 0) ry = 0;
		if (rw < 1) rw = 1;
		if (rh < 1) rh = 1;
		if (rx + rw > base_w) rw = base_w - rx;
		if (ry + rh > base_h) rh = base_h - ry;
		for (int y = ry; y < ry + rh; y++) {
			for (int x = rx; x < rx + rw; x++) {
				size_t idx = (size_t)y * (size_t)base_w + (size_t)x;
				buf[idx] = ((uint32_t)0xFF << 24)
						| ((uint32_t)bars[i].r << 16)
						| ((uint32_t)bars[i].g << 8)
						| (uint32_t)bars[i].b;
			}
		}
	}

	wb = statusbar_scaled_buffer_from_argb32_raw((const uint32_t *)buf, base_w, base_h, target_h_final);
	free(buf);
	if (wb) {
		if (out_w)
			*out_w = target_w;
		if (out_h)
			*out_h = target_h_final;
	}
	return wb;
}

static int
tray_load_svg_pixbuf(const char *path, int desired_h, GdkPixbuf **out_pixbuf)
{
	RsvgHandle *handle = NULL;
	GError *gerr = NULL;
	double svg_w = 0.0, svg_h = 0.0;
	int target_w, target_h;
	cairo_surface_t *surface = NULL;
	cairo_t *cr = NULL;
	GdkPixbuf *pixbuf = NULL;

	if (!path || !*path || !out_pixbuf)
		return -1;

	/* Read the file manually so changes aren’t hidden by loader caches */
	{
		gchar *data = NULL;
		gsize len = 0;

		if (!g_file_get_contents(path, &data, &len, &gerr) || !data || len == 0) {
			if (gerr) {
				wlr_log(WLR_ERROR, "tray: failed to read SVG '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
			}
			g_free(data);
			return -1;
		}

		handle = rsvg_handle_new_from_data((const guint8 *)data, len, &gerr);
		g_free(data);
	}
	if (!handle) {
		if (gerr) {
			wlr_log(WLR_ERROR, "tray: failed to parse SVG '%s': %s",
					path, gerr->message);
			g_error_free(gerr);
		}
		return -1;
	}

	if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &svg_w, &svg_h) ||
			svg_w <= 0.0 || svg_h <= 0.0) {
		RsvgDimensionData dim = {0};
		rsvg_handle_get_dimensions(handle, &dim);
		svg_w = dim.width;
		svg_h = dim.height;
	}

	target_h = (desired_h > 0) ? desired_h :
		((svg_h > 0.0) ? (int)lround(svg_h) : 24);
	if (target_h <= 0)
		target_h = 24;
	if (svg_w > 0.0 && svg_h > 0.0)
		target_w = MAX(1, (int)lround((svg_w * (double)target_h) / svg_h));
	else
		target_w = target_h;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, target_w, target_h);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
		goto out;

	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);

	{
		RsvgRectangle viewport = {
			.x = 0,
			.y = 0,
			.width = (double)target_w,
			.height = (double)target_h,
		};
		if (!rsvg_handle_render_document(handle, cr, &viewport, &gerr)) {
			if (gerr) {
				wlr_log(WLR_ERROR, "tray: failed to render SVG '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
				gerr = NULL;
			}
			goto out;
		}
	}

	cairo_destroy(cr);
	cr = NULL;
	pixbuf = pixbuf_from_cairo_surface(surface, target_w, target_h);

out:
	if (cr)
		cairo_destroy(cr);
	if (surface)
		cairo_surface_destroy(surface);
	if (handle)
		g_object_unref(handle);
	if (!pixbuf && gerr) {
		wlr_log(WLR_ERROR, "tray: failed to load SVG '%s': %s",
				path, gerr->message);
	}
	if (gerr)
		g_error_free(gerr);

	if (!pixbuf)
		return -1;

	*out_pixbuf = pixbuf;
	return 0;
}

static void
drop_cpu_icon_buffer(void)
{
	if (cpu_icon_buf) {
		wlr_buffer_drop(cpu_icon_buf);
		cpu_icon_buf = NULL;
	}
	cpu_icon_loaded_h = 0;
	cpu_icon_w = cpu_icon_h = 0;
	cpu_icon_loaded_path[0] = '\0';
}

static int
ensure_cpu_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = cpu_icon_path;

	if (target_h <= 0)
		return -1;

	if (resolve_asset_path(cpu_icon_path, resolved, sizeof(resolved)) == 0 && resolved[0])
		path = resolved;

	if (cpu_icon_buf && cpu_icon_loaded_h == target_h &&
			strncmp(cpu_icon_loaded_path, path, sizeof(cpu_icon_loaded_path)) == 0)
		return 0;

	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "cpu icon: failed to load '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

	drop_cpu_icon_buffer();
	cpu_icon_buf = buf;
	cpu_icon_w = w;
	cpu_icon_h = h;
	cpu_icon_loaded_h = target_h;
	snprintf(cpu_icon_loaded_path, sizeof(cpu_icon_loaded_path), "%s", path);
	return 0;
}

static void
drop_clock_icon_buffer(void)
{
	if (clock_icon_buf) {
		wlr_buffer_drop(clock_icon_buf);
		clock_icon_buf = NULL;
	}
	clock_icon_loaded_h = 0;
	clock_icon_w = clock_icon_h = 0;
	clock_icon_loaded_path[0] = '\0';
}

static int
ensure_clock_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = clock_icon_path;

	if (target_h <= 0)
		return -1;

	if (resolve_asset_path(clock_icon_path, resolved, sizeof(resolved)) == 0 && resolved[0])
		path = resolved;

	if (clock_icon_buf && clock_icon_loaded_h == target_h &&
			strncmp(clock_icon_loaded_path, path, sizeof(clock_icon_loaded_path)) == 0)
		return 0;

	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "clock icon: failed to load '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

	drop_clock_icon_buffer();
	clock_icon_buf = buf;
	clock_icon_w = w;
	clock_icon_h = h;
	clock_icon_loaded_h = target_h;
	snprintf(clock_icon_loaded_path, sizeof(clock_icon_loaded_path), "%s", path);
	return 0;
}

static void
drop_light_icon_buffer(void)
{
	if (light_icon_buf) {
		wlr_buffer_drop(light_icon_buf);
		light_icon_buf = NULL;
	}
	light_icon_loaded_h = 0;
	light_icon_w = light_icon_h = 0;
	light_icon_loaded_path[0] = '\0';
}

static int
ensure_light_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = light_icon_path;

	if (target_h <= 0)
		return -1;

	if (resolve_asset_path(light_icon_path, resolved, sizeof(resolved)) == 0 && resolved[0])
		path = resolved;

	if (light_icon_buf && light_icon_loaded_h == target_h &&
			strncmp(light_icon_loaded_path, path, sizeof(light_icon_loaded_path)) == 0)
		return 0;

	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "light icon: failed to load '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

	drop_light_icon_buffer();
	light_icon_buf = buf;
	light_icon_w = w;
	light_icon_h = h;
	light_icon_loaded_h = target_h;
	snprintf(light_icon_loaded_path, sizeof(light_icon_loaded_path), "%s", path);
	return 0;
}

static void
drop_ram_icon_buffer(void)
{
	if (ram_icon_buf) {
		wlr_buffer_drop(ram_icon_buf);
		ram_icon_buf = NULL;
	}
	ram_icon_loaded_h = 0;
	ram_icon_w = ram_icon_h = 0;
	ram_icon_loaded_path[0] = '\0';
}

static int
ensure_ram_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = ram_icon_path;

	if (target_h <= 0)
		return -1;

	if (resolve_asset_path(ram_icon_path, resolved, sizeof(resolved)) == 0 && resolved[0])
		path = resolved;

	if (ram_icon_buf && ram_icon_loaded_h == target_h &&
			strncmp(ram_icon_loaded_path, path, sizeof(ram_icon_loaded_path)) == 0)
		return 0;

	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "ram icon: failed to load '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

	drop_ram_icon_buffer();
	ram_icon_buf = buf;
	ram_icon_w = w;
	ram_icon_h = h;
	ram_icon_loaded_h = target_h;
	snprintf(ram_icon_loaded_path, sizeof(ram_icon_loaded_path), "%s", path);
	return 0;
}

static void
drop_volume_icon_buffer(void)
{
	if (volume_icon_buf) {
		wlr_buffer_drop(volume_icon_buf);
		volume_icon_buf = NULL;
	}
	volume_icon_loaded_h = 0;
	volume_icon_w = volume_icon_h = 0;
	volume_icon_loaded_path[0] = '\0';
}

static int
ensure_volume_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = volume_icon_path;

	if (target_h <= 0)
		return -1;

	if (resolve_asset_path(volume_icon_path, resolved, sizeof(resolved)) == 0 && resolved[0])
		path = resolved;

	if (volume_icon_buf && volume_icon_loaded_h == target_h &&
			strncmp(volume_icon_loaded_path, path, sizeof(volume_icon_loaded_path)) == 0)
		return 0;

	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "volume icon: failed to load '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

	drop_volume_icon_buffer();
	volume_icon_buf = buf;
	volume_icon_w = w;
	volume_icon_h = h;
	volume_icon_loaded_h = target_h;
	snprintf(volume_icon_loaded_path, sizeof(volume_icon_loaded_path), "%s", path);
	return 0;
}

static void
drop_battery_icon_buffer(void)
{
	if (battery_icon_buf) {
		wlr_buffer_drop(battery_icon_buf);
		battery_icon_buf = NULL;
	}
	battery_icon_loaded_h = 0;
	battery_icon_w = battery_icon_h = 0;
	battery_icon_loaded_path[0] = '\0';
}

static int
ensure_battery_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = battery_icon_path;

	if (target_h <= 0)
		return -1;

	if (resolve_asset_path(battery_icon_path, resolved, sizeof(resolved)) == 0 && resolved[0])
		path = resolved;

	if (battery_icon_buf && battery_icon_loaded_h == target_h &&
			strncmp(battery_icon_loaded_path, path, sizeof(battery_icon_loaded_path)) == 0)
		return 0;

	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "battery icon: failed to load '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

	drop_battery_icon_buffer();
	battery_icon_buf = buf;
	battery_icon_w = w;
	battery_icon_h = h;
	battery_icon_loaded_h = target_h;
	snprintf(battery_icon_loaded_path, sizeof(battery_icon_loaded_path), "%s", path);
	return 0;
}

static void
drop_mic_icon_buffer(void)
{
	if (mic_icon_buf) {
		wlr_buffer_drop(mic_icon_buf);
		mic_icon_buf = NULL;
	}
	mic_icon_loaded_h = 0;
	mic_icon_w = mic_icon_h = 0;
	mic_icon_loaded_path[0] = '\0';
}

static int
ensure_mic_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = mic_icon_path;

	if (target_h <= 0)
		return -1;

	if (resolve_asset_path(mic_icon_path, resolved, sizeof(resolved)) == 0 && resolved[0])
		path = resolved;

	if (mic_icon_buf && mic_icon_loaded_h == target_h &&
			strncmp(mic_icon_loaded_path, path, sizeof(mic_icon_loaded_path)) == 0)
		return 0;

	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "mic icon: failed to load '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

	drop_mic_icon_buffer();
	mic_icon_buf = buf;
	mic_icon_w = w;
	mic_icon_h = h;
	mic_icon_loaded_h = target_h;
	snprintf(mic_icon_loaded_path, sizeof(mic_icon_loaded_path), "%s", path);
	return 0;
}

static int
tray_load_icon_file(TrayItem *it, const char *path, int desired_h)
{
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	GdkPixbuf *pixbuf = NULL;

	if (!it || !path || !*path)
		return -1;

	if (has_svg_extension(path)) {
		if (tray_load_svg_pixbuf(path, desired_h, &pixbuf) != 0) {
			pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
			if (!pixbuf && gerr) {
				wlr_log(WLR_ERROR, "tray: failed to load icon '%s': %s", path, gerr->message);
				g_error_free(gerr);
				gerr = NULL;
			}
			if (!pixbuf)
				return -1;
		}
	} else {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "tray: failed to load icon '%s': %s", path, gerr->message);
				g_error_free(gerr);
				gerr = NULL;
			}
			if (has_svg_extension(path) &&
					tray_load_svg_pixbuf(path, desired_h, &pixbuf) != 0)
				return -1;
			if (!pixbuf)
				return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, desired_h, &w, &h);
	if (!buf)
		return -1;

	if (it->icon_buf)
		wlr_buffer_drop(it->icon_buf);
	it->icon_buf = buf;
	it->icon_w = w;
	it->icon_h = h;
	return 0;
}

static int
tray_lookup_icon_in_theme(const char *theme_root, const char *name, int desired_h,
		char *best_path, int *best_diff, int *found)
{
	static const char *subdirs[] = {
		"", "status", "apps", "panel", "actions",
		"devices", "emblems", "places", "categories", "mimetypes"
	};
	static const char *exts[] = { "png", "svg", "xpm", "jpg", "jpeg" };
	const int base_sizes[] = { 16, 22, 24, 32, 48, 64, 96, 128, 256 };
	int sizes[LENGTH(base_sizes) + 1] = {0};
	size_t size_count = 0;
	char symbolic[128] = {0};
	char nosymbolic[128] = {0};
	const char *candidates[3] = { name, NULL, NULL };
	size_t cand_count = 1;

	if (!theme_root || !name || !*name || !pathisdir(theme_root))
		return -1;

	if (desired_h > 0)
		sizes[size_count++] = desired_h;
	for (size_t i = 0; i < LENGTH(base_sizes) && size_count < LENGTH(sizes); i++) {
		if (desired_h > 0 && base_sizes[i] == desired_h)
			continue;
		sizes[size_count++] = base_sizes[i];
	}

	if (strip_symbolic_suffix(name, nosymbolic, sizeof(nosymbolic))) {
		candidates[0] = nosymbolic; /* Prefer colored variant over symbolic */
		candidates[cand_count++] = name;
	} else {
		candidates[0] = name;
	}
	if (!strstr(name, "-symbolic") && cand_count < LENGTH(candidates)) {
		snprintf(symbolic, sizeof(symbolic), "%s-symbolic", name);
		candidates[cand_count++] = symbolic;
	}

	for (size_t s = 0; s < size_count; s++) {
		char sizedir[32];
		snprintf(sizedir, sizeof(sizedir), "%dx%d", sizes[s], sizes[s]);
		for (size_t d = 0; d < LENGTH(subdirs); d++) {
			for (size_t c = 0; c < cand_count; c++) {
				if (!candidates[c])
					continue;
				for (size_t e = 0; e < LENGTH(exts); e++) {
					char path[PATH_MAX];
					if (*subdirs[d])
						snprintf(path, sizeof(path), "%s/%s/%s/%s.%s",
								theme_root, sizedir, subdirs[d], candidates[c], exts[e]);
					else
						snprintf(path, sizeof(path), "%s/%s/%s.%s",
								theme_root, sizedir, candidates[c], exts[e]);
					tray_consider_icon(path, sizes[s], desired_h, best_path, best_diff, found);
					if (*found && *best_diff == 0)
						return 0;
				}
			}
		}
	}

	for (size_t d = 0; d < LENGTH(subdirs); d++) {
		for (size_t c = 0; c < cand_count; c++) {
			if (!candidates[c])
				continue;
			for (size_t e = 0; e < LENGTH(exts); e++) {
				char path[PATH_MAX];
				if (*subdirs[d])
					snprintf(path, sizeof(path), "%s/scalable/%s/%s.%s",
							theme_root, subdirs[d], candidates[c], exts[e]);
				else
					snprintf(path, sizeof(path), "%s/scalable/%s.%s",
							theme_root, candidates[c], exts[e]);
				tray_consider_icon(path, desired_h, desired_h, best_path, best_diff, found);
				if (*found && *best_diff == 0)
					return 0;
			}
		}
	}

	for (size_t d = 0; d < LENGTH(subdirs); d++) {
		for (size_t c = 0; c < cand_count; c++) {
			if (!candidates[c])
				continue;
			for (size_t e = 0; e < LENGTH(exts); e++) {
				char path[PATH_MAX];
				if (*subdirs[d])
					snprintf(path, sizeof(path), "%s/%s/%s.%s",
							theme_root, subdirs[d], candidates[c], exts[e]);
				else
					snprintf(path, sizeof(path), "%s/%s.%s",
							theme_root, candidates[c], exts[e]);
				tray_consider_icon(path, desired_h, desired_h, best_path, best_diff, found);
				if (*found && *best_diff == 0)
					return 0;
			}
		}
	}

	return *found ? 0 : -1;
}

static int
tray_lookup_icon_in_root(const char *base, const char *name, int desired_h,
		char *best_path, int *best_diff, int *found)
{
	DIR *dir;
	struct dirent *ent;
	static const char *exts[] = { "png", "svg", "xpm", "jpg", "jpeg" };

	if (!base || !*base || !name || !*name)
		return -1;

	tray_lookup_icon_in_theme(base, name, desired_h, best_path, best_diff, found);
	if (*found && *best_diff == 0)
		return 0;

	if (pathisdir(base)) {
		dir = opendir(base);
		if (dir) {
			while ((ent = readdir(dir))) {
				char sub[PATH_MAX];
				if (ent->d_name[0] == '.')
					continue;
				snprintf(sub, sizeof(sub), "%s/%s", base, ent->d_name);
				if (!pathisdir(sub))
					continue;
				tray_lookup_icon_in_theme(sub, name, desired_h, best_path, best_diff, found);
				if (*found && *best_diff == 0) {
					closedir(dir);
					return 0;
				}
			}
			closedir(dir);
		}
	}

	for (size_t e = 0; e < LENGTH(exts); e++) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s.%s", base, name, exts[e]);
		tray_consider_icon(path, desired_h, desired_h, best_path, best_diff, found);
		if (*found && *best_diff == 0)
			return 0;
	}

	return *found ? 0 : -1;
}

static int
tray_find_icon_path(const char *name, const char *theme_path, int desired_h,
		char *out, size_t outlen)
{
	const char *xdg_data_dirs;
	char best_path[PATH_MAX] = {0};
	char pathbufs[64][PATH_MAX];
	size_t pathcount = 0;
	int found = 0;
	int best_diff = INT_MAX;
	const char *themes[16] = {0};
	size_t theme_count = 0;
	char theme_env[256] = {0};
	const char *default_themes[] = {
		"Papirus", "Papirus-Dark", "Papirus-Light", "Adwaita", "hicolor"
	};

#define ADD_PATH(P) do { \
	const char *p_ = (P); \
	if (p_ && *p_ && pathcount < LENGTH(pathbufs)) { \
		snprintf(pathbufs[pathcount], sizeof(pathbufs[pathcount]), "%s", p_); \
		pathcount++; \
	} \
} while (0)

	if (getenv("STATUSBAR_ICON_THEMES")) {
		snprintf(theme_env, sizeof(theme_env), "%s", getenv("STATUSBAR_ICON_THEMES"));
		for (char *tok = theme_env; tok && *tok && theme_count < LENGTH(themes); ) {
			char *sep = strchr(tok, ':');
			if (sep)
				*sep = '\0';
			if (*tok)
				themes[theme_count++] = tok;
			if (!sep)
				break;
			tok = sep + 1;
		}
	}
	if (theme_count == 0) {
		for (size_t i = 0; i < LENGTH(default_themes) && theme_count < LENGTH(themes); i++)
			themes[theme_count++] = default_themes[i];
	}

	if (!name || !*name || !out || outlen == 0)
		return -1;

	if (name[0] == '/' && access(name, R_OK) == 0) {
		snprintf(out, outlen, "%s", name);
		return 0;
	}

	if (theme_path && *theme_path && pathcount < LENGTH(pathbufs)) {
		ADD_PATH(theme_path);
	}

	{
		const char *xdg_home = getenv("XDG_DATA_HOME");
		const char *home = getenv("HOME");
		if (xdg_home && *xdg_home && pathcount < LENGTH(pathbufs)) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/icons", xdg_home);
			add_icon_root_paths(path, themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));
		} else if (home && *home && pathcount < LENGTH(pathbufs)) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/.local/share/icons", home);
			add_icon_root_paths(path, themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));
		}
		if (home && *home && pathcount < LENGTH(pathbufs)) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/.icons", home);
			add_icon_root_paths(path, themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));
		}
	}

	xdg_data_dirs = getenv("XDG_DATA_DIRS");
	if (!xdg_data_dirs || !*xdg_data_dirs)
		xdg_data_dirs = "/usr/local/share:/usr/share";
	while (*xdg_data_dirs && pathcount < LENGTH(pathbufs)) {
		const char *end = strchrnul(xdg_data_dirs, ':');
		size_t len = (size_t)(end - xdg_data_dirs);
		if (len > 0 && len < sizeof(pathbufs[0]) - 7) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%.*s/icons",
					(int)len, xdg_data_dirs);
			add_icon_root_paths(path, themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));
		}
		if (*end == '\0')
			break;
		xdg_data_dirs = end + 1;
	}

	{
		const char *nix_profiles = getenv("NIX_PROFILES");
		while (nix_profiles && *nix_profiles && pathcount < LENGTH(pathbufs)) {
			const char *end = strchrnul(nix_profiles, ' ');
			size_t len = (size_t)(end - nix_profiles);
			if (len > 0 && len < sizeof(pathbufs[0]) - 14 && pathcount < LENGTH(pathbufs)) {
				char path[PATH_MAX];
				snprintf(path, sizeof(path),
						"%.*s/share/icons", (int)len, nix_profiles);
				add_icon_root_paths(path, themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));
			}
			if (len > 0 && len < sizeof(pathbufs[0]) - 16 && pathcount < LENGTH(pathbufs)) {
				char path[PATH_MAX];
				snprintf(path, sizeof(path),
						"%.*s/share/pixmaps", (int)len, nix_profiles);
				ADD_PATH(path);
			}
			if (*end == '\0')
				break;
			nix_profiles = end + 1;
		}
	}

	if (pathcount < LENGTH(pathbufs)) {
		add_icon_root_paths("/run/current-system/sw/share/icons", themes, theme_count, pathbufs, &pathcount, LENGTH(pathbufs));
	}
	if (pathcount < LENGTH(pathbufs)) {
		ADD_PATH("/run/current-system/sw/share/pixmaps");
	}

	if (pathcount < LENGTH(pathbufs)) {
		ADD_PATH("/usr/share/pixmaps");
	}

#undef ADD_PATH

	for (size_t i = 0; i < pathcount; i++) {
		tray_lookup_icon_in_root(pathbufs[i], name, desired_h, best_path, &best_diff, &found);
		if (found && best_diff == 0)
			break;
	}

	if (!found)
		return -1;

	snprintf(out, outlen, "%s", best_path);
	return 0;
}

static int
tray_get_string_property(TrayItem *it, const char *prop, char *out, size_t outlen)
{
	const char *ifaces[] = {
		"org.kde.StatusNotifierItem",
		"org.freedesktop.StatusNotifierItem",
	};
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	const char *val = NULL;
	int r = -1;

	if (!tray_bus || !it || !prop)
		return -1;

	for (size_t iface_idx = 0; iface_idx < LENGTH(ifaces); iface_idx++) {
		sd_bus_error_free(&err);
		sd_bus_message_unref(reply);
		reply = NULL;

		r = sd_bus_call_method(tray_bus, it->service, it->path,
				"org.freedesktop.DBus.Properties", "Get",
				&err, &reply, "ss", ifaces[iface_idx], prop);
		if (r >= 0)
			break;
	}
	if (r < 0 || !reply)
		goto out;
	if (sd_bus_message_enter_container(reply, 'v', "s") < 0)
		goto out;
	if (sd_bus_message_read(reply, "s", &val) < 0)
		goto out;
	if (val && *val && out && outlen > 0)
		snprintf(out, outlen, "%s", val);

out:
	sd_bus_error_free(&err);
	sd_bus_message_unref(reply);
	return (val && *val) ? 0 : -1;
}

__attribute__((unused)) static int
tray_item_load_icon(TrayItem *it)
{
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	const void *best_data = NULL;
	size_t best_len = 0;
	int desired_h = statusbar_height > 0 ? MAX(12, (int)statusbar_height - 4) : 24;
	int best_score = INT_MAX;
	int best_w = 0, best_h = 0;
	int r;
	const char *ifaces[] = {
		"org.kde.StatusNotifierItem",
		"org.freedesktop.StatusNotifierItem",
	};
	const char *props[] = { "IconPixmap", "AttentionIconPixmap" };
	int prop_idx;

	if (!tray_bus || !it)
		return -1;
	if (it->icon_tried && it->icon_failed)
		return -1;
	it->icon_tried = 1;

	/* Prefer themed icons first so the user's icon theme takes effect */
	{
		char icon_theme_path[PATH_MAX] = {0};
		char icon_path[PATH_MAX] = {0};
		char icon_name[128] = {0};

		tray_get_string_property(it, "IconThemePath", icon_theme_path,
				sizeof(icon_theme_path));

		if (tray_get_string_property(it, "AttentionIconName", icon_name, sizeof(icon_name)) == 0 &&
				tray_find_icon_path(icon_name, icon_theme_path, desired_h,
					icon_path, sizeof(icon_path)) == 0 &&
				tray_load_icon_file(it, icon_path, desired_h) == 0) {
			it->icon_failed = 0;
			return 0;
		}

		memset(icon_name, 0, sizeof(icon_name));
		memset(icon_path, 0, sizeof(icon_path));
		if (tray_get_string_property(it, "IconName", icon_name, sizeof(icon_name)) == 0 &&
				tray_find_icon_path(icon_name, icon_theme_path, desired_h,
					icon_path, sizeof(icon_path)) == 0 &&
				tray_load_icon_file(it, icon_path, desired_h) == 0) {
			it->icon_failed = 0;
			return 0;
		}
	}

	for (prop_idx = 0; prop_idx < (int)LENGTH(props); prop_idx++) {
		best_data = NULL;
		best_len = 0;
		best_score = INT_MAX;
		best_w = best_h = 0;

		for (size_t iface_idx = 0; iface_idx < LENGTH(ifaces); iface_idx++) {
			sd_bus_error_free(&err);
			sd_bus_message_unref(reply);
			reply = NULL;

			r = sd_bus_call_method(tray_bus, it->service, it->path,
					"org.freedesktop.DBus.Properties", "Get",
					&err, &reply, "ss", ifaces[iface_idx], props[prop_idx]);
			if (r >= 0)
				break;
		}
		if (r < 0)
			continue;

		if (sd_bus_message_enter_container(reply, 'v', "a(iiay)") < 0)
			continue;
		if (sd_bus_message_enter_container(reply, 'a', "(iiay)") < 0)
			continue;

		while (sd_bus_message_enter_container(reply, 'r', "iiay") > 0) {
			int w = 0, h = 0;
			const void *pix = NULL;
			size_t len = 0;
			if (sd_bus_message_read(reply, "ii", &w, &h) < 0) {
				sd_bus_message_exit_container(reply); /* struct */
				break;
			}
			if (sd_bus_message_read_array(reply, 'y', &pix, &len) < 0) {
				sd_bus_message_exit_container(reply); /* struct */
				break;
			}
			if (w > 0 && h > 0 && len >= (size_t)w * (size_t)h * 4) {
				int diff = desired_h > 0 ? abs(h - desired_h) : INT_MAX;
				size_t area = (size_t)w * (size_t)h;
				if ((desired_h > 0 && (diff < best_score ||
						(diff == best_score && h < best_h))) ||
						(desired_h <= 0 && area > best_len)) {
					best_score = diff;
					best_len = area;
					best_w = w;
					best_h = h;
					best_data = pix;
				}
			}
			sd_bus_message_exit_container(reply); /* struct */
		}

		sd_bus_message_exit_container(reply);
		sd_bus_message_exit_container(reply);

		if (best_data && best_w > 0 && best_h > 0)
			break;
	}

	if (best_data && best_w > 0 && best_h > 0) {
		int target_h = (desired_h > 0) ? desired_h : best_h;
		struct wlr_buffer *buf = NULL;

		if (best_h > target_h && target_h > 0)
			buf = statusbar_scaled_buffer_from_argb32(best_data, best_w, best_h, target_h);
		else
			buf = statusbar_buffer_from_argb32(best_data, best_w, best_h);
		if (buf) {
			if (it->icon_buf)
				wlr_buffer_drop(it->icon_buf);
			it->icon_buf = buf;
			if (target_h > 0 && best_h > target_h) {
				it->icon_w = (int)lround(((double)best_w * (double)target_h) / (double)best_h);
				it->icon_h = target_h;
			} else {
				it->icon_w = best_w;
				it->icon_h = best_h;
			}
			sd_bus_message_unref(reply);
			sd_bus_error_free(&err);
			it->icon_failed = 0;
			return 0;
		}
	}

	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
	it->icon_failed = 1;
	return -1;
}

static void
drop_net_icon_buffer(void)
{
	if (net_icon_buf) {
		wlr_buffer_drop(net_icon_buf);
		net_icon_buf = NULL;
	}
	net_icon_loaded_h = 0;
	net_icon_loaded_path[0] = '\0';
	net_icon_w = 0;
	net_icon_h = 0;
}

static int
load_net_icon_buffer(const char *path, int target_h)
{
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	GdkPixbuf *pixbuf = NULL;
	char resolved[PATH_MAX];
	const char *load_path = path;

	if (!path || !*path || target_h <= 0)
		return -1;

	if (resolve_asset_path(path, resolved, sizeof(resolved)) == 0)
		load_path = resolved;

	/* For the 100% icon, synthesize the bars to avoid theme tinting entirely */
	if (strcmp(path, net_icon_wifi_100) == 0
			|| (net_icon_wifi_100_resolved[0]
				&& strcmp(load_path, net_icon_wifi_100_resolved) == 0)) {
		buf = statusbar_buffer_from_wifi100(target_h, &w, &h);
		if (!buf) {
			wlr_log(WLR_ERROR, "net icon: synth wifi_100 failed (path=%s resolved=%s)",
					path, load_path);
			return -1;
		}
		snprintf(net_icon_loaded_path, sizeof(net_icon_loaded_path), "%s", load_path);
		goto done;
	}

	if (tray_load_svg_pixbuf(load_path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(load_path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "net icon: failed to load '%s': %s", load_path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	} else if (strcmp(path, net_icon_wifi_100) == 0
			|| (net_icon_wifi_100_resolved[0]
					&& strcmp(load_path, net_icon_wifi_100_resolved) == 0)) {
		/* Some themes tint this asset; normalize to expected green */
		recolor_wifi100_pixbuf(pixbuf);
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

done:
	drop_net_icon_buffer();
	net_icon_buf = buf;
	net_icon_w = w;
	net_icon_h = h;
	net_icon_loaded_h = target_h;
	snprintf(net_icon_loaded_path, sizeof(net_icon_loaded_path), "%s", load_path);
	wlr_log(WLR_INFO, "net icon: loaded %s (resolved=%s) w=%d h=%d target_h=%d",
			path, load_path, net_icon_w, net_icon_h, target_h);
	return 0;
}

static int
ensure_net_icon_buffer(int target_h)
{
	if (target_h <= 0)
		return -1;

	if (net_icon_buf && net_icon_loaded_h == target_h &&
			strncmp(net_icon_loaded_path, net_icon_path, sizeof(net_icon_loaded_path)) == 0)
		return 0;

	if (load_net_icon_buffer(net_icon_path, target_h) == 0)
		return 0;

	/* fallback to offline icon if the requested asset is missing */
	if (strcmp(net_icon_path, net_icon_no_conn) != 0)
		return load_net_icon_buffer(net_icon_no_conn, target_h);

	return -1;
}

static void
rendertrayicons(Monitor *m, int bar_height)
{
	StatusModule *module;
	int padding, target_h, icon_x, icon_y, base_h;
	struct wlr_scene_buffer *scene_buf;

	if (!m || !m->statusbar.sysicons.tree)
		return;

	module = &m->statusbar.sysicons;
	clearstatusmodule(module);
	module->width = 0;
	module->x = 0;

	/* Use a small padding so the icon fills most of the bar height */
	padding = statusbar_module_padding / 2;
	if (padding < 1)
		padding = 1;
	base_h = bar_height - 2 * padding;
	target_h = (int)lround(base_h * 1.5); /* 50% larger */
	if (target_h > bar_height)
		target_h = bar_height;
	if (target_h <= 0)
		target_h = bar_height - padding;
	if (target_h <= 0)
		target_h = 1;

	if (ensure_net_icon_buffer(target_h) != 0 || !net_icon_buf || net_icon_w <= 0 || net_icon_h <= 0) {
		wlr_scene_node_set_enabled(&module->tree->node, 0);
		return;
	}

	module->width = net_icon_w + 2 * padding;
	if (module->width < net_icon_w)
		module->width = net_icon_w;

	updatemodulebg(module, module->width, bar_height, statusbar_bg);

	scene_buf = wlr_scene_buffer_create(module->tree, NULL);
	if (scene_buf) {
		int usable_w = module->width - 2 * padding;
		icon_x = padding + MAX(0, (usable_w - net_icon_w) / 2);
		icon_y = MAX(0, (bar_height - net_icon_h) / 2);
		wlr_scene_buffer_set_buffer(scene_buf, net_icon_buf);
		wlr_scene_node_set_position(&scene_buf->node, icon_x, icon_y);
	}

	wlr_scene_node_set_enabled(&module->tree->node, module->width > 0);
}

static void
fix_tray_argb32(uint32_t *pixels, size_t count, int use_rgba_order)
{
	size_t i;

	if (!pixels || count == 0)
		return;

	for (i = 0; i < count; i++) {
		/* IconPixmap spec: ARGB32, bytes in big-endian order A,R,G,B */
		const uint8_t *p8 = (const uint8_t *)&pixels[i];
		uint8_t a, r, g, b;

		if (use_rgba_order || statusbar_tray_force_rgba) {
			/* Fallback for apps that provide RGBA bytes */
			r = p8[0];
			g = p8[1];
			b = p8[2];
			a = p8[3];
		} else {
			a = p8[0];
			r = p8[1];
			g = p8[2];
			b = p8[3];
		}

		/* Premultiply for pixman/wlroots */
		if (a != 0 && a != 255) {
			r = (uint8_t)((r * a + 127) / 255);
			g = (uint8_t)((g * a + 127) / 255);
			b = (uint8_t)((b * a + 127) / 255);
		}

		pixels[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
			((uint32_t)g << 8) | (uint32_t)b;
	}
}

static void
init_net_icon_paths(void)
{
	resolve_asset_path(net_icon_wifi_100, net_icon_wifi_100_resolved, sizeof(net_icon_wifi_100_resolved));
	resolve_asset_path(net_icon_wifi_75, net_icon_wifi_75_resolved, sizeof(net_icon_wifi_75_resolved));
	resolve_asset_path(net_icon_wifi_50, net_icon_wifi_50_resolved, sizeof(net_icon_wifi_50_resolved));
	resolve_asset_path(net_icon_wifi_25, net_icon_wifi_25_resolved, sizeof(net_icon_wifi_25_resolved));
	resolve_asset_path(net_icon_eth, net_icon_eth_resolved, sizeof(net_icon_eth_resolved));
	resolve_asset_path(net_icon_no_conn, net_icon_no_conn_resolved, sizeof(net_icon_no_conn_resolved));
	if (net_icon_no_conn_resolved[0])
		snprintf(net_icon_path, sizeof(net_icon_path), "%s", net_icon_no_conn_resolved);
	wlr_log(WLR_INFO, "net icon paths: wifi100=%s wifi75=%s wifi50=%s wifi25=%s eth=%s noconn=%s",
			net_icon_wifi_100_resolved[0] ? net_icon_wifi_100_resolved : net_icon_wifi_100,
			net_icon_wifi_75_resolved[0] ? net_icon_wifi_75_resolved : net_icon_wifi_75,
			net_icon_wifi_50_resolved[0] ? net_icon_wifi_50_resolved : net_icon_wifi_50,
			net_icon_wifi_25_resolved[0] ? net_icon_wifi_25_resolved : net_icon_wifi_25,
			net_icon_eth_resolved[0] ? net_icon_eth_resolved : net_icon_eth,
			net_icon_no_conn_resolved[0] ? net_icon_no_conn_resolved : net_icon_no_conn);
}

static struct wlr_buffer *
statusbar_scaled_buffer_from_argb32(const uint32_t *data, int width, int height, int target_h)
{
	pixman_image_t *src = NULL, *dst = NULL;
	struct PixmanBuffer *buf;
	uint8_t *src_copy = NULL;
	uint8_t *dst_copy = NULL;
	size_t src_stride, dst_stride, src_size, dst_size;
	int target_w;
	pixman_transform_t transform;

	if (!data || width <= 0 || height <= 0 || target_h <= 0)
		return NULL;

	target_w = (int)lround(((double)width * (double)target_h) / (double)height);
	if (target_w <= 0)
		target_w = 1;

	src_stride = (size_t)width * 4;
	dst_stride = (size_t)target_w * 4;
	src_size = src_stride * (size_t)height;
	dst_size = dst_stride * (size_t)target_h;

	src_copy = calloc(1, src_size);
	dst_copy = calloc(1, dst_size);
	if (!src_copy || !dst_copy)
		goto fail;
	memcpy(src_copy, data, src_size);
	fix_tray_argb32((uint32_t *)src_copy, (size_t)width * (size_t)height, 0);

	/* If the first pass produced fully transparent data, retry assuming RGBA order */
	{
		size_t alpha_sum = 0;
		for (size_t n = 0; n < (size_t)width * (size_t)height; n++)
			alpha_sum += src_copy[n * 4];
		if (alpha_sum == 0) {
			memcpy(src_copy, data, src_size);
			fix_tray_argb32((uint32_t *)src_copy, (size_t)width * (size_t)height, 1);
		}
	}

	src = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height,
			(uint32_t *)src_copy, (int)src_stride);
	dst = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, target_w, target_h,
			(uint32_t *)dst_copy, (int)dst_stride);
	if (!src || !dst)
		goto fail;

	/* Scale with a simple bilinear filter to fit the bar height */
	pixman_transform_init_identity(&transform);
	pixman_transform_scale(&transform, NULL,
			pixman_double_to_fixed((double)width / (double)target_w),
			pixman_double_to_fixed((double)height / (double)target_h));
	pixman_image_set_transform(src, &transform);
	pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);

	pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, dst,
			0, 0, 0, 0, 0, 0, target_w, target_h);

	pixman_image_unref(src);
	free(src_copy);
	src = NULL;
	src_copy = NULL;

	buf = ecalloc(1, sizeof(*buf));
	if (!buf)
		goto fail;

	buf->image = dst;
	buf->data = dst_copy;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = (int)dst_stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, target_w, target_h);
	return &buf->base;

fail:
	if (src)
		pixman_image_unref(src);
	if (dst)
		pixman_image_unref(dst);
	free(src_copy);
	free(dst_copy);
	return NULL;
}

static struct wlr_buffer *
statusbar_scaled_buffer_from_argb32_raw(const uint32_t *data, int width, int height, int target_h)
{
	pixman_image_t *src = NULL, *dst = NULL;
	struct PixmanBuffer *buf;
	uint8_t *src_copy = NULL;
	uint8_t *dst_copy = NULL;
	size_t src_stride, dst_stride, src_size, dst_size;
	int target_w;
	pixman_transform_t transform;

	if (!data || width <= 0 || height <= 0 || target_h <= 0)
		return NULL;

	target_w = (int)lround(((double)width * (double)target_h) / (double)height);
	if (target_w <= 0)
		target_w = 1;

	src_stride = (size_t)width * 4;
	dst_stride = (size_t)target_w * 4;
	src_size = src_stride * (size_t)height;
	dst_size = dst_stride * (size_t)target_h;

	src_copy = calloc(1, src_size);
	dst_copy = calloc(1, dst_size);
	if (!src_copy || !dst_copy)
		goto fail;
	memcpy(src_copy, data, src_size);

	src = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height,
			(uint32_t *)src_copy, (int)src_stride);
	dst = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, target_w, target_h,
			(uint32_t *)dst_copy, (int)dst_stride);
	if (!src || !dst)
		goto fail;

	pixman_transform_init_identity(&transform);
	pixman_transform_scale(&transform, NULL,
			pixman_double_to_fixed((double)width / (double)target_w),
			pixman_double_to_fixed((double)height / (double)target_h));
	pixman_image_set_transform(src, &transform);
	pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);

	pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, dst,
			0, 0, 0, 0, 0, 0, target_w, target_h);

	pixman_image_unref(src);
	free(src_copy);
	src = NULL;
	src_copy = NULL;

	buf = ecalloc(1, sizeof(*buf));
	if (!buf)
		goto fail;

	buf->image = dst;
	buf->data = dst_copy;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = (int)dst_stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, target_w, target_h);
	return &buf->base;

fail:
	if (src)
		pixman_image_unref(src);
	if (dst)
		pixman_image_unref(dst);
	free(src_copy);
	free(dst_copy);
	return NULL;
}

__attribute__((unused)) static struct wlr_buffer *
statusbar_buffer_from_argb32_raw(const uint32_t *data, int width, int height)
{
	pixman_image_t *dst;
	struct PixmanBuffer *buf;
	uint8_t *copy;
	size_t stride, size;

	if (!data || width <= 0 || height <= 0)
		return NULL;

	stride = (size_t)width * 4;
	size = stride * (size_t)height;
	copy = calloc(1, size);
	if (!copy)
		return NULL;
	memcpy(copy, data, size);

	dst = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height,
			(uint32_t *)copy, (int)stride);
	if (!dst) {
		free(copy);
		return NULL;
	}

	buf = ecalloc(1, sizeof(*buf));
	if (!buf) {
		pixman_image_unref(dst);
		free(copy);
		return NULL;
	}

	buf->image = dst;
	buf->data = copy;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = (int)stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, width, height);
	return &buf->base;
}

static struct wlr_buffer *
statusbar_buffer_from_argb32(const uint32_t *data, int width, int height)
{
	pixman_image_t *dst;
	struct PixmanBuffer *buf;
	uint8_t *copy;
	size_t stride, size;

	if (!data || width <= 0 || height <= 0)
		return NULL;

	stride = (size_t)width * 4;
	size = stride * (size_t)height;
	copy = calloc(1, size);
	if (!copy)
		return NULL;
	memcpy(copy, data, size);
	fix_tray_argb32((uint32_t *)copy, (size_t)width * (size_t)height, 0);

	/* If the first pass produced fully transparent data, retry assuming RGBA order */
	{
		size_t alpha_sum = 0;
		for (size_t n = 0; n < (size_t)width * (size_t)height; n++)
			alpha_sum += copy[n * 4];
		if (alpha_sum == 0) {
			memcpy(copy, data, size);
			fix_tray_argb32((uint32_t *)copy, (size_t)width * (size_t)height, 1);
		}
	}

	dst = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, width, height,
			(uint32_t *)copy, (int)stride);
	if (!dst) {
		free(copy);
		return NULL;
	}

	buf = ecalloc(1, sizeof(*buf));
	if (!buf) {
		pixman_image_unref(dst);
		free(copy);
		return NULL;
	}

	buf->image = dst;
	buf->data = copy;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = (int)stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, width, height);

	return &buf->base;
}

static int
cpu_proc_is_critical(pid_t pid, const char *name)
{
	const char *blocked[] = {
		"dwl", "systemd", "init", "dbus-daemon", "login", "seatd",
		"wpa_supplicant", "NetworkManager", "pipewire", "wireplumber",
		"pulseaudio", "udevd", "Xwayland", "kswapd"
	};

	if (pid <= 1 || pid == getpid())
		return 1;
	if (!name || !*name)
		return 1;
	if (name[0] == '[')
		return 1;
	if (!strncmp(name, "kworker", 7) || !strncmp(name, "ksoftirq", 8)
			|| !strncmp(name, "rcu_", 4) || !strncmp(name, "migration", 9)
			|| !strncmp(name, "kswapd", 6))
		return 1;

	for (size_t i = 0; i < LENGTH(blocked); i++) {
		if (strcasestr(name, blocked[i]))
			return 1;
	}
	return 0;
}

static int
cpu_proc_cmp(const void *a, const void *b)
{
	const CpuProcEntry *pa = a;
	const CpuProcEntry *pb = b;

	if (pa->cpu < pb->cpu)
		return 1;
	if (pa->cpu > pb->cpu)
		return -1;
	if (pa->pid < pb->pid)
		return -1;
	if (pa->pid > pb->pid)
		return 1;
	return 0;
}

static int
cpu_popup_clamped_x(Monitor *m, CpuPopup *p)
{
	int popup_x;

	if (!m || !p)
		return 0;

	popup_x = m->statusbar.cpu.x;
	if (p->width > 0 && m->statusbar.area.width > 0) {
		int max_x = m->statusbar.area.width - p->width;
		if (max_x < 0)
			max_x = 0;
		if (popup_x > max_x)
			popup_x = max_x;
		if (popup_x < 0)
			popup_x = 0;
	}
	return popup_x;
}

static int
cpu_popup_hover_index(Monitor *m, CpuPopup *p)
{
	int rel_x, rel_y;
	int popup_x;

	if (!m || !p || p->proc_count <= 0 || p->width <= 0 || p->height <= 0)
		return -1;

	popup_x = cpu_popup_clamped_x(m, p);
	rel_x = (int)floor(cursor->x) - m->statusbar.area.x - popup_x;
	rel_y = (int)floor(cursor->y) - m->statusbar.area.y - m->statusbar.area.height;
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return -1;

	for (int i = 0; i < p->proc_count; i++) {
		CpuProcEntry *e = &p->procs[i];
		if (!e->has_kill || e->kill_w <= 0 || e->kill_h <= 0)
			continue;
		if (rel_x >= e->kill_x && rel_x < e->kill_x + e->kill_w &&
				rel_y >= e->kill_y && rel_y < e->kill_y + e->kill_h)
			return i;
	}
	return -1;
}

static int
kill_processes_with_name(const char *name)
{
	DIR *dir;
	struct dirent *ent;
	int killed = 0;
	int found = 0;

	if (!name || !*name)
		return 0;

	dir = opendir("/proc");
	if (!dir)
		return 0;

	while ((ent = readdir(dir))) {
		pid_t pid = 0;
		char comm_path[PATH_MAX];
		char comm[256];
		FILE *fp;
		size_t len;
		int is_num = 1;

		if (!ent->d_name || !*ent->d_name)
			continue;
		for (size_t i = 0; ent->d_name[i]; i++) {
			if (!isdigit((unsigned char)ent->d_name[i])) {
				is_num = 0;
				break;
			}
		}
		if (!is_num)
			continue;

		pid = (pid_t)atoi(ent->d_name);
		if (pid <= 1 || pid == getpid())
			continue;

		found = 1;

		snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", ent->d_name);
		fp = fopen(comm_path, "r");
		if (!fp)
			continue;
		if (!fgets(comm, sizeof(comm), fp)) {
			fclose(fp);
			continue;
		}
		fclose(fp);
		len = strlen(comm);
		if (len > 0 && comm[len - 1] == '\n')
			comm[len - 1] = '\0';

		if (strcmp(comm, name) == 0) {
			if (kill(pid, SIGKILL) == 0) {
				killed++;
			} else {
				wlr_log(WLR_ERROR, "cpu popup: kill %d (%s) failed: %s",
						pid, name, strerror(errno));
			}
		}
	}

	closedir(dir);
	if (killed == 0 && found > 0) {
		char cmd[256];
		int ret = snprintf(cmd, sizeof(cmd), "pkill -9 -x %s", name);
		if (ret > 0 && ret < (int)sizeof(cmd)) {
			int res = system(cmd);
			(void)res;
		}
	}
	return killed;
}

static int
read_top_cpu_processes(CpuPopup *p)
{
	FILE *fp;
	char line[256];
	int count = 0;
	int lines = 0;

	if (!p)
		return 0;

	fp = popen("ps -eo pid,comm,pcpu --no-headers --sort=-pcpu", "r");
	if (!fp) {
		p->proc_count = 0;
		return 0;
	}

	while (fgets(line, sizeof(line), fp) && lines < 128) {
		CpuProcEntry *e;
		pid_t pid = 0;
		char name[64] = {0};
		double cpu = 0.0;
		int existing = -1;

		lines++;
		if (sscanf(line, "%d %63s %lf", &pid, name, &cpu) != 3)
			continue;
		if (name[0] == '[')
			continue;
		if (cpu_proc_is_critical(pid, name))
			continue;

		for (int i = 0; i < count; i++) {
			if (strcmp(p->procs[i].name, name) == 0) {
				existing = i;
				break;
			}
		}

		if (existing >= 0) {
			e = &p->procs[existing];
			e->cpu += cpu;
			if (cpu > e->max_single_cpu) {
				e->max_single_cpu = cpu;
				e->pid = pid;
			}
		} else if (count < (int)LENGTH(p->procs)) {
			e = &p->procs[count];
			e->pid = pid;
			snprintf(e->name, sizeof(e->name), "%s", name);
			e->cpu = cpu;
			e->max_single_cpu = cpu;
			e->y = e->height = 0;
			e->kill_x = e->kill_y = e->kill_w = e->kill_h = 0;
			e->has_kill = 1;
			count++;
		}
	}

	pclose(fp);
	if (count > 1)
		qsort(p->procs, (size_t)count, sizeof(p->procs[0]), cpu_proc_cmp);
	p->proc_count = count;
	return count;
}

static void
rendercpupopup(Monitor *m)
{
	CpuPopup *p;
	int padding, line_spacing;
	int line_count;
	int left_w = 0, right_w = 0;
	int left_h = 0, right_h = 0;
	int content_h;
	int column_gap;
	int row_height;
	int button_gap;
	int kill_text_w;
	int kill_w;
	int kill_h;
	uint64_t now;
	int use_right_gap;
	int hover_idx;
	int popup_x = m->statusbar.cpu.x;
	int need_fetch_now;
	char line[64];
	struct wlr_scene_node *node, *tmp;

	if (!m || !m->statusbar.cpu_popup.tree)
		return;

	p = &m->statusbar.cpu_popup;
	hover_idx = p->hover_idx;
	if (hover_idx < -1 || hover_idx >= (int)LENGTH(p->procs))
		hover_idx = -1;
	padding = statusbar_module_padding;
	line_spacing = 2;
	column_gap = statusbar_module_spacing + 8;
	row_height = statusfont.height > 0 ? statusfont.height : 16;
	button_gap = statusbar_module_spacing > 0 ? statusbar_module_spacing / 2 : 6;
	kill_text_w = status_text_width("Kill");
	kill_w = kill_text_w + 12;
	kill_h = row_height;
	now = monotonic_msec();
	need_fetch_now = 0;
	now = monotonic_msec();

	/* Clear previous buffers but keep bg */
	wl_list_for_each_safe(node, tmp, &p->tree->children, link) {
		if (p->bg && node == &p->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!statusfont.font || cpu_core_count <= 0) {
		p->width = p->height = 0;
		if (p->tree)
			wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->visible = 0;
		return;
	}

	if (p->suppress_refresh_until_ms == 0 || now >= p->suppress_refresh_until_ms) {
		if (p->last_fetch_ms == 0 ||
				now < p->last_fetch_ms ||
				now - p->last_fetch_ms >= cpu_popup_refresh_interval_ms)
			p->refresh_data = 1;
	}

	if (p->refresh_data) {
		if (p->last_fetch_ms == 0 || now - p->last_fetch_ms >= 200)
			need_fetch_now = 1;
		if (need_fetch_now) {
			read_top_cpu_processes(p);
			p->last_fetch_ms = now;
		}
		if (p->suppress_refresh_until_ms > 0 && now >= p->suppress_refresh_until_ms)
			p->suppress_refresh_until_ms = 0;
		p->refresh_data = 0;
	}
	line_count = cpu_core_count + 1; /* +1 for avg line */

	for (int i = 0; i < line_count; i++) {
		double perc = (i < cpu_core_count) ? cpu_last_core_percent[i] : cpu_last_percent;

		if (perc < 0.0) {
			if (i < cpu_core_count)
				snprintf(line, sizeof(line), "C%d: --%%", i);
			else
				snprintf(line, sizeof(line), "Avg: --%%");
		} else if (i < cpu_core_count) {
			snprintf(line, sizeof(line), "C%d: %d%%", i, (int)lround(perc));
		} else {
			int avg_disp = (perc < 1.0) ? 0 : (int)lround(perc);
			snprintf(line, sizeof(line), "Avg: %d%%", avg_disp);
		}
		left_w = MAX(left_w, status_text_width(line));
	}
	left_h = line_count * row_height + (line_count - 1) * line_spacing;

	for (int i = 0; i < p->proc_count; i++) {
		CpuProcEntry *e = &p->procs[i];
		int cpu_disp = (int)lround(e->cpu < 0.0 ? 0.0 : e->cpu);
		int row_w;
		char proc_line[128];

		snprintf(proc_line, sizeof(proc_line), "%s %d%%", e->name, cpu_disp);
		row_w = status_text_width(proc_line);
		if (e->has_kill)
			row_w += button_gap + kill_w;
		right_w = MAX(right_w, row_w);
	}
	if (p->proc_count > 0)
		right_h = p->proc_count * row_height + (p->proc_count - 1) * line_spacing;

	content_h = MAX(left_h, right_h);
	use_right_gap = right_w > 0 ? column_gap : 0;
	p->width = 2 * padding + left_w + use_right_gap + right_w;
	p->height = content_h + 2 * padding;

	if (p->width > 0 && m->statusbar.area.width > 0) {
		int max_x = m->statusbar.area.width - p->width;
		if (max_x < 0)
			max_x = 0;
		if (popup_x > max_x)
			popup_x = max_x;
		if (popup_x < 0)
			popup_x = 0;
	}

	if (!p->bg && !(p->bg = wlr_scene_tree_create(p->tree)))
		return;
	wlr_scene_node_set_enabled(&p->bg->node, 1);
	wlr_scene_node_set_position(&p->bg->node, 0, 0);
	wl_list_for_each_safe(node, tmp, &p->bg->children, link)
		wlr_scene_node_destroy(node);
	drawrect(p->bg, 0, 0, p->width, p->height, statusbar_popup_bg);

	for (int i = 0; i < line_count; i++) {
		double perc = (i < cpu_core_count) ? cpu_last_core_percent[i] : cpu_last_percent;
		int row_y = padding + i * (row_height + line_spacing);
		struct wlr_scene_tree *row;
		StatusModule mod = {0};

		if (perc < 0.0) {
			if (i < cpu_core_count)
				snprintf(line, sizeof(line), "C%d: --%%", i);
			else
				snprintf(line, sizeof(line), "Avg: --%%");
		} else if (i < cpu_core_count) {
			snprintf(line, sizeof(line), "C%d: %d%%", i, (int)lround(perc));
		} else {
			int avg_disp = (perc < 1.0) ? 0 : (int)lround(perc);
			snprintf(line, sizeof(line), "Avg: %d%%", avg_disp);
		}

		row = wlr_scene_tree_create(p->tree);
		if (!row)
			continue;
		wlr_scene_node_set_position(&row->node, padding, row_y);
		mod.tree = row;
		tray_render_label(&mod, line, 0, row_height, statusbar_fg);
	}

	if (right_w > 0) {
		int right_x = padding + left_w + use_right_gap;
		for (int i = 0; i < p->proc_count; i++) {
			CpuProcEntry *e = &p->procs[i];
			int cpu_disp = (int)lround(e->cpu < 0.0 ? 0.0 : e->cpu);
			int row_y = padding + i * (row_height + line_spacing);
			int text_w;
			char proc_line[128];
			struct wlr_scene_tree *row;
			StatusModule mod = {0};

			snprintf(proc_line, sizeof(proc_line), "%s %d%%", e->name, cpu_disp);
			row = wlr_scene_tree_create(p->tree);
			if (row) {
				wlr_scene_node_set_position(&row->node, right_x, row_y);
				mod.tree = row;
				text_w = tray_render_label(&mod, proc_line, 0, row_height, statusbar_fg);
				if (text_w <= 0)
					text_w = status_text_width(proc_line);
			} else {
				text_w = status_text_width(proc_line);
			}

			e->y = row_y;
			e->height = row_height;
		if (e->has_kill && kill_w > 0 && kill_h > 0) {
			int btn_x = right_x + text_w + button_gap;
			int btn_y = row_y + (row_height - kill_h) / 2;
			struct wlr_scene_tree *btn;
			StatusModule btn_mod = {0};
			const float *btn_color = statusbar_volume_muted_fg;
			int is_hover = (hover_idx == i);

			if (is_hover)
				btn_color = statusbar_tag_active_bg;

			e->kill_x = btn_x;
				e->kill_y = btn_y;
				e->kill_w = kill_w;
				e->kill_h = kill_h;
				drawrect(p->tree, btn_x, btn_y, kill_w, kill_h, btn_color);

				btn = wlr_scene_tree_create(p->tree);
				if (btn) {
					int text_x = (kill_w - kill_text_w) / 2;
					if (text_x < 2)
						text_x = 2;
					wlr_scene_node_set_position(&btn->node, btn_x, btn_y);
					btn_mod.tree = btn;
					tray_render_label(&btn_mod, "Kill", text_x, kill_h, statusbar_fg);
				}
			} else {
				e->kill_x = e->kill_y = e->kill_w = e->kill_h = 0;
				e->has_kill = 0;
			}
		}
	}

	if (p->width <= 0 || p->height <= 0)
		wlr_scene_node_set_enabled(&p->tree->node, 0);
	p->last_render_ms = now;
}

static int
cpu_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	CpuPopup *p;
	int rel_x, rel_y;
	int popup_x;

	if (!m || !m->statusbar.cpu_popup.visible || button != BTN_LEFT)
		return 0;

	p = &m->statusbar.cpu_popup;
	if (!p->tree || p->width <= 0 || p->height <= 0)
		return 0;

	popup_x = m->statusbar.cpu.x;
	if (p->width > 0 && m->statusbar.area.width > 0) {
		int max_x = m->statusbar.area.width - p->width;
		if (max_x < 0)
			max_x = 0;
		if (popup_x > max_x)
			popup_x = max_x;
		if (popup_x < 0)
			popup_x = 0;
	}

	rel_x = lx - popup_x;
	rel_y = ly - m->statusbar.area.height;
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;

	for (int i = 0; i < p->proc_count; i++) {
		CpuProcEntry *e = &p->procs[i];
		if (!e->has_kill || e->kill_w <= 0 || e->kill_h <= 0)
			continue;
		if (rel_x >= e->kill_x && rel_x < e->kill_x + e->kill_w &&
				rel_y >= e->kill_y && rel_y < e->kill_y + e->kill_h) {
			if (cpu_proc_is_critical(e->pid, e->name))
				return 1;
			if (kill_processes_with_name(e->name) == 0) {
				if (kill(e->pid, SIGKILL) != 0)
					wlr_log(WLR_ERROR, "cpu popup: kill %d failed: %s",
							e->pid, strerror(errno));
			}
			p->last_fetch_ms = monotonic_msec();
			p->suppress_refresh_until_ms = p->last_fetch_ms + 2000;
			p->refresh_data = 0;
			schedule_cpu_popup_refresh(2000);
			refreshstatuscpu();
			return 1;
		}
	}

	return 0;
}

static void
rendernetpopup(Monitor *m)
{
	NetPopup *p;
	int padding, line_spacing;
	int line_count = 4;
	int max_width = 0;
	int total_height;
	char lines[4][128];
	struct wlr_scene_node *node, *tmp;

	if (!m || !m->statusbar.net_popup.tree)
		return;

	p = &m->statusbar.net_popup;
	padding = statusbar_module_padding;
	line_spacing = 2;

	if (net_is_wireless) {
		int sig = (net_last_wifi_quality >= 0.0) ? (int)lround(net_last_wifi_quality) : -1;
		if (sig >= 0)
			snprintf(lines[0], sizeof(lines[0]), "Wifi: %s | %d%%", net_ssid, sig);
		else
			snprintf(lines[0], sizeof(lines[0]), "Wifi: %s | --", net_ssid);
	} else {
		if (net_link_speed_mbps >= 1000)
			snprintf(lines[0], sizeof(lines[0]), "Ethernet | %.1f Gbps", net_link_speed_mbps / 1000.0);
		else if (net_link_speed_mbps > 0)
			snprintf(lines[0], sizeof(lines[0]), "Ethernet | %d Mbps", net_link_speed_mbps);
		else
			snprintf(lines[0], sizeof(lines[0]), "Ethernet | --");
	}
	snprintf(lines[1], sizeof(lines[1]), "Local: %s", net_local_ip);
	snprintf(lines[2], sizeof(lines[2]), "Public: %s", net_public_ip);
	snprintf(lines[3], sizeof(lines[3]), "Up: %s  Down: %s", net_up_text, net_down_text);

	/* Clear previous buffers but keep bg */
	wl_list_for_each_safe(node, tmp, &p->tree->children, link) {
		if (p->bg && node == &p->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!statusfont.font || !net_available) {
		p->width = p->height = 0;
		if (p->tree)
			wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->visible = 0;
		return;
	}

	/* First pass: compute max width */
	for (int i = 0; i < line_count; i++) {
		const struct fcft_glyph *glyph;
		uint32_t prev_cp = 0;
		int pen_x = 0;
		int min_x = INT_MAX, max_x_local = INT_MIN;
		const char *text = lines[i];

		for (size_t j = 0; text[j]; j++) {
			long kern_x = 0, kern_y = 0;
			uint32_t cp = (unsigned char)text[j];
			if (prev_cp)
				fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
			pen_x += (int)kern_x;

			glyph = fcft_rasterize_char_utf32(statusfont.font, cp, statusbar_font_subpixel);
			if (!glyph || !glyph->pix) {
				prev_cp = cp;
				continue;
			}

			min_x = MIN(min_x, pen_x + glyph->x);
			max_x_local = MAX(max_x_local, pen_x + glyph->x + glyph->width);
			pen_x += glyph->advance.x;
			if (text[j + 1])
				pen_x += statusbar_font_spacing;
			prev_cp = cp;
		}

		if (min_x == INT_MAX || max_x_local == INT_MIN)
			continue;
		max_width = MAX(max_width, max_x_local - min_x);
	}

	p->width = max_width + 2 * padding;
	p->height = line_count * statusfont.height + (line_count - 1) * line_spacing + 2 * padding;
	total_height = p->height;

	if (!p->bg && !(p->bg = wlr_scene_tree_create(p->tree)))
		return;
	wlr_scene_node_set_enabled(&p->bg->node, 1);
	wlr_scene_node_set_position(&p->bg->node, 0, 0);
	wl_list_for_each_safe(node, tmp, &p->bg->children, link)
		wlr_scene_node_destroy(node);
	drawrect(p->bg, 0, 0, p->width, total_height, statusbar_popup_bg);

	for (int i = 0; i < line_count; i++) {
		const char *text = lines[i];
		int min_x = INT_MAX, max_x_local = INT_MIN;
		int min_y = INT_MAX, max_y = INT_MIN;
		int pen_x = 0;
		int width = 0;
		int origin_x = 0;
		int origin_y = 0;
		uint32_t prev_cp = 0;

		tll(struct GlyphRun) glyphs = tll_init();
		for (size_t j = 0; text[j]; j++) {
			long kern_x = 0, kern_y = 0;
			uint32_t cp = (unsigned char)text[j];
			struct GlyphRun run;
			const struct fcft_glyph *glyph;

			if (prev_cp)
				fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
			pen_x += (int)kern_x;

			glyph = fcft_rasterize_char_utf32(statusfont.font,
					cp, statusbar_font_subpixel);
			if (!glyph || !glyph->pix) {
				prev_cp = cp;
				continue;
			}

			run.glyph = glyph;
			run.pen_x = pen_x;
			run.codepoint = cp;
			tll_push_back(glyphs, run);

			min_x = MIN(min_x, pen_x + glyph->x);
			min_y = MIN(min_y, -glyph->y);
			max_x_local = MAX(max_x_local, pen_x + glyph->x + glyph->width);
			max_y = MAX(max_y, -glyph->y + glyph->height);

			pen_x += glyph->advance.x;
			if (text[j + 1])
				pen_x += statusbar_font_spacing;
			prev_cp = cp;
		}

		if (tll_length(glyphs) == 0) {
			tll_free(glyphs);
			continue;
		}

		width = max_x_local - min_x;
		origin_x = padding + (max_width - width) / 2;
		origin_y = padding + i * (statusfont.height + line_spacing) + statusfont.ascent;

		tll_foreach(glyphs, it) {
			struct wlr_buffer *buffer;
			struct wlr_scene_buffer *scene_buf;
			const struct fcft_glyph *glyph = it->item.glyph;

			buffer = statusbar_buffer_from_glyph(glyph);
			if (!buffer)
				continue;

			scene_buf = wlr_scene_buffer_create(p->tree, NULL);
			if (scene_buf) {
				wlr_scene_buffer_set_buffer(scene_buf, buffer);
				wlr_scene_node_set_position(&scene_buf->node,
						origin_x + it->item.pen_x + glyph->x,
						origin_y - glyph->y);
			}
			wlr_buffer_drop(buffer);
		}

		tll_free(glyphs);
	}

	if (p->width <= 0 || p->height <= 0)
		wlr_scene_node_set_enabled(&p->tree->node, 0);
	else {
		/* keep anchor so hover detection survives brief relayouts */
		p->anchor_x = m->statusbar.sysicons.x;
		p->anchor_y = m->statusbar.area.height;
		p->anchor_w = m->statusbar.sysicons.width;
	}
}

static void
modal_hide(Monitor *m)
{
	if (!m || !m->modal.tree)
		return;
	modal_file_search_stop(m);
	m->modal.visible = 0;
	wlr_scene_node_set_enabled(&m->modal.tree->node, 0);
}

static void
modal_hide_all(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link)
		modal_hide(m);
}

static void
modal_show(const Arg *arg)
{
	Monitor *m = selmon ? selmon : xytomon(cursor->x, cursor->y);
	Monitor *vm = modal_visible_monitor();
	ModalOverlay *mo;
	int mw, mh, w, h, x, y;
	int min_w = 300, min_h = 200;
	int max_w = 1400, max_h = 800;
	int cycling = 0;

	(void)arg;

	if (!m || !m->modal.tree)
		return;

	if (vm && vm != m)
		modal_hide_all();
	else if (vm == m)
		cycling = 1;

	mw = m->w.width > 0 ? m->w.width : m->m.width;
	mh = m->w.height > 0 ? m->w.height : m->m.height;
	w = mw > 0 ? (int)(mw * 0.8) : 1100;
	h = mh > 0 ? (int)(mh * 0.6) : 500;
	if (w < min_w)
		w = min_w;
	if (h < min_h)
		h = min_h;
	if (w > max_w)
		w = max_w;
	if (h > max_h)
		h = max_h;

	x = m->m.x + (m->m.width - w) / 2;
	y = m->m.y + (m->m.height - h) / 2;
	if (x < m->m.x)
		x = m->m.x;
	if (y < m->m.y)
		y = m->m.y;

	mo = &m->modal;
	mo->width = w;
	mo->height = h;
	mo->x = x;
	mo->y = y;

	if (!mo->bg)
		mo->bg = wlr_scene_tree_create(mo->tree);
	if (mo->bg) {
		struct wlr_scene_node *node, *tmp;
		wl_list_for_each_safe(node, tmp, &mo->bg->children, link)
			wlr_scene_node_destroy(node);
		drawrect(mo->bg, 0, 0, w, h, statusbar_popup_bg);
	}

	wlr_scene_node_set_position(&mo->tree->node, x, y);
	wlr_scene_node_set_enabled(&mo->tree->node, 1);
	if (cycling)
		mo->active_idx = (mo->active_idx + 1) % 3;
	else if (mo->active_idx < 0 || mo->active_idx > 2)
		mo->active_idx = 0;
	else
		mo->active_idx = 0;
	modal_file_search_stop(m);
	modal_file_search_clear_results(m);
	mo->file_search_last[0] = '\0';
	for (int i = 0; i < 3; i++) {
		mo->search[i][0] = '\0';
		mo->search_len[i] = 0;
		mo->selected[i] = -1;
		mo->scroll[i] = 0;
	}
	mo->file_search_fallback = 0;
	mo->visible = 1;
	modal_update_results(m);
	modal_render(m);
}

static void
renderworkspaces(Monitor *m, StatusModule *module, int bar_height)
{
	uint32_t mask = 0;
	Client *c;
	int padding, inner, spacing, outer_pad;
	int box_h, box_y, total_w = 0;
	int x, count = 0;
	struct wlr_scene_buffer *scene_buf;
	struct wlr_buffer *buffer;

	if (!m || !module || !module->tree)
		return;
	if (!statusfont.font || bar_height <= 0) {
		module->width = 0;
		return;
	}

	padding = statusbar_module_padding;
	inner = statusbar_workspace_padding;
	spacing = statusbar_workspace_spacing;
	/* give the module a little extra padding so bg extends past boxes */
	outer_pad = padding + spacing + 6;
	module->box_count = 0;
	module->tagmask = 0;
	if (module->hover_tag < -1 || module->hover_tag >= TAGCOUNT)
		module->hover_tag = -1;

	wl_list_for_each(c, &clients, link) {
		if (c->mon != m)
			continue;
		mask |= c->tags;
	}
	mask |= m->tagset[m->seltags];
	mask |= 1u; /* Always show workspace 1 */
	mask &= TAGMASK;

	clearstatusmodule(module);

	box_h = MAX(1, MIN(bar_height - 2, statusfont.height + inner * 2 + 2));
	box_y = (bar_height - box_h) / 2;
	x = outer_pad;

	for (int i = 0; i < TAGCOUNT; i++) {
		const struct fcft_glyph *glyph;
		int min_x, max_x, min_y, max_y;
		int text_w, text_h, box_w;
		int origin_x, origin_y;
		const float *bgcol;
		int active;

		if (!(mask & (1u << i)))
			continue;

		if (count > 0) {
			x += spacing;
			total_w += spacing;
		}

		glyph = fcft_rasterize_char_utf32(statusfont.font,
				(uint32_t)('1' + i), statusbar_font_subpixel);
		if (!glyph || !glyph->pix) {
			continue;
		}

		min_x = glyph->x;
		max_x = glyph->x + glyph->width;
		min_y = -glyph->y;
		max_y = -glyph->y + glyph->height;
		text_w = max_x - min_x;
		text_h = max_y - min_y;
		box_w = text_w + inner * 2;
		origin_x = x + (box_w - text_w) / 2 - min_x;
		origin_y = box_y + (box_h - text_h) / 2 - min_y;

		active = (m->tagset[m->seltags] & (1u << i)) != 0;
		bgcol = statusbar_tag_bg;
		if (active)
			bgcol = statusbar_tag_active_bg;

		drawrect(module->tree, x, box_y, box_w, box_h, bgcol);

		if (!active && module->hover_alpha[i] > 0.0f) {
			drawhoverrect(module->tree, x, box_y, box_w, box_h,
					statusbar_tag_hover_bg, module->hover_alpha[i]);
		}

		buffer = statusbar_buffer_from_glyph(glyph);
		if (buffer) {
			scene_buf = wlr_scene_buffer_create(module->tree, NULL);
			if (scene_buf) {
				wlr_scene_buffer_set_buffer(scene_buf, buffer);
				wlr_scene_node_set_position(&scene_buf->node,
						origin_x + glyph->x,
						origin_y - glyph->y);
			}
			wlr_buffer_drop(buffer);
		}

		x += box_w;
		total_w += box_w;
		if (module->box_count < TAGCOUNT) {
			int idx = module->box_count;
			module->box_x[idx] = outer_pad + total_w - box_w;
			module->box_w[idx] = box_w;
			module->box_tag[idx] = i;
			module->tagmask |= (1u << i);
			module->box_count++;
		}
		count++;
	}

	module->width = total_w + outer_pad * 2;
	updatemodulebg(module, module->width, bar_height, statusbar_tag_bg);
}

static int
readcpustats(struct CpuSample *out, int maxcount)
{
	FILE *fp;
	char line[256];
	int max_idx = -1;

	if (!out || maxcount <= 0)
		return 0;

	for (int i = 0; i < maxcount; i++) {
		out[i].idle = 0;
		out[i].total = 0;
	}

	fp = fopen("/proc/stat", "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		int idx = -1;
		unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
		unsigned long long idle_all, non_idle;

		if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
				&idx, &user, &nice, &system, &idle, &iowait, &irq, &softirq,
				&steal) == 9) {
			if (idx < 0 || idx >= maxcount)
				continue;
			idle_all = idle + iowait;
			non_idle = user + nice + system + irq + softirq + steal;
			out[idx].idle = idle_all;
			out[idx].total = idle_all + non_idle;
			if (idx > max_idx)
				max_idx = idx;
		}
	}

	fclose(fp);
	return max_idx + 1;
}

static double
cpuaverage(void)
{
	struct CpuSample curr[MAX_CPU_CORES];
	int count, used, i;
	double sum_busy = 0.0, sum_total = 0.0;
	double busy, perc;

	count = readcpustats(curr, MAX_CPU_CORES);
	if (count <= 0)
		return -1.0;
	if (count > MAX_CPU_CORES)
		count = MAX_CPU_CORES;

	for (i = 0; i < MAX_CPU_CORES; i++)
		cpu_last_core_percent[i] = -1.0;

	if (cpu_prev_count <= 0) {
		memcpy(cpu_prev, curr, count * sizeof(struct CpuSample));
		cpu_prev_count = count;
		cpu_core_count = count;
		return -1.0;
	}

	used = MIN(count, cpu_prev_count);
	cpu_core_count = count;
	for (i = 0; i < used; i++) {
		unsigned long long diff_total = curr[i].total - cpu_prev[i].total;
		unsigned long long diff_idle = curr[i].idle - cpu_prev[i].idle;
		if (curr[i].total == 0 || curr[i].idle == 0 ||
					curr[i].total <= cpu_prev[i].total ||
					curr[i].idle < cpu_prev[i].idle ||
					diff_total == 0) {
			cpu_last_core_percent[i] = -1.0;
			continue;
		}
		busy = (double)(diff_total - diff_idle);
		perc = (busy / (double)diff_total) * 100.0;
		cpu_last_core_percent[i] = perc;
		sum_busy += busy;
		sum_total += (double)diff_total;
	}

	memcpy(cpu_prev, curr, count * sizeof(struct CpuSample));
	cpu_prev_count = count;

	if (sum_total <= 0.0)
		return -1.0;

	return (sum_busy / sum_total) * 100.0;
}

static int
readmeminfo(unsigned long long *total_kb, unsigned long long *avail_kb)
{
	FILE *fp;
	char line[256];
	unsigned long long total = 0, avail = 0;

	fp = fopen("/proc/meminfo", "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "MemTotal: %llu kB", &total) == 1)
			continue;
		if (sscanf(line, "MemAvailable: %llu kB", &avail) == 1)
			continue;
	}

	fclose(fp);

	if (total == 0 || avail == 0)
		return -1;

	if (total_kb)
		*total_kb = total;
	if (avail_kb)
		*avail_kb = avail;
	return 0;
}

static double
ramused_mb(void)
{
	unsigned long long total, avail;

	if (readmeminfo(&total, &avail) != 0)
		return -1.0;

	if (total <= avail)
		return 0.0;

	return (double)(total - avail) / 1024.0;
}

static int
readulong(const char *path, unsigned long long *out)
{
	FILE *fp;
	unsigned long long val = 0;

	if (!path || !out)
		return -1;

	fp = fopen(path, "r");
	if (!fp)
		return -1;
	if (fscanf(fp, "%llu", &val) != 1) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	*out = val;
	return 0;
}

static int
findbatterydevice(char *capacity_path, size_t capacity_len)
{
	DIR *dir;
	struct dirent *ent;
	int have_battery = 0;
	char found[PATH_MAX] = {0};

	if (!capacity_path || capacity_len == 0)
		return 0;

	dir = opendir("/sys/class/power_supply");
	if (!dir)
		return 0;

	while ((ent = readdir(dir))) {
		char type_path[PATH_MAX];
		char cap_path[PATH_MAX];
		struct stat st;
		FILE *fp;
		char type[32] = {0};
		char *nl;

		if (ent->d_name[0] == '.')
			continue;

		if (snprintf(type_path, sizeof(type_path), "/sys/class/power_supply/%s/type",
					ent->d_name) >= (int)sizeof(type_path))
			continue;
		if (snprintf(cap_path, sizeof(cap_path), "/sys/class/power_supply/%s/capacity",
					ent->d_name) >= (int)sizeof(cap_path))
			continue;

		if (stat(type_path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (stat(cap_path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (access(cap_path, R_OK) != 0)
			continue;

		fp = fopen(type_path, "r");
		if (!fp)
			continue;
		if (!fgets(type, sizeof(type), fp)) {
			fclose(fp);
			continue;
		}
		fclose(fp);

		nl = strchr(type, '\n');
		if (nl)
			*nl = '\0';
		if (strcmp(type, "Battery") != 0)
			continue;

		if (snprintf(found, sizeof(found), "%s", cap_path) >= (int)sizeof(found))
			continue;
		have_battery = 1;
		break;
	}

	closedir(dir);

	if (!have_battery)
		return 0;
	if (snprintf(capacity_path, capacity_len, "%s", found) >= (int)capacity_len)
		return 0;
	return 1;
}

static int
readfirstline(const char *path, char *buf, size_t len)
{
	FILE *fp;

	if (!path || !buf || len == 0)
		return -1;
	fp = fopen(path, "r");
	if (!fp)
		return -1;
	if (!fgets(buf, (int)len, fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	for (size_t i = 0; i < len; i++) {
		if (buf[i] == '\n') {
			buf[i] = '\0';
			break;
		}
	}
	return 0;
}

static int
readlinkspeedmbps(const char *iface, int *out)
{
	char path[PATH_MAX];
	char buf[32];
	long val;
	char *end;

	if (!iface || !out)
		return -1;
	if (snprintf(path, sizeof(path), "/sys/class/net/%s/speed", iface)
			>= (int)sizeof(path))
		return -1;
	if (readfirstline(path, buf, sizeof(buf)) != 0)
		return -1;
	errno = 0;
	val = strtol(buf, &end, 10);
	if (errno != 0 || end == buf)
		return -1;
	if (val <= 0)
		return -1;
	*out = (int)val;
	return 0;
}

static int
iface_is_wireless(const char *iface)
{
	char path[PATH_MAX];
	struct stat st;

	if (!iface)
		return 0;
	if (snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", iface)
			>= (int)sizeof(path))
		return 0;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int
findactiveinterface(char *iface, size_t len, int *is_wireless)
{
	DIR *dir;
	struct dirent *ent;
	char best_wifi[IF_NAMESIZE] = {0};
	char best_wired[IF_NAMESIZE] = {0};
	char state[32];

	if (!iface || len == 0)
		return 0;

	dir = opendir("/sys/class/net");
	if (!dir)
		return 0;

	while ((ent = readdir(dir))) {
		char oper[PATH_MAX];

		if (ent->d_name[0] == '.')
			continue;
		if (strcmp(ent->d_name, "lo") == 0)
			continue;

		if (snprintf(oper, sizeof(oper), "/sys/class/net/%s/operstate",
					ent->d_name) >= (int)sizeof(oper))
			continue;
		if (readfirstline(oper, state, sizeof(state)) != 0)
			continue;
		if (strcmp(state, "up") != 0)
			continue;

		if (iface_is_wireless(ent->d_name)) {
			snprintf(best_wifi, sizeof(best_wifi), "%s", ent->d_name);
		} else if (!best_wired[0]) {
			snprintf(best_wired, sizeof(best_wired), "%s", ent->d_name);
		}

		if (best_wifi[0])
			break;
	}

	closedir(dir);

	if (best_wifi[0]) {
		if (snprintf(iface, len, "%s", best_wifi) >= (int)len)
			return 0;
		if (is_wireless)
			*is_wireless = 1;
		return 1;
	}
	if (best_wired[0]) {
		if (snprintf(iface, len, "%s", best_wired) >= (int)len)
			return 0;
		if (is_wireless)
			*is_wireless = 0;
		return 1;
	}
	return 0;
}

__attribute__((unused)) static int
readssid(const char *iface, char *out, size_t len)
{
	/* Deprecated: synchronous SSID lookup removed to avoid blocking */
	(void)iface;
	if (!out || len == 0)
		return 0;
	out[0] = '\0';
	return 0;
}

static int
localip(const char *iface, char *out, size_t len)
{
	struct ifaddrs *ifaddr, *ifa;
	int ret = 0;

	if (!iface || !out || len == 0)
		return 0;

	if (getifaddrs(&ifaddr) == -1)
		return 0;

	for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
		void *addr;
		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_RUNNING))
			continue;
		if (strcmp(ifa->ifa_name, iface) != 0)
			continue;

		addr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
		if (inet_ntop(AF_INET, addr, out, (socklen_t)len)) {
			ret = 1;
			break;
		}
	}

	freeifaddrs(ifaddr);
	return ret;
}

static double
wireless_signal_percent(const char *iface)
{
	FILE *fp;
	char line[256];
	double quality = -1.0;

	if (!iface || !*iface)
		return -1.0;

	fp = fopen("/proc/net/wireless", "r");
	if (!fp)
		return -1.0;

	for (int i = 0; i < 2; i++) {
		if (!fgets(line, sizeof(line), fp))
			break;
	}

	while (fgets(line, sizeof(line), fp)) {
		char name[IF_NAMESIZE] = {0};
		double link = -1.0;

		if (sscanf(line, " %[^:]: %*[^ ] %lf", name, &link) == 2) {
			if (strcmp(name, iface) == 0) {
				quality = link;
				break;
			}
		}
	}

	fclose(fp);
	if (quality < 0.0)
		return -1.0;

	quality = (quality / 70.0) * 100.0;
	if (quality > 100.0)
		quality = 100.0;
	if (quality < 0.0)
		quality = 0.0;
	quality = round(quality);
	return quality;
}

static void
format_speed(double bps, char *out, size_t len)
{
	const char *unit = "kbps";
	double val = bps / 1000.0;

	if (!out || len == 0)
		return;

	if (bps < 0.0) {
		snprintf(out, len, "--");
		return;
	}

	if (val >= 1000.0) {
		val /= 1000.0;
		unit = "Mbps";
	}
	if (val >= 1000.0) {
		val /= 1000.0;
		unit = "Gbps";
	}

	if (val >= 100.0)
		snprintf(out, len, "%.0f %s", val, unit);
	else
		snprintf(out, len, "%.1f %s", val, unit);
}

static const char *
wifi_icon_for_quality(double quality_pct)
{
	if (quality_pct >= 75.0)
		return net_icon_wifi_100_resolved[0] ? net_icon_wifi_100_resolved : net_icon_wifi_100;
	if (quality_pct >= 50.0)
		return net_icon_wifi_75_resolved[0] ? net_icon_wifi_75_resolved : net_icon_wifi_75;
	if (quality_pct >= 25.0)
		return net_icon_wifi_50_resolved[0] ? net_icon_wifi_50_resolved : net_icon_wifi_50;
	return net_icon_wifi_25_resolved[0] ? net_icon_wifi_25_resolved : net_icon_wifi_25;
}

static void
set_net_icon_path(const char *path)
{
	if (!path || !*path)
		path = net_icon_no_conn_resolved[0] ? net_icon_no_conn_resolved : net_icon_no_conn;

	if (strncmp(net_icon_path, path, sizeof(net_icon_path)) != 0) {
		snprintf(net_icon_path, sizeof(net_icon_path), "%s", path);
	}
}

static void
request_public_ip_async(void)
{
	const char *cmd;
	int pipefd[2] = {-1, -1};
	time_t now = time(NULL);

	if (now == (time_t)-1)
		return;

	if (net_public_ip_last != 0 && (now - net_public_ip_last) < 300)
		return;

	if (public_ip_pid > 0 || public_ip_event)
		return;

	cmd = getenv("DWL_PUBLIC_IP_CMD");
	if (!cmd || !cmd[0])
		cmd = "curl -4 -s https://ifconfig.me";

	if (pipe(pipefd) != 0)
		return;

	public_ip_pid = fork();
	if (public_ip_pid == 0) {
		/* child */
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		_exit(127);
	} else if (public_ip_pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		public_ip_pid = -1;
		return;
	}

	/* parent */
	close(pipefd[1]);
	public_ip_fd = pipefd[0];
	fcntl(public_ip_fd, F_SETFL, fcntl(public_ip_fd, F_GETFL) | O_NONBLOCK);
	public_ip_len = 0;
	public_ip_buf[0] = '\0';
	net_public_ip_last = now; /* rate-limit even if fetch fails */
	public_ip_event = wl_event_loop_add_fd(event_loop, public_ip_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP,
			public_ip_event_cb, NULL);
	if (!public_ip_event)
		stop_public_ip_fetch();
}

static void
stop_public_ip_fetch(void)
{
	if (public_ip_event) {
		wl_event_source_remove(public_ip_event);
		public_ip_event = NULL;
	}
	if (public_ip_fd >= 0) {
		close(public_ip_fd);
		public_ip_fd = -1;
	}
	if (public_ip_pid > 0) {
		waitpid(public_ip_pid, NULL, WNOHANG);
		public_ip_pid = -1;
	}
	public_ip_len = 0;
	public_ip_buf[0] = '\0';
}

static int
public_ip_event_cb(int fd, uint32_t mask, void *data)
{
	ssize_t n;
	(void)data;

	(void)mask;

	for (;;) {
		if (public_ip_len >= sizeof(public_ip_buf) - 1)
			break;
		n = read(fd, public_ip_buf + public_ip_len,
				sizeof(public_ip_buf) - 1 - public_ip_len);
		if (n > 0) {
			public_ip_len += (size_t)n;
			continue;
		} else if (n == 0) {
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		} else {
			break;
		}
	}

	public_ip_buf[public_ip_len] = '\0';
	for (size_t i = 0; i < public_ip_len; i++) {
		if (public_ip_buf[i] == '\n' || public_ip_buf[i] == '\r') {
			public_ip_buf[i] = '\0';
			break;
		}
	}
	if (public_ip_buf[0])
		snprintf(net_public_ip, sizeof(net_public_ip), "%s", public_ip_buf);
	else if (!net_public_ip[0])
		snprintf(net_public_ip, sizeof(net_public_ip), "--");
	net_public_ip_last = time(NULL);
	stop_public_ip_fetch();
	return 0;
}

static void
request_ssid_async(const char *iface)
{
	int pipefd[2] = {-1, -1};
	char cmd[256];
	const char *iw = "/run/current-system/sw/bin/iw";
	const char *nmcli = "/run/current-system/sw/bin/nmcli";

	if (!iface || !*iface)
		return;
	if (ssid_pid > 0 || ssid_event)
		return;
	if (access(nmcli, X_OK) != 0) {
		nmcli = "/run/wrappers/bin/nmcli";
		if (access(nmcli, X_OK) != 0)
			nmcli = "nmcli";
	}
	if (access(iw, X_OK) != 0) {
		iw = "/run/wrappers/bin/iw";
		if (access(iw, X_OK) != 0)
			iw = "iw";
	}
	if (snprintf(cmd, sizeof(cmd),
				"%s -t -f active,ssid dev wifi | grep '^yes' | cut -d: -f2 || "
				"%s dev %s link 2>/dev/null",
				nmcli, iw, iface) >= (int)sizeof(cmd))
		return;

	if (pipe(pipefd) != 0)
		return;

	ssid_pid = fork();
	if (ssid_pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		_exit(127);
	} else if (ssid_pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		ssid_pid = -1;
		return;
	}

	close(pipefd[1]);
	ssid_fd = pipefd[0];
	fcntl(ssid_fd, F_SETFL, fcntl(ssid_fd, F_GETFL) | O_NONBLOCK);
	ssid_len = 0;
	ssid_buf[0] = '\0';
	ssid_event = wl_event_loop_add_fd(event_loop, ssid_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP,
			ssid_event_cb, NULL);
	if (!ssid_event)
		stop_ssid_fetch();
}

static void
stop_ssid_fetch(void)
{
	if (ssid_event) {
		wl_event_source_remove(ssid_event);
		ssid_event = NULL;
	}
	if (ssid_fd >= 0) {
		close(ssid_fd);
		ssid_fd = -1;
	}
	if (ssid_pid > 0) {
		waitpid(ssid_pid, NULL, WNOHANG);
		ssid_pid = -1;
	}
	ssid_len = 0;
	ssid_buf[0] = '\0';
}

static int
ssid_event_cb(int fd, uint32_t mask, void *data)
{
	ssize_t n;
	(void)data;
	(void)mask;

	for (;;) {
		if (ssid_len >= sizeof(ssid_buf) - 1)
			break;
		n = read(fd, ssid_buf + ssid_len, sizeof(ssid_buf) - 1 - ssid_len);
		if (n > 0) {
			ssid_len += (size_t)n;
			continue;
		} else if (n == 0) {
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		} else {
			break;
		}
	}

	ssid_buf[ssid_len] = '\0';
	if (ssid_buf[0]) {
		char *line = ssid_buf;
		while (line && *line) {
			char *next = strpbrk(line, "\r\n");
			if (next)
				*next = '\0';

			/* Try iw-style output first */
			char *ssid = strstr(line, "SSID");
			if (ssid) {
				while (*ssid && *ssid != ':' && *ssid != ' ')
					ssid++;
				while (*ssid == ':' || *ssid == ' ' || *ssid == '\t')
					ssid++;
			} else {
				ssid = line;
				while (*ssid == ' ' || *ssid == '\t')
					ssid++;
			}

			if (ssid && *ssid) {
				snprintf(net_ssid, sizeof(net_ssid), "%s", ssid);
				break;
			}

			line = next ? next + 1 : NULL;
		}
	}
	if (!net_ssid[0])
		snprintf(net_ssid, sizeof(net_ssid), "WiFi");
	ssid_last_time = time(NULL);
	stop_ssid_fetch();
	return 0;
}

static double
net_bytes_to_rate(unsigned long long cur, unsigned long long prev, double elapsed)
{
	if (elapsed <= 0.0 || cur < prev)
		return -1.0;
	return (double)(cur - prev) / elapsed;
}

static uint64_t
monotonic_msec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

static void
tray_emit_host_registered(void)
{
	if (!tray_bus)
		return;
	sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher",
			"StatusNotifierHostRegistered", "");
	sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
			"org.freedesktop.StatusNotifierWatcher",
			"StatusNotifierHostRegistered", "");
}

static int
tray_search_item_path(const char *service, const char *start_path,
		char *out, size_t outlen, int depth)
{
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	const char *xml;
	const char *p;
	int r = -1;

	if (!tray_bus || !service || !start_path || !out || outlen == 0 || depth <= 0)
		return -EINVAL;

	r = sd_bus_call_method(tray_bus, service, start_path,
			"org.freedesktop.DBus.Introspectable", "Introspect",
			&err, &reply, "");
	if (r < 0)
		goto out;

	if (sd_bus_message_read(reply, "s", &xml) < 0 || !xml) {
		r = -EINVAL;
		goto out;
	}

	if (strstr(xml, "org.kde.StatusNotifierItem")
			|| strstr(xml, "org.freedesktop.StatusNotifierItem")) {
		snprintf(out, outlen, "%s", start_path);
		r = 0;
		goto out;
	}

	if (depth <= 1) {
		r = 1;
		goto out;
	}

	p = xml;
	while ((p = strstr(p, "<node name=\""))) {
		char name[128];
		char child[256];
		const char *end;
		size_t nlen;

		p += strlen("<node name=\"");
		end = strchr(p, '"');
		if (!end)
			break;
		nlen = (size_t)(end - p);
		if (nlen == 0 || nlen >= sizeof(name)) {
			p = end ? end + 1 : p + 1;
			continue;
		}
		memcpy(name, p, nlen);
		name[nlen] = '\0';

		if (strcmp(start_path, "/") == 0)
			snprintf(child, sizeof(child), "/%s", name);
		else
			snprintf(child, sizeof(child), "%s/%s", start_path, name);

		if (tray_search_item_path(service, child, out, outlen, depth - 1) == 0) {
			r = 0;
			goto out;
		}
		p = end + 1;
	}

out:
	sd_bus_error_free(&err);
	sd_bus_message_unref(reply);
	return r;
}

static int
tray_find_item_path(const char *service, char *path, size_t pathlen)
{
	static const char *candidates[] = {
		"/StatusNotifierItem",
		"/org/ayatana/NotificationItem",
	};

	if (!service || !path || pathlen == 0)
		return -EINVAL;

	for (size_t i = 0; i < LENGTH(candidates); i++) {
		if (tray_search_item_path(service, candidates[i], path, pathlen, 1) == 0)
			return 0;
	}

	if (tray_search_item_path(service, "/", path, pathlen, 6) == 0)
		return 0;

	snprintf(path, pathlen, "%s", "/StatusNotifierItem");
	return -1;
}

static void
tray_sanitize_label(const char *src, char *dst, size_t len)
{
	size_t di = 0;
	int in_tag = 0;

	if (!dst || len == 0) {
		return;
	}

	dst[0] = '\0';
	if (!src)
		return;

	for (size_t i = 0; src[i] && di + 1 < len; i++) {
		char c = src[i];
		if (c == '<') {
			in_tag = 1;
			continue;
		}
		if (in_tag) {
			if (c == '>')
				in_tag = 0;
			continue;
		}
		if (c == '_' || c == '&')
			continue;
		dst[di++] = c;
	}

	while (di > 0 && (dst[di - 1] == ' ' || dst[di - 1] == '\t'))
		di--;
	dst[di] = '\0';
}

static void
tray_menu_clear(TrayMenu *menu)
{
	TrayMenuEntry *e, *tmp;

	if (!menu)
		return;

	wl_list_for_each_safe(e, tmp, &menu->entries, link) {
		wl_list_remove(&e->link);
		free(e);
	}
	wl_list_init(&menu->entries);

	menu->width = menu->height = 0;
	menu->x = menu->y = 0;
	menu->visible = 0;
	menu->service[0] = '\0';
	menu->menu_path[0] = '\0';

	if (menu->tree)
		wlr_scene_node_set_enabled(&menu->tree->node, 0);
}

static void
tray_menu_hide(Monitor *m)
{
	if (!m)
		return;
	tray_menu_clear(&m->statusbar.tray_menu);
}

static void
tray_menu_hide_all(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link)
		tray_menu_hide(m);
}

static int
tray_item_get_menu_path(TrayItem *it)
{
	static const char *ifaces[] = {
		"org.kde.StatusNotifierItem",
		"org.freedesktop.StatusNotifierItem",
	};
	const char *item_path;
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	const char *path = NULL;
	int r = -1;

	if (!tray_bus || !it)
		return -EINVAL;

	it->has_menu = 0;
	it->menu[0] = '\0';
	item_path = it->path[0] ? it->path : "/StatusNotifierItem";

	for (size_t i = 0; i < LENGTH(ifaces); i++) {
		sd_bus_error_free(&err);
		reply = NULL;
		r = sd_bus_call_method(tray_bus, it->service,
				item_path,
				"org.freedesktop.DBus.Properties", "Get",
				&err, &reply, "ss", ifaces[i], "Menu");
		if (r < 0)
			continue;
		if (sd_bus_message_enter_container(reply, 'v', "o") < 0) {
			sd_bus_message_unref(reply);
			continue;
		}
		if (sd_bus_message_read(reply, "o", &path) < 0) {
			sd_bus_message_exit_container(reply);
			sd_bus_message_unref(reply);
			continue;
		}
		sd_bus_message_exit_container(reply);
		if (path && path[0] && strcmp(path, "/") != 0) {
			snprintf(it->menu, sizeof(it->menu), "%s", path);
			it->has_menu = 1;
			sd_bus_message_unref(reply);
			sd_bus_error_free(&err);
			return 0;
		}
		wlr_log(WLR_ERROR, "tray: service %s Menu property empty/invalid", it->service);
		sd_bus_message_unref(reply);
	}

	sd_bus_error_free(&err);
	wlr_log(WLR_ERROR, "tray: failed to get Menu property for %s: %s",
			it->service, strerror(-r));
	return -1;
}

static int
tray_menu_parse_node(sd_bus_message *msg, TrayMenu *menu, int depth, int max_depth)
{
	int r;

	if (!msg || !menu)
		return -EINVAL;

	r = sd_bus_message_enter_container(msg, 'r', "ia{sv}av");
	if (r < 0)
		return r;
	if (r == 0)
		return 0;
	if (depth < 0 || depth > max_depth + 4) /* sanity */
		return -EINVAL;

	r = tray_menu_parse_node_body(msg, menu, depth, max_depth);
	sd_bus_message_exit_container(msg);
	return r;
}

static int
tray_menu_parse_node_body(sd_bus_message *msg, TrayMenu *menu, int depth, int max_depth)
{
	TrayMenuEntry *entry;
	int id = 0, r = 0;
	int enabled = 1, visible = 1;
	int is_separator = 0, has_submenu = 0;
	int toggle_type = 0, toggle_state = 0;
	int child_count = 0;
	char label[256] = {0};

	if (!msg || !menu)
		return -EINVAL;
	if (depth > max_depth + 4)
		return -EINVAL;

	if (sd_bus_message_read(msg, "i", &id) < 0)
		return -EINVAL;

	r = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (r < 0)
		return r;
	while ((r = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		const char *key = NULL;
		if (sd_bus_message_read(msg, "s", &key) < 0) {
			sd_bus_message_exit_container(msg);
			r = -EINVAL;
			break;
		}
		if (key && strcmp(key, "label") == 0) {
			const char *val = NULL;
			if (sd_bus_message_enter_container(msg, 'v', "s") >= 0) {
				if (sd_bus_message_read(msg, "s", &val) >= 0 && val)
					tray_sanitize_label(val, label, sizeof(label));
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (key && strcmp(key, "enabled") == 0) {
			int v = 1;
			if (sd_bus_message_enter_container(msg, 'v', "b") >= 0) {
				sd_bus_message_read(msg, "b", &v);
				enabled = v;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (key && strcmp(key, "visible") == 0) {
			int v = 1;
			if (sd_bus_message_enter_container(msg, 'v', "b") >= 0) {
				sd_bus_message_read(msg, "b", &v);
				visible = v;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (key && strcmp(key, "type") == 0) {
			const char *val = NULL;
			if (sd_bus_message_enter_container(msg, 'v', "s") >= 0) {
				if (sd_bus_message_read(msg, "s", &val) >= 0 && val
						&& strcmp(val, "separator") == 0)
					is_separator = 1;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (key && strcmp(key, "toggle-type") == 0) {
			const char *val = NULL;
			if (sd_bus_message_enter_container(msg, 'v', "s") >= 0) {
				if (sd_bus_message_read(msg, "s", &val) >= 0 && val) {
					if (strcmp(val, "checkmark") == 0)
						toggle_type = 1;
					else if (strcmp(val, "radio") == 0)
						toggle_type = 2;
				}
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (key && strcmp(key, "toggle-state") == 0) {
			int32_t state = 0;
			if (sd_bus_message_enter_container(msg, 'v', "i") >= 0) {
				sd_bus_message_read(msg, "i", &state);
				if (state < 0)
					state = 0;
				toggle_state = state;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else if (key && strcmp(key, "children-display") == 0) {
			const char *val = NULL;
			if (sd_bus_message_enter_container(msg, 'v', "s") >= 0) {
				if (sd_bus_message_read(msg, "s", &val) >= 0 && val
						&& strcmp(val, "submenu") == 0)
					has_submenu = 1;
				sd_bus_message_exit_container(msg);
			} else {
				sd_bus_message_skip(msg, "v");
			}
		} else {
			sd_bus_message_skip(msg, "v");
		}
		sd_bus_message_exit_container(msg);
	}
	sd_bus_message_exit_container(msg);
	if (r < 0)
		return r;

	{
		r = sd_bus_message_enter_container(msg, 'a', "(ia{sv}av)");
		if (r < 0)
			return r;
		while ((r = sd_bus_message_enter_container(msg, 'r', "ia{sv}av")) > 0) {
			child_count++;
			if (depth < max_depth) {
				int cr = tray_menu_parse_node_body(msg, menu, depth + 1, max_depth);
				if (cr < 0) {
					sd_bus_message_exit_container(msg);
					r = cr;
					break;
				}
			} else {
				if (sd_bus_message_skip(msg, "ia{sv}av") < 0) {
					sd_bus_message_exit_container(msg);
					r = -EINVAL;
					break;
				}
			}
			sd_bus_message_exit_container(msg);
		}
		sd_bus_message_exit_container(msg);
		if (r < 0)
			return r;
	}

	if (!visible || depth == 0 || depth > max_depth)
		return 0;

	entry = ecalloc(1, sizeof(*entry));
	entry->id = id;
	entry->enabled = enabled;
	entry->is_separator = is_separator;
	entry->depth = depth - 1;
	entry->has_submenu = has_submenu || child_count > 0;
	entry->toggle_type = toggle_type;
	entry->toggle_state = toggle_state;
	if (label[0])
		snprintf(entry->label, sizeof(entry->label), "%s", label);

	wl_list_insert(menu->entries.prev, &entry->link);
	return 0;
}

static void
tray_menu_draw_text(struct wlr_scene_tree *tree, const char *text, int x, int y, int row_h)
{
	tll(struct GlyphRun) glyphs = tll_init();
	const struct fcft_glyph *glyph;
	struct wlr_scene_buffer *scene_buf;
	struct wlr_buffer *buffer;
	uint32_t prev_cp = 0;
	int pen_x = 0;
	int min_y = INT_MAX, max_y = INT_MIN;

	if (!tree || !text || !*text || row_h <= 0)
		return;
	if (!statusfont.font)
		return;

	for (size_t i = 0; text[i]; i++) {
		long kern_x = 0, kern_y = 0;
		uint32_t cp = (unsigned char)text[i];

		if (prev_cp)
			fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
		pen_x += (int)kern_x;

		glyph = fcft_rasterize_char_utf32(statusfont.font, cp, statusbar_font_subpixel);
		if (glyph && glyph->pix) {
			tll_push_back(glyphs, ((struct GlyphRun){
				.glyph = glyph,
				.pen_x = pen_x,
				.codepoint = cp,
			}));
			if (-glyph->y < min_y)
				min_y = -glyph->y;
			if (-glyph->y + glyph->height > max_y)
				max_y = -glyph->y + glyph->height;
			pen_x += glyph->advance.x;
			if (text[i + 1])
				pen_x += statusbar_font_spacing;
		}
		prev_cp = cp;
	}

	if (tll_length(glyphs) == 0) {
		tll_free(glyphs);
		return;
	}

	{
		int text_height = max_y - min_y;
		int origin_y = y + (row_h - text_height) / 2 - min_y;

		tll_foreach(glyphs, it) {
			glyph = it->item.glyph;
			buffer = statusbar_buffer_from_glyph(glyph);
			if (!buffer)
				continue;

			scene_buf = wlr_scene_buffer_create(tree, NULL);
			if (scene_buf) {
				wlr_scene_buffer_set_buffer(scene_buf, buffer);
				wlr_scene_node_set_position(&scene_buf->node,
						x + it->item.pen_x + glyph->x,
						origin_y - glyph->y);
			}
			wlr_buffer_drop(buffer);
		}
	}

	tll_free(glyphs);
}

static void
tray_menu_render(Monitor *m)
{
	TrayMenu *menu;
	struct wlr_scene_node *node, *tmp;
	TrayMenuEntry *entry;
	int padding, line_spacing, indent_w;
	int max_width = 0;
	int total_height;
	int row_h;

	if (!m || !m->statusbar.tray_menu.tree)
		return;
	menu = &m->statusbar.tray_menu;

	padding = statusbar_module_padding;
	line_spacing = 4;
	indent_w = statusbar_font_spacing > 0 ? statusbar_font_spacing * 2 : 10;

	wl_list_for_each_safe(node, tmp, &menu->tree->children, link) {
		if (menu->bg && node == &menu->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!statusfont.font || wl_list_empty(&menu->entries)) {
		menu->width = menu->height = 0;
		wlr_scene_node_set_enabled(&menu->tree->node, 0);
		menu->visible = 0;
		return;
	}

	row_h = statusfont.height + line_spacing;
	if (row_h < statusfont.height)
		row_h = statusfont.height;
	total_height = padding * 2;

	wl_list_for_each(entry, &menu->entries, link) {
		int text_w = entry->is_separator ? 0 : status_text_width(
				entry->label[0] ? entry->label : " ");
		int row_width = text_w + 2 * padding + indent_w * entry->depth;
		if (entry->toggle_type)
			row_width += row_h;
		if (entry->has_submenu)
			row_width += row_h / 2;
		max_width = MAX(max_width, row_width);
		total_height += entry->is_separator ? line_spacing : row_h;
	}

	if (max_width <= 0)
		max_width = 80;

	menu->width = max_width;
	menu->height = total_height;

	if (!menu->bg && !(menu->bg = wlr_scene_tree_create(menu->tree)))
		return;
	wlr_scene_node_set_enabled(&menu->bg->node, 1);
	wlr_scene_node_set_position(&menu->bg->node, 0, 0);
	wl_list_for_each_safe(node, tmp, &menu->bg->children, link)
		wlr_scene_node_destroy(node);
	drawrect(menu->bg, 0, 0, menu->width, menu->height, statusbar_bg);

	{
		int y = padding;
		wl_list_for_each(entry, &menu->entries, link) {
			char text[512];
			int row = entry->is_separator ? line_spacing : row_h;
			int x = padding + indent_w * entry->depth;

			entry->y = y;
			entry->height = row;

			if (entry->is_separator) {
				int sep_y = y + row / 2;
				drawrect(menu->tree, padding, sep_y, menu->width - 2 * padding, 1, statusbar_fg);
				y += row;
				continue;
			}

			text[0] = '\0';
			if (entry->toggle_type == 1) {
				snprintf(text, sizeof(text), "%s%s%s",
						entry->toggle_state ? "[x] " : "[ ] ",
						entry->label[0] ? entry->label : "",
						entry->has_submenu ? "  >" : "");
			} else if (entry->toggle_type == 2) {
				snprintf(text, sizeof(text), "%s%s%s",
						entry->toggle_state ? "(o) " : "( ) ",
						entry->label[0] ? entry->label : "",
						entry->has_submenu ? "  >" : "");
			} else {
				snprintf(text, sizeof(text), "%s%s",
						entry->label[0] ? entry->label : " ",
						entry->has_submenu ? "  >" : "");
			}

			tray_menu_draw_text(menu->tree, text, x, y, row);
			y += row;
		}
	}
}

static __attribute__((unused)) int
tray_menu_open_at(Monitor *m, TrayItem *it, int icon_x)
{
	sd_bus_message *req = NULL, *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	char *props[] = {"label", "enabled", "type", "children-display",
		"toggle-type", "toggle-state", "visible", NULL};
	int r;
	int max_depth = 3; /* limited depth for safety; can be raised if needed */
	TrayMenu *menu;
	int desired_x, max_x;
	const char *sig;

	if (!m || !m->showbar || !m->statusbar.tray_menu.tree || !it || !tray_bus)
		return 0;
	if (!m->statusbar.area.width || !m->statusbar.area.height)
		return 0;

	if (tray_item_get_menu_path(it) != 0 || !it->has_menu)
		return 0;

	menu = &m->statusbar.tray_menu;
	tray_menu_hide_all();
	tray_menu_clear(menu);
	wl_list_init(&menu->entries);
	if (!menu->tree) {
		return 0;
	}
	if (max_depth < 1)
		max_depth = 1;

	/* Temporarily disable custom menu parsing to avoid crashes; fall back to client ContextMenu */
	return 0;

	r = sd_bus_message_new_method_call(tray_bus, &req, it->service, it->menu,
			"com.canonical.dbusmenu", "GetLayout");
	if (r < 0)
		goto fail;

	r = sd_bus_message_append(req, "iias", 0, max_depth, props);
	if (r < 0)
		goto fail;

	r = sd_bus_call(tray_bus, req, 2 * 1000 * 1000, &err, &reply);
	if (r < 0)
		goto fail;

	sig = sd_bus_message_get_signature(reply, 0);
	if (!sig || !strstr(sig, "(ia{sv}av)")) {
		r = -EINVAL;
		goto fail;
	}

	{
		int revision = 0;
		if (sd_bus_message_read(reply, "i", &revision) < 0)
			goto fail;
		(void)revision;
		if ((r = tray_menu_parse_node(reply, menu, 0, max_depth)) < 0)
			goto fail;
	}

	sd_bus_message_unref(req);
	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);

	if (wl_list_empty(&menu->entries))
		goto fail;

	snprintf(menu->service, sizeof(menu->service), "%s", it->service);
	snprintf(menu->menu_path, sizeof(menu->menu_path), "%s", it->menu);

	tray_menu_render(m);
	if (menu->width <= 0 || menu->height <= 0)
		goto fail;

	desired_x = icon_x - menu->width / 2;
	if (desired_x < 0)
		desired_x = 0;
	max_x = m->statusbar.area.width - menu->width;
	if (max_x < 0)
		max_x = 0;
	if (desired_x > max_x)
		desired_x = max_x;

	menu->x = desired_x;
	menu->y = m->statusbar.area.height;
	wlr_scene_node_set_position(&menu->tree->node, menu->x, menu->y);
	wlr_scene_node_set_enabled(&menu->tree->node, 1);
	menu->visible = 1;
	return 1;

fail:
	wlr_log(WLR_ERROR, "tray: GetLayout failed for %s%s: %s",
			it->service, it->menu, err.message ? err.message : strerror(-r));
	if (req)
		sd_bus_message_unref(req);
	if (reply)
		sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
	tray_menu_hide_all();
	return 0;
}

static TrayMenuEntry *
tray_menu_entry_at(Monitor *m, int lx, int ly)
{
	TrayMenuEntry *entry;
	TrayMenu *menu;

	if (!m)
		return NULL;
	menu = &m->statusbar.tray_menu;
	wl_list_for_each(entry, &menu->entries, link) {
		if (ly >= entry->y && ly < entry->y + entry->height) {
			if (entry->is_separator || !entry->enabled)
				return NULL;
			return entry;
		}
	}
	return NULL;
}

static int
tray_menu_send_event(TrayMenu *menu, TrayMenuEntry *entry, uint32_t time_msec)
{
	sd_bus_message *msg = NULL, *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	int r;

	if (!tray_bus || !menu || !entry || !menu->service[0] || !menu->menu_path[0])
		return -EINVAL;

	r = sd_bus_message_new_method_call(tray_bus, &msg, menu->service, menu->menu_path,
			"com.canonical.dbusmenu", "Event");
	if (r < 0)
		goto out;

	r = sd_bus_message_append(msg, "is", entry->id, "clicked");
	if (r < 0)
		goto out;

	r = sd_bus_message_open_container(msg, 'v', "s");
	if (r < 0)
		goto out;
	r = sd_bus_message_append_basic(msg, 's', "");
	if (r < 0)
		goto out;
	r = sd_bus_message_close_container(msg);
	if (r < 0)
		goto out;

	r = sd_bus_message_append(msg, "u", time_msec);
	if (r < 0)
		goto out;

	r = sd_bus_call(tray_bus, msg, 2 * 1000 * 1000, &err, &reply);

out:
	if (msg)
		sd_bus_message_unref(msg);
	if (reply)
		sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
	return r < 0 ? r : 0;
}

static void
connect_wifi_ssid(const char *ssid)
{
	if (!ssid || !*ssid)
		return;
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "/bin/sh", "-c",
				"nmcli device wifi connect \"$1\" >/dev/null 2>&1",
				"nmcli-connect", ssid, (char *)NULL);
		_exit(0);
	}
}

static void
connect_wifi_with_prompt(const char *ssid, int secure)
{
	if (!ssid || !*ssid)
		return;
	/* Prefer zenity if present, else fallback to a terminal prompt */
	if (secure) {
		const char *cmd =
			"if command -v zenity >/dev/null 2>&1; then "
				"pw=$(zenity --entry --hide-text --text=\"Passphrase for $1\" --title=\"Wi-Fi\" 2>/dev/null) || exit 1; "
			"elif command -v wofi >/dev/null 2>&1; then "
				"pw=$(printf '' | wofi --dmenu --password --prompt \"Passphrase for $1\" 2>/dev/null) || exit 1; "
			"elif command -v bemenu >/dev/null 2>&1; then "
				"pw=$(printf '' | bemenu -p \"Passphrase for $1\" -P 2>/dev/null) || exit 1; "
			"elif command -v dmenu >/dev/null 2>&1; then "
				"pw=$(printf '' | dmenu -p \"Passphrase for $1\" 2>/dev/null) || exit 1; "
			"else exit 1; fi; "
			"nmcli device wifi connect \"$1\" password \"$pw\" >/dev/null 2>&1";
		if (fork() == 0) {
			setsid();
			execl("/bin/sh", "/bin/sh", "-c", cmd, "nmcli-connect", ssid, (char *)NULL);
			_exit(0);
		}
		return;
	}
	/* Fallback: attempt plain connect (will fail if not saved) */
	connect_wifi_ssid(ssid);
}

static void
wifi_networks_clear(void)
{
	WifiNetwork *n, *tmp;
	wl_list_for_each_safe(n, tmp, &wifi_networks, link) {
		wl_list_remove(&n->link);
		free(n);
	}
	wl_list_init(&wifi_networks);
}

static void
net_menu_hide_all(void)
{
	Monitor *m;
	int any_visible = 0;

	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.net_menu.tree) {
			if (m->statusbar.net_menu.visible)
				any_visible = 1;
			wlr_scene_node_set_enabled(&m->statusbar.net_menu.tree->node, 0);
			m->statusbar.net_menu.visible = 0;
		}
		if (m->statusbar.net_menu.submenu_tree) {
			wlr_scene_node_set_enabled(&m->statusbar.net_menu.submenu_tree->node, 0);
			m->statusbar.net_menu.submenu_visible = 0;
		}
		m->statusbar.net_menu.hover = -1;
		m->statusbar.net_menu.submenu_hover = -1;
	}
	if (any_visible) {
		wifi_networks_accept_updates = 0;
		wifi_networks_freeze_existing = 0;
		wifi_networks_generation++;
		wifi_networks_clear();
	}
}

static void
net_menu_render(Monitor *m)
{
	const char *label = "Available networks";
	int padding = statusbar_module_padding;
	int line_spacing = 4;
	int row_h;
	int text_w;
	int max_w;
	NetMenu *menu;
	struct wlr_scene_node *node, *tmp;
	float border_col[4] = {0, 0, 0, 1};
	int border_px = 1;

	if (!m || !m->statusbar.net_menu.tree)
		return;
	menu = &m->statusbar.net_menu;

	wl_list_for_each_safe(node, tmp, &menu->tree->children, link) {
		if (menu->bg && node == &menu->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!statusfont.font) {
		menu->width = menu->height = 0;
		wlr_scene_node_set_enabled(&menu->tree->node, 0);
		menu->visible = 0;
		return;
	}

	row_h = statusfont.height + line_spacing;
	if (row_h < statusfont.height)
		row_h = statusfont.height;
	text_w = status_text_width(label);
	max_w = text_w + 2 * padding + row_h; /* space for arrow */
	menu->width = max_w;
	menu->height = row_h + 2 * padding;

	if (!menu->bg && !(menu->bg = wlr_scene_tree_create(menu->tree)))
		return;
	wlr_scene_node_set_enabled(&menu->bg->node, 1);
	wlr_scene_node_set_position(&menu->bg->node, 0, 0);
	wl_list_for_each_safe(node, tmp, &menu->bg->children, link)
		wlr_scene_node_destroy(node);
	drawrect(menu->bg, 0, 0, menu->width, menu->height, net_menu_row_bg);
	drawrect(menu->bg, 0, 0, menu->width, border_px, border_col);
	drawrect(menu->bg, 0, menu->height - border_px, menu->width, border_px, border_col);
	drawrect(menu->bg, 0, 0, border_px, menu->height, border_col);
	drawrect(menu->bg, menu->width - border_px, 0, border_px, menu->height, border_col);

	{
		char text[256];
		snprintf(text, sizeof(text), "%s  >", label);
		drawrect(menu->tree, padding, padding, menu->width - 2 * padding, row_h,
				menu->hover == 0 ? net_menu_row_bg_hover : net_menu_row_bg);
		tray_menu_draw_text(menu->tree, text, padding + 4, padding, row_h);
	}

	menu->visible = 1;
	wlr_scene_node_set_enabled(&menu->tree->node, 1);
}

static void
net_menu_submenu_render(Monitor *m)
{
	NetMenu *menu;
	struct wlr_scene_node *node, *tmp;
	int padding = statusbar_module_padding;
	int line_spacing = 4;
	int row_h;
	int max_w = 0;
	int total_h = 0;
	int y;
	int count = 0;
	WifiNetwork *n;
	char line[256];
	float border_col[4] = {0, 0, 0, 1};
	int border_px = 1;

	if (!m || !m->statusbar.net_menu.submenu_tree)
		return;
	menu = &m->statusbar.net_menu;

	wl_list_for_each_safe(node, tmp, &menu->submenu_tree->children, link) {
		if (menu->submenu_bg && node == &menu->submenu_bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!statusfont.font) {
		wlr_scene_node_set_enabled(&menu->submenu_tree->node, 0);
		menu->submenu_visible = 0;
		return;
	}

	row_h = statusfont.height + line_spacing;
	if (row_h < statusfont.height)
		row_h = statusfont.height;

	if (wifi_scan_inflight && wl_list_empty(&wifi_networks)) {
		snprintf(line, sizeof(line), "Scanning...");
		max_w = status_text_width(line);
		total_h = row_h + 2 * padding;
	} else if (wl_list_empty(&wifi_networks)) {
		snprintf(line, sizeof(line), "No networks");
		max_w = status_text_width(line);
		total_h = row_h + 2 * padding;
	} else {
		wl_list_for_each(n, &wifi_networks, link) {
			int w;
			count++;
			snprintf(line, sizeof(line), "%s (%d%%%s)", n->ssid, n->strength,
					n->secure ? ", secured" : "");
			w = status_text_width(line);
			if (w > max_w)
				max_w = w;
		}
		total_h = padding * 2 + count * row_h;
	}

	if (max_w <= 0)
		max_w = 100;

	menu->submenu_width = max_w + 2 * padding;
	menu->submenu_height = total_h;

	if (!menu->submenu_bg && !(menu->submenu_bg = wlr_scene_tree_create(menu->submenu_tree)))
		return;
	wlr_scene_node_set_enabled(&menu->submenu_bg->node, 1);
	wlr_scene_node_set_position(&menu->submenu_bg->node, 0, 0);
	wl_list_for_each_safe(node, tmp, &menu->submenu_bg->children, link)
		wlr_scene_node_destroy(node);
	drawrect(menu->submenu_bg, 0, 0, menu->submenu_width, menu->submenu_height, net_menu_row_bg);
	drawrect(menu->submenu_bg, 0, 0, menu->submenu_width, border_px, border_col);
	drawrect(menu->submenu_bg, 0, menu->submenu_height - border_px, menu->submenu_width, border_px, border_col);
	drawrect(menu->submenu_bg, 0, 0, border_px, menu->submenu_height, border_col);
	drawrect(menu->submenu_bg, menu->submenu_width - border_px, 0, border_px, menu->submenu_height, border_col);

	y = padding;
	if (wl_list_empty(&wifi_networks)) {
		drawrect(menu->submenu_tree, padding, y, menu->submenu_width - 2 * padding, row_h,
				net_menu_row_bg);
		tray_menu_draw_text(menu->submenu_tree, line, padding + 4, y, row_h);
	} else {
		int idx = 0;
		wl_list_for_each(n, &wifi_networks, link) {
			int x = padding;
			int hover = (menu->submenu_hover == idx);
			snprintf(line, sizeof(line), "%s (%d%%%s)", n->ssid, n->strength,
					n->secure ? ", secured" : "");
			drawrect(menu->submenu_tree, x, y, menu->submenu_width - 2 * padding, row_h,
					hover ? net_menu_row_bg_hover : net_menu_row_bg);
			tray_menu_draw_text(menu->submenu_tree, line, x + 4, y, row_h);
			n->secure = n->secure ? 1 : 0;
			idx++;
			y += row_h;
		}
	}

	menu->submenu_visible = 1;
	wlr_scene_node_set_enabled(&menu->submenu_tree->node, 1);
}

static void
request_wifi_scan(void)
{
	const char *cmd = "nmcli -t -f SSID,SIGNAL,SECURITY device wifi list --rescan no";
	int pipefd[2] = {-1, -1};

	if (wifi_scan_inflight)
		return;

	if (pipe(pipefd) != 0)
		return;

	wifi_scan_generation = wifi_networks_generation;
	wifi_scan_pid = fork();
	if (wifi_scan_pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		_exit(127);
	} else if (wifi_scan_pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		wifi_scan_pid = -1;
		return;
	}

	close(pipefd[1]);
	wifi_scan_fd = pipefd[0];
	fcntl(wifi_scan_fd, F_SETFL, fcntl(wifi_scan_fd, F_GETFL) | O_NONBLOCK);
	wifi_scan_len = 0;
	wifi_scan_buf[0] = '\0';
	wifi_scan_inflight = 1;
	wifi_scan_event = wl_event_loop_add_fd(event_loop, wifi_scan_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP, wifi_scan_event_cb, NULL);
	if (!wifi_scan_event) {
		wifi_scan_inflight = 0;
		close(wifi_scan_fd);
		wifi_scan_fd = -1;
		if (wifi_scan_pid > 0)
			waitpid(wifi_scan_pid, NULL, WNOHANG);
		wifi_scan_pid = -1;
	}
}

static void
tray_add_item(const char *service, const char *path, int emit_signals)
{
	const char *base;
	TrayItem *it;

	if (!service || !service[0] || !path || !path[0])
		return;

	tray_remove_item(service);
	it = ecalloc(1, sizeof(*it));
	snprintf(it->service, sizeof(it->service), "%s", service);
	snprintf(it->path, sizeof(it->path), "%s", path);
	base = strrchr(service, '.');
	base = base ? base + 1 : service;
	snprintf(it->label, sizeof(it->label), "%s", base);
	it->icon_tried = 0;
	it->icon_failed = 0;
	wl_list_insert(&tray_items, &it->link);
	wlr_log(WLR_INFO, "tray: registered %s%s", service, path);

	tray_update_icons_text();

	if (!emit_signals || !tray_bus)
		return;

	sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher",
			"StatusNotifierItemRegistered", "s", service);
	sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
			"org.freedesktop.StatusNotifierWatcher",
			"StatusNotifierItemRegistered", "s", service);
}

static int
tray_method_register_item(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	const char *arg = NULL;
	const char *sender = sd_bus_message_get_sender(m);
	char service[128] = {0};
	char path[128] = {0};
	sd_bus_error err = SD_BUS_ERROR_NULL;

	(void)userdata;
	(void)ret_error;

	if (sd_bus_message_read(m, "s", &arg) < 0) {
		sd_bus_error_set_const(&err, SD_BUS_ERROR_INVALID_ARGS, "invalid arg");
		return sd_bus_reply_method_error(m, &err);
	}
	if (!sender || !sender[0]) {
		sd_bus_error_set_const(&err, SD_BUS_ERROR_FAILED, "no sender");
		return sd_bus_reply_method_error(m, &err);
	}

	if (arg && strchr(arg, '/')) {
		snprintf(service, sizeof(service), "%s", sender);
		snprintf(path, sizeof(path), "%s", arg);
	} else {
		snprintf(service, sizeof(service), "%s", arg && arg[0] ? arg : sender);
		snprintf(path, sizeof(path), "/StatusNotifierItem");
	}

	if (tray_find_item_path(service, path, sizeof(path)) == 0) {
		/* path updated by helper */
	}

	tray_add_item(service, path, 1);
	tray_emit_host_registered();
	return sd_bus_reply_method_return(m, "");
}

static int
tray_method_register_host(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	(void)userdata;
	(void)ret_error;
	tray_host_registered = 1;
	tray_emit_host_registered();
	return sd_bus_reply_method_return(m, "");
}

static void
tray_scan_existing_items(void)
{
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	const char *name;

	if (!tray_bus)
		return;

	if (sd_bus_call_method(tray_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
				"org.freedesktop.DBus", "ListNames", &err, &reply, "") < 0)
		goto out;
	if (sd_bus_message_enter_container(reply, 'a', "s") < 0)
		goto out;

	while (sd_bus_message_read_basic(reply, 's', &name) > 0) {
		if (!name)
			continue;
		if (strstr(name, "StatusNotifierItem") || strstr(name, "NotificationItem")) {
			char path[128];
			if (tray_find_item_path(name, path, sizeof(path)) != 0)
				snprintf(path, sizeof(path), "%s", "/StatusNotifierItem");
			tray_add_item(name, path, 1);
		}
	}

	sd_bus_message_exit_container(reply);
out:
	sd_bus_error_free(&err);
	sd_bus_message_unref(reply);
}

static int
tray_property_get_registered(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	TrayItem *it;

	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;

	if (sd_bus_message_open_container(reply, 'a', "s") < 0)
		return -EINVAL;

	wl_list_for_each(it, &tray_items, link) {
		char full[256];
		snprintf(full, sizeof(full), "%s%s", it->service, it->path);
		sd_bus_message_append_basic(reply, 's', full);
	}

	return sd_bus_message_close_container(reply);
}

static int
tray_property_get_host_registered(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append_basic(reply, 'b', &tray_host_registered);
}

static int
tray_property_get_protocol_version(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	int version = 1;

	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append_basic(reply, 'i', &version);
}

static int
tray_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	const char *name, *old_owner, *new_owner;

	(void)userdata;
	(void)ret_error;
	if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0)
		return 0;
	if (name && new_owner && *new_owner &&
			(strstr(name, "StatusNotifierItem") || strstr(name, "NotificationItem"))) {
		char path[128];
		if (tray_find_item_path(name, path, sizeof(path)) != 0)
			snprintf(path, sizeof(path), "%s", "/StatusNotifierItem");
		tray_add_item(name, path, 1);
		return 0;
	}
	if (name && old_owner && *old_owner && (!new_owner || !*new_owner))
		tray_remove_item(name);
	return 0;
}

static int
tray_bus_event(int fd, uint32_t mask, void *data)
{
	sd_bus *bus = data;
	int r;
	int events;
	uint32_t newmask;

	(void)fd;
	if (!bus)
		return 0;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		return 0;

	while ((r = sd_bus_process(bus, NULL)) > 0)
		;

	events = sd_bus_get_events(bus);
	newmask = 0;
	if (events & SD_BUS_EVENT_READABLE)
		newmask |= WL_EVENT_READABLE;
	if (events & SD_BUS_EVENT_WRITABLE)
		newmask |= WL_EVENT_WRITABLE;
	if (tray_event)
		wl_event_source_fd_update(tray_event, newmask ? newmask : WL_EVENT_READABLE);

	return 0;
}

static void
tray_init(void)
{
	static const sd_bus_vtable tray_vtable[] = {
		SD_BUS_VTABLE_START(0),
		SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", tray_method_register_item, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", tray_method_register_host, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", tray_property_get_registered, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
		SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", tray_property_get_host_registered, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
		SD_BUS_PROPERTY("ProtocolVersion", "i", tray_property_get_protocol_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
		SD_BUS_VTABLE_END
	};
	int r;
	int fd;
	int events;
	uint32_t mask = WL_EVENT_READABLE;
	uint64_t name_flags = SD_BUS_NAME_ALLOW_REPLACEMENT | SD_BUS_NAME_REPLACE_EXISTING;

	if (tray_bus)
		return;

	r = sd_bus_open_user(&tray_bus);
	if (r < 0) {
		wlr_log(WLR_ERROR, "tray: failed to connect to session bus: %s", strerror(-r));
		tray_bus = NULL;
		return;
	}

	/* Keep icon/property queries from stalling the compositor for too long */
	sd_bus_set_method_call_timeout(tray_bus, 2 * 1000 * 1000); /* 2s */

	r = sd_bus_request_name(tray_bus, "org.kde.StatusNotifierWatcher", name_flags);
	if (r < 0) {
		wlr_log(WLR_ERROR, "tray: failed to acquire watcher name: %s", strerror(-r));
		goto fail;
	}
	/* Some implementations also look for the freedesktop alias */
	sd_bus_request_name(tray_bus, "org.freedesktop.StatusNotifierWatcher", name_flags);
	sd_bus_add_object_vtable(tray_bus, &tray_vtable_slot, "/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher", tray_vtable, NULL);
	sd_bus_add_object_vtable(tray_bus, &tray_fdo_vtable_slot, "/StatusNotifierWatcher",
			"org.freedesktop.StatusNotifierWatcher", tray_vtable, NULL);
	sd_bus_add_match(tray_bus, &tray_name_slot,
		"type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
		tray_name_owner_changed, NULL);

	fd = sd_bus_get_fd(tray_bus);
	events = sd_bus_get_events(tray_bus);
	mask = 0;
	if (events & SD_BUS_EVENT_READABLE)
		mask |= WL_EVENT_READABLE;
	if (events & SD_BUS_EVENT_WRITABLE)
		mask |= WL_EVENT_WRITABLE;
	if (mask == 0)
		mask = WL_EVENT_READABLE;

	tray_event = wl_event_loop_add_fd(event_loop, fd, mask, tray_bus_event, tray_bus);
	tray_scan_existing_items();
	tray_host_registered = 1;
	tray_emit_host_registered();
	tray_update_icons_text();
	return;
fail:
	if (tray_vtable_slot)
		sd_bus_slot_unref(tray_vtable_slot);
	tray_vtable_slot = NULL;
	if (tray_fdo_vtable_slot)
		sd_bus_slot_unref(tray_fdo_vtable_slot);
	tray_fdo_vtable_slot = NULL;
	if (tray_name_slot)
		sd_bus_slot_unref(tray_name_slot);
	tray_name_slot = NULL;
	if (tray_event) {
		wl_event_source_remove(tray_event);
		tray_event = NULL;
	}
	if (tray_bus) {
		sd_bus_unref(tray_bus);
		tray_bus = NULL;
	}
}

static int
findbacklightdevice(char *brightness_path, size_t brightness_len,
		char *max_path, size_t max_len)
{
	DIR *dir;
	struct dirent *ent;
	char w_bpath[PATH_MAX] = {0};
	char w_mpath[PATH_MAX] = {0};
	char r_bpath[PATH_MAX] = {0};
	char r_mpath[PATH_MAX] = {0};
	int have_writable = 0;
	int have_readable = 0;

	if (!brightness_path || !max_path || brightness_len == 0 || max_len == 0)
		return 0;

	dir = opendir("/sys/class/backlight");
	if (!dir)
		return 0;

	backlight_writable = 0;

	while ((ent = readdir(dir))) {
		char bpath[PATH_MAX];
		char mpath[PATH_MAX];
		struct stat st;

		if (ent->d_name[0] == '.')
			continue;

		snprintf(bpath, sizeof(bpath), "/sys/class/backlight/%s/brightness",
				ent->d_name);
		snprintf(mpath, sizeof(mpath), "/sys/class/backlight/%s/max_brightness",
				ent->d_name);

		if (stat(bpath, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (stat(mpath, &st) != 0 || !S_ISREG(st.st_mode))
			continue;

		if (access(bpath, R_OK) != 0 || access(mpath, R_OK) != 0)
			continue;

		if (access(bpath, W_OK) == 0 && !have_writable) {
			if (snprintf(w_bpath, sizeof(w_bpath), "%s", bpath) < (int)sizeof(w_bpath)
					&& snprintf(w_mpath, sizeof(w_mpath), "%s", mpath) < (int)sizeof(w_mpath))
				have_writable = 1;
		}

		if (!have_readable) {
			if (snprintf(r_bpath, sizeof(r_bpath), "%s", bpath) < (int)sizeof(r_bpath)
					&& snprintf(r_mpath, sizeof(r_mpath), "%s", mpath) < (int)sizeof(r_mpath))
				have_readable = 1;
		}
	}

	closedir(dir);

	if (have_writable) {
		if (snprintf(brightness_path, brightness_len, "%s", w_bpath) >= (int)brightness_len)
			return 0;
		if (snprintf(max_path, max_len, "%s", w_mpath) >= (int)max_len)
			return 0;
		backlight_writable = 1;
		return 1;
	}

	if (have_readable) {
		if (snprintf(brightness_path, brightness_len, "%s", r_bpath) >= (int)brightness_len)
			return 0;
		if (snprintf(max_path, max_len, "%s", r_mpath) >= (int)max_len)
			return 0;
		backlight_writable = 0;
		return 1;
	}

	return 0;
}

static int
readulong_cmd(const char *cmd, unsigned long long *out)
{
	FILE *fp;
	char buf[64];
	unsigned long long val;
	char *end = NULL;

	if (!cmd || !out)
		return -1;

	fp = popen(cmd, "r");
	if (!fp)
		return -1;
	if (!fgets(buf, sizeof(buf), fp)) {
		pclose(fp);
		return -1;
	}
	pclose(fp);

	errno = 0;
	val = strtoull(buf, &end, 10);
	if (errno != 0)
		return -1;
	if (end == buf)
		return -1;
	*out = val;
	return 0;
}

static double
backlight_percent(void)
{
	unsigned long long cur, max;
	double percent;

	/* Prefer brightnessctl */
	if (readulong_cmd("brightnessctl g", &cur) == 0 &&
			readulong_cmd("brightnessctl m", &max) == 0 && max > 0) {
		if (cur > max)
			cur = max;
		light_cached_percent = ((double)cur * 100.0) / (double)max;
		return light_cached_percent;
	}

	/* Fallback to light -G */
	{
		FILE *fp = popen("light -G", "r");
		if (fp) {
			if (fscanf(fp, "%lf", &percent) == 1) {
				pclose(fp);
				if (percent < 0.0)
					percent = -1.0;
				if (percent > 100.0)
					percent = 100.0;
				if (percent >= 0.0)
					light_cached_percent = percent;
				return percent;
			}
			pclose(fp);
		}
	}

	/* Fallback to sysfs if available */
	if (backlight_available) {
		if (readulong(backlight_brightness_path, &cur) == 0 &&
				readulong(backlight_max_path, &max) == 0 && max > 0) {
			if (cur > max)
				cur = max;
			light_cached_percent = ((double)cur * 100.0) / (double)max;
			return light_cached_percent;
		}
	}

	/* Fallback to brightnessctl */
	if (readulong_cmd("brightnessctl g", &cur) == 0 &&
			readulong_cmd("brightnessctl m", &max) == 0 && max > 0) {
		if (cur > max)
			cur = max;
		light_cached_percent = ((double)cur * 100.0) / (double)max;
		return light_cached_percent;
	}

	/* Fallback to light -G */
	{
		FILE *fp = popen("light -G", "r");
		if (fp) {
			if (fscanf(fp, "%lf", &percent) == 1) {
				pclose(fp);
				if (percent < 0.0)
					percent = -1.0;
				if (percent > 100.0)
					percent = 100.0;
				if (percent >= 0.0)
					light_cached_percent = percent;
				return percent;
			}
			pclose(fp);
		}
	}

	return light_cached_percent;
}

static int
set_backlight_percent(double percent)
{
	unsigned long long max, target;
	FILE *fp;
	int attempted = 0;

	if (percent < 0.0)
		percent = 0.0;
	if (percent > 100.0)
		percent = 100.0;

	if (backlight_available && readulong(backlight_max_path, &max) == 0 && max > 0) {
		target = (unsigned long long)lround((percent / 100.0) * (double)max);
		if (target > max)
			target = max;

		if (backlight_writable && (fp = fopen(backlight_brightness_path, "w"))) {
			attempted = 1;
			if (fprintf(fp, "%llu", target) >= 0) {
				fclose(fp);
				light_cached_percent = percent;
				return 0;
			}
			fclose(fp);
		}
	}

	/* Prefer external tools first */
	{
		char cmd[128];
		int ret;

		ret = snprintf(cmd, sizeof(cmd), "brightnessctl set %.2f%%", percent);
		if (ret > 0 && ret < (int)sizeof(cmd)) {
			attempted = 1;
			if (system(cmd) == 0) {
				light_cached_percent = percent;
				return 0;
			}
		}

		ret = snprintf(cmd, sizeof(cmd), "light -S %.2f", percent);
		if (ret > 0 && ret < (int)sizeof(cmd)) {
			attempted = 1;
			if (system(cmd) == 0) {
				light_cached_percent = percent;
				return 0;
			}
		}
	}

	/* Fallback to sysfs if available */
	if (backlight_available && readulong(backlight_max_path, &max) == 0 && max > 0) {
		target = (unsigned long long)lround((percent / 100.0) * (double)max);
		if (target > max)
			target = max;

		if (backlight_writable && (fp = fopen(backlight_brightness_path, "w"))) {
			attempted = 1;
			if (fprintf(fp, "%llu", target) >= 0) {
				fclose(fp);
				light_cached_percent = percent;
				return 0;
			}
			fclose(fp);
		}
	}

	return attempted ? -1 : 0;
}

static int
set_backlight_relative(double delta_percent)
{
	char cmd[128];
	int ret;

	if (delta_percent == 0.0)
		return 0;

	/* brightnessctl relative */
	if (delta_percent > 0) {
		ret = snprintf(cmd, sizeof(cmd), "brightnessctl set +%.2f%%", delta_percent);
	} else {
		ret = snprintf(cmd, sizeof(cmd), "brightnessctl set %.2f%%-", -delta_percent);
	}
	if (ret > 0 && ret < (int)sizeof(cmd)) {
		if (system(cmd) == 0)
			return 0;
	}

	/* light relative */
	if (delta_percent > 0) {
		ret = snprintf(cmd, sizeof(cmd), "light -A %.2f", delta_percent);
	} else {
		ret = snprintf(cmd, sizeof(cmd), "light -U %.2f", -delta_percent);
	}
	if (ret > 0 && ret < (int)sizeof(cmd)) {
		if (system(cmd) == 0)
			return 0;
	}

	/* Prefer sysfs if available and writable */
	if (backlight_available && backlight_writable) {
		double cur = backlight_percent();
		if (cur < 0.0)
			cur = light_cached_percent;
		if (cur >= 0.0) {
			double target = cur + delta_percent;
			if (target < 0.0)
				target = 0.0;
			if (target > 100.0)
				target = 100.0;
			return set_backlight_percent(target);
		}
	}

	return -1;
}

static void
apply_startup_defaults(void)
{
	static int applied = 0;
	const double light_default = 40.0;
	const double speaker_default = 70.0;
	const double mic_default = 80.0;

	if (applied)
		return;

	/* Backlight */
	if (set_backlight_percent(light_default) == 0) {
		light_last_percent = light_default;
		light_cached_percent = light_default;
	} else if (light_cached_percent < 0.0) {
		light_cached_percent = light_default;
	}

	/* Speaker/headset volume */
	if (set_pipewire_volume(speaker_default) == 0) {
		speaker_active = speaker_default;
		speaker_stored = speaker_default;
		volume_last_speaker_percent = speaker_default;
		volume_last_headset_percent = speaker_default;
		volume_muted = 0;
	}

	/* Microphone volume */
	if (set_pipewire_mic_volume(mic_default) == 0) {
		microphone_active = mic_default;
		microphone_stored = mic_default;
		mic_last_percent = mic_default;
		mic_muted = 0;
	}

	applied = 1;
}

static double
battery_percent(void)
{
	unsigned long long cur;

	if (!battery_available)
		return -1.0;
	if (readulong(battery_capacity_path, &cur) != 0)
		return -1.0;
	if (cur > 100)
		cur = 100;

	return (double)cur;
}

static struct xkb_rule_names
getxkbrules(void)
{
	struct xkb_rule_names names = xkb_rules;
	const char *env;

	if (!names.rules && (env = getenv("XKB_DEFAULT_RULES")))
		names.rules = env;
	if (!names.model && (env = getenv("XKB_DEFAULT_MODEL")))
		names.model = env;
	if (!names.layout && (env = getenv("XKB_DEFAULT_LAYOUT")))
		names.layout = env;
	if (!names.variant && (env = getenv("XKB_DEFAULT_VARIANT")))
		names.variant = env;
	if (!names.options && (env = getenv("XKB_DEFAULT_OPTIONS")))
		names.options = env;

	return names;
}

static double
volume_last_for_type(int is_headset)
{
	return is_headset ? volume_last_headset_percent : volume_last_speaker_percent;
}

static void
volume_cache_store(int is_headset, double level, int muted, uint64_t now)
{
	if (level < 0.0)
		return;

	if (is_headset) {
		volume_cached_headset = level;
		volume_cached_headset_muted = muted;
		volume_last_read_headset_ms = now;
		volume_last_headset_percent = level;
	} else {
		volume_cached_speaker = level;
		volume_cached_speaker_muted = muted;
		volume_last_read_speaker_ms = now;
		volume_last_speaker_percent = level;
	}
	speaker_active = level;
}

static void
volume_invalidate_cache(int is_headset)
{
	if (is_headset)
		volume_last_read_headset_ms = 0;
	else
		volume_last_read_speaker_ms = 0;
}

static double
pipewire_volume_percent(int *is_headset_out)
{
	FILE *fp;
	char line[128];
	double level = -1.0;
	int muted = 0;
	uint64_t now = monotonic_msec();
	int is_headset = (is_headset_out && (*is_headset_out == 0 || *is_headset_out == 1))
			? *is_headset_out
			: pipewire_sink_is_headset();
	uint64_t last_read = is_headset ? volume_last_read_headset_ms : volume_last_read_speaker_ms;
	double cached = is_headset ? volume_cached_headset : volume_cached_speaker;
	int cached_muted = is_headset ? volume_cached_headset_muted : volume_cached_speaker_muted;

	if (is_headset_out)
		*is_headset_out = is_headset;

	if (last_read != 0 && now - last_read < 8000 && cached >= 0.0) {
		volume_muted = cached_muted;
		if (is_headset)
			volume_last_headset_percent = cached;
		else
			volume_last_speaker_percent = cached;
		return cached;
	}

	fp = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@", "r");
	if (!fp)
		return -1.0;

	if (fgets(line, sizeof(line), fp)) {
		double raw = 0.0;
		if (strstr(line, "[MUTED]"))
			muted = 1;
		if (sscanf(line, "Volume: %lf", &raw) == 1)
			level = raw * 100.0;
	}

	pclose(fp);
	volume_muted = muted;
	volume_cache_store(is_headset, level, muted, now);
	if (level >= 0.0)
		speaker_active = level;
	return level;
}

static double
pipewire_mic_volume_percent(void)
{
	FILE *fp;
	char line[128];
	double level = -1.0;
	int muted = 0;
	uint64_t now = monotonic_msec();

	if (mic_last_read_ms != 0 && now - mic_last_read_ms < 8000) {
		if (mic_cached >= 0.0) {
			mic_muted = mic_cached_muted;
			return mic_cached;
		}
	}

	fp = popen("wpctl get-volume @DEFAULT_AUDIO_SOURCE@", "r");
	if (!fp)
		return -1.0;

	if (fgets(line, sizeof(line), fp)) {
		double raw = 0.0;
		if (strstr(line, "[MUTED]"))
			muted = 1;
		if (sscanf(line, "Volume: %lf", &raw) == 1)
			level = raw * 100.0;
	}

	pclose(fp);
	mic_muted = muted;
	if (level >= 0.0) {
		mic_cached = level;
		mic_cached_muted = muted;
		mic_last_read_ms = now;
		microphone_active = level;
	}
	return level;
}

static int
pipewire_sink_is_headset(void)
{
	FILE *fp;
	char line[512];
	int headset = 0;
	const char *kw[] = {
		"headset", "headphone", "headphones", "earbud", "earbuds",
		"earphone", "handsfree", "bluez", "bluetooth", "a2dp",
		"hfp", "hsp", "head-unit"
	};
	size_t i;

	fp = popen("wpctl inspect @DEFAULT_AUDIO_SINK@", "r");
	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp)) {
		for (i = 0; i < LENGTH(kw); i++) {
			if (strcasestr(line, kw[i])) {
				headset = 1;
				break;
			}
		}
		if (headset)
			break;
	}

	pclose(fp);
	if (headset)
		return 1;

	fp = popen("wpctl status", "r");
	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp)) {
		if (!strchr(line, '*'))
			continue;
		for (i = 0; i < LENGTH(kw); i++) {
			if (strcasestr(line, kw[i])) {
				headset = 1;
				break;
			}
		}
		if (headset)
			break;
	}

	pclose(fp);
	return headset;
}

static int
set_pipewire_mute(int mute)
{
	char cmd[128];
	int ret;

	ret = snprintf(cmd, sizeof(cmd), "wpctl set-mute @DEFAULT_AUDIO_SINK@ %d", mute ? 1 : 0);
	if (ret < 0 || ret >= (int)sizeof(cmd))
		return -1;

	ret = system(cmd);
	if (ret != 0)
		return -1;

	/* Re-read to confirm state */
	pipewire_volume_percent(NULL);
	return 0;
}

static int
set_pipewire_mic_mute(int mute)
{
	char cmd[128];
	int ret;

	ret = snprintf(cmd, sizeof(cmd), "wpctl set-mute @DEFAULT_AUDIO_SOURCE@ %d", mute ? 1 : 0);
	if (ret < 0 || ret >= (int)sizeof(cmd))
		return -1;

	ret = system(cmd);
	if (ret != 0)
		return -1;

	pipewire_mic_volume_percent();
	return 0;
}

static int
set_pipewire_volume(double percent)
{
	char cmd[128];
	int ret;

	if (percent < 0.0)
		percent = 0.0;
	if (percent > volume_max_percent)
		percent = volume_max_percent;

	ret = snprintf(cmd, sizeof(cmd),
			"wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f%%", percent);
	if (ret < 0 || ret >= (int)sizeof(cmd))
		return -1;

	ret = system(cmd);
	return ret == 0 ? 0 : -1;
}

static int
set_pipewire_mic_volume(double percent)
{
	char cmd[128];
	int ret;

	if (percent < 0.0)
		percent = 0.0;
	if (percent > mic_max_percent)
		percent = mic_max_percent;

	ret = snprintf(cmd, sizeof(cmd),
			"wpctl set-volume @DEFAULT_AUDIO_SOURCE@ %.2f%%", percent);
	if (ret < 0 || ret >= (int)sizeof(cmd))
		return -1;

	ret = system(cmd);
	if (ret == 0)
		mic_last_percent = percent;
	return ret == 0 ? 0 : -1;
}

static int
toggle_pipewire_mute(void)
{
	int ret;
	int current;
	int is_headset = pipewire_sink_is_headset();
	double vol;
	uint64_t now;
	double base;
	double target;

	volume_invalidate_cache(is_headset); /* force fresh read for active sink */
	vol = speaker_active;
	if (vol < 0.0)
		vol = pipewire_volume_percent(&is_headset);
	current = vol >= 0.0 ? volume_muted : 0;
	base = vol >= 0.0 ? vol : volume_last_for_type(is_headset);

	if (current == 0) { /* muting */
		ret = set_pipewire_mute(1);
		if (ret != 0)
			return -1;
		volume_muted = 1;
		now = monotonic_msec();
		if (base >= 0.0) {
			volume_cache_store(is_headset, base, volume_muted, now);
			speaker_stored = base;
		} else {
			volume_invalidate_cache(is_headset);
		}
	} else { /* unmuting */
		target = speaker_stored >= 0.0 ? speaker_stored : base;
		ret = set_pipewire_mute(0);
		if (ret != 0)
			return -1;
		volume_muted = 0;
		if (target >= 0.0)
			set_pipewire_volume(target);
		if (target >= 0.0) {
			speaker_active = target;
		}
		volume_invalidate_cache(is_headset);
		pipewire_volume_percent(&is_headset); /* refresh cache from PipeWire */
	}

	refreshstatusvolume();
	return 0;
}

static int
toggle_pipewire_mic_mute(void)
{
	int ret;
	int current;
	double vol;
	double base;
	double target;

	mic_last_read_ms = 0;
	vol = microphone_active;
	if (vol < 0.0)
		vol = pipewire_mic_volume_percent();
	current = vol >= 0.0 ? mic_muted : 0;
	base = vol >= 0.0 ? vol : mic_last_percent;

	if (current == 0) { /* muting */
		ret = set_pipewire_mic_mute(1);
		if (ret != 0)
			return -1;
		mic_muted = 1;
		mic_cached_muted = mic_muted;
		if (base >= 0.0) {
			mic_cached = base;
			mic_last_percent = base;
			microphone_active = base;
			mic_last_read_ms = monotonic_msec();
			microphone_stored = base;
		}
	} else { /* unmuting */
		target = microphone_stored >= 0.0 ? microphone_stored : base;
		ret = set_pipewire_mic_mute(0);
		if (ret != 0)
			return -1;
		mic_muted = 0;
		mic_cached_muted = mic_muted;
		mic_last_read_ms = 0;
		mic_cached = -1.0;
		if (target >= 0.0) {
			set_pipewire_mic_volume(target);
			mic_cached = target;
			mic_last_percent = target;
			microphone_active = target;
			mic_last_read_ms = monotonic_msec();
		} else {
			pipewire_mic_volume_percent(); /* refresh cache from PipeWire */
		}
	}

	if (mic_last_percent < 0.0)
		mic_last_percent = mic_cached >= 0.0 ? mic_cached : mic_last_percent;
	refreshstatusmic();
	return 0;
}

static int
loadstatusfont(void)
{
	size_t count;

	if (statusfont.font)
		return 1;

	count = LENGTH(statusbar_fonts);
	if (count == 0)
		return 0;

	statusfont.font = fcft_from_name(count, statusbar_fonts, statusbar_font_attributes);
	if (!statusfont.font)
		return 0;

	statusfont.ascent = statusfont.font->ascent;
	statusfont.descent = statusfont.font->descent;
	statusfont.height = statusfont.font->height;
	return 1;
}

static void
freestatusfont(void)
{
	if (statusfont.font)
		fcft_destroy(statusfont.font);
	statusfont.font = NULL;
}

static void
positionstatusmodules(Monitor *m)
{
	int x, spacing;

	if (!m || !m->statusbar.tree)
		return;

	if (!m->showbar || !m->statusbar.area.width || !m->statusbar.area.height) {
		wlr_scene_node_set_enabled(&m->statusbar.tree->node, 0);
		if (m->statusbar.tags.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.tags.tree->node, 0);
			m->statusbar.tags.x = 0;
		}
		if (m->statusbar.traylabel.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.traylabel.tree->node, 0);
			m->statusbar.traylabel.x = 0;
		}
		if (m->statusbar.sysicons.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.sysicons.tree->node, 0);
			m->statusbar.sysicons.x = 0;
		}
		if (m->statusbar.tray_menu.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.tray_menu.tree->node, 0);
			m->statusbar.tray_menu.visible = 0;
		}
		if (m->statusbar.cpu.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.cpu.tree->node, 0);
			m->statusbar.cpu.x = 0;
		}
		if (m->statusbar.net.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.net.tree->node, 0);
			m->statusbar.net.x = 0;
		}
		if (m->statusbar.battery.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.battery.tree->node, 0);
			m->statusbar.battery.x = 0;
		}
			if (m->statusbar.light.tree) {
				wlr_scene_node_set_enabled(&m->statusbar.light.tree->node, 0);
				m->statusbar.light.x = 0;
			}
			if (m->statusbar.mic.tree) {
				wlr_scene_node_set_enabled(&m->statusbar.mic.tree->node, 0);
				m->statusbar.mic.x = 0;
			}
			if (m->statusbar.volume.tree) {
				wlr_scene_node_set_enabled(&m->statusbar.volume.tree->node, 0);
				m->statusbar.volume.x = 0;
			}
		if (m->statusbar.ram.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.ram.tree->node, 0);
			m->statusbar.ram.x = 0;
		}
		if (m->statusbar.clock.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.clock.tree->node, 0);
			m->statusbar.clock.x = 0;
		}
		if (m->statusbar.cpu_popup.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.cpu_popup.tree->node, 0);
			m->statusbar.cpu_popup.visible = 0;
		}
		if (m->statusbar.net_popup.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.net_popup.tree->node, 0);
			m->statusbar.net_popup.visible = 0;
		}
		return;
	}

	wlr_scene_node_set_enabled(&m->statusbar.tree->node, 1);
	if (m->statusbar.tags.tree)
		wlr_scene_node_set_enabled(&m->statusbar.tags.tree->node,
				m->statusbar.tags.width > 0);
	if (m->statusbar.traylabel.tree)
		wlr_scene_node_set_enabled(&m->statusbar.traylabel.tree->node,
				m->statusbar.traylabel.width > 0);
	if (m->statusbar.sysicons.tree)
		wlr_scene_node_set_enabled(&m->statusbar.sysicons.tree->node,
				m->statusbar.sysicons.width > 0);
	if (m->statusbar.cpu.tree)
		wlr_scene_node_set_enabled(&m->statusbar.cpu.tree->node,
				m->statusbar.cpu.width > 0);
	if (m->statusbar.net.tree)
		wlr_scene_node_set_enabled(&m->statusbar.net.tree->node,
				m->statusbar.net.width > 0);
	if (m->statusbar.battery.tree)
		wlr_scene_node_set_enabled(&m->statusbar.battery.tree->node,
				m->statusbar.battery.width > 0);
	if (m->statusbar.light.tree)
		wlr_scene_node_set_enabled(&m->statusbar.light.tree->node,
				m->statusbar.light.width > 0);
	if (m->statusbar.mic.tree)
		wlr_scene_node_set_enabled(&m->statusbar.mic.tree->node,
				m->statusbar.mic.width > 0);
	if (m->statusbar.volume.tree)
		wlr_scene_node_set_enabled(&m->statusbar.volume.tree->node,
				m->statusbar.volume.width > 0);
	if (m->statusbar.ram.tree)
		wlr_scene_node_set_enabled(&m->statusbar.ram.tree->node,
				m->statusbar.ram.width > 0);
	if (m->statusbar.clock.tree)
		wlr_scene_node_set_enabled(&m->statusbar.clock.tree->node,
				m->statusbar.clock.width > 0);
	if (m->statusbar.cpu_popup.tree && m->statusbar.cpu.width > 0) {
		if (!m->statusbar.cpu_popup.visible)
			wlr_scene_node_set_enabled(&m->statusbar.cpu_popup.tree->node, 0);
	}
	x = 0;
	spacing = statusbar_module_spacing;

	if (m->statusbar.tags.width > 0) {
		wlr_scene_node_set_position(&m->statusbar.tags.tree->node, x, 0);
		m->statusbar.tags.x = x;
		x += m->statusbar.tags.width + spacing;
	}
	if (m->statusbar.net.width > 0) {
		wlr_scene_node_set_position(&m->statusbar.net.tree->node, x, 0);
		m->statusbar.net.x = x;
		x += m->statusbar.net.width + spacing;
	}
	if (m->statusbar.traylabel.width > 0) {
		wlr_scene_node_set_position(&m->statusbar.traylabel.tree->node, x, 0);
		m->statusbar.traylabel.x = x;
		x += m->statusbar.traylabel.width + spacing;
	}
	if (m->statusbar.sysicons.width > 0) {
		wlr_scene_node_set_position(&m->statusbar.sysicons.tree->node, x, 0);
		m->statusbar.sysicons.x = x;
		x += m->statusbar.sysicons.width + spacing;
	}

	x = m->statusbar.area.width;
	spacing = statusbar_module_spacing;

	if (m->statusbar.clock.width > 0) {
		x -= m->statusbar.clock.width;
		wlr_scene_node_set_position(&m->statusbar.clock.tree->node, x, 0);
		m->statusbar.clock.x = x;
		x -= spacing;
	}
	if (m->statusbar.ram.width > 0) {
		x -= m->statusbar.ram.width;
		wlr_scene_node_set_position(&m->statusbar.ram.tree->node, x, 0);
		m->statusbar.ram.x = x;
		x -= spacing;
	}
	if (m->statusbar.cpu.width > 0) {
		x -= m->statusbar.cpu.width;
		wlr_scene_node_set_position(&m->statusbar.cpu.tree->node, x, 0);
		m->statusbar.cpu.x = x;
		x -= spacing;
	}
	if (m->statusbar.mic.width > 0) {
		x -= m->statusbar.mic.width;
		wlr_scene_node_set_position(&m->statusbar.mic.tree->node, x, 0);
		m->statusbar.mic.x = x;
		x -= spacing;
	}
	if (m->statusbar.volume.width > 0) {
		x -= m->statusbar.volume.width;
		wlr_scene_node_set_position(&m->statusbar.volume.tree->node, x, 0);
		m->statusbar.volume.x = x;
		x -= spacing;
	}
	if (m->statusbar.light.width > 0) {
		x -= m->statusbar.light.width;
		wlr_scene_node_set_position(&m->statusbar.light.tree->node, x, 0);
		m->statusbar.light.x = x;
		x -= spacing;
	}
	if (m->statusbar.battery.width > 0) {
		x -= m->statusbar.battery.width;
		wlr_scene_node_set_position(&m->statusbar.battery.tree->node, x, 0);
		m->statusbar.battery.x = x;
		x -= spacing;
	}
	if (m->statusbar.cpu_popup.tree) {
		if (m->statusbar.cpu.width > 0 && m->statusbar.area.height > 0) {
			int popup_x = m->statusbar.cpu.x;
			int max_x = m->statusbar.area.width - m->statusbar.cpu_popup.width;
			if (max_x < 0)
				max_x = 0;
			if (popup_x > max_x)
				popup_x = max_x;
			if (popup_x < 0)
				popup_x = 0;
			wlr_scene_node_set_position(&m->statusbar.cpu_popup.tree->node,
					popup_x, m->statusbar.area.height);
			m->statusbar.cpu_popup.refresh_data = 1;
		} else {
			wlr_scene_node_set_enabled(&m->statusbar.cpu_popup.tree->node, 0);
			m->statusbar.cpu_popup.visible = 0;
			m->statusbar.cpu_popup.refresh_data = 0;
		}
	}
	if (m->statusbar.net_popup.tree) {
		int icon_w = m->statusbar.sysicons.width;
		int pos_x = icon_w > 0 ? m->statusbar.sysicons.x : m->statusbar.net_popup.anchor_x;

		if (m->statusbar.area.height > 0) {
			wlr_scene_node_set_position(&m->statusbar.net_popup.tree->node,
					pos_x, m->statusbar.area.height);
			if (icon_w > 0) {
				m->statusbar.net_popup.anchor_x = pos_x;
				m->statusbar.net_popup.anchor_y = m->statusbar.area.height;
				m->statusbar.net_popup.anchor_w = icon_w;
			}
			if (!m->statusbar.net_popup.visible)
				wlr_scene_node_set_enabled(&m->statusbar.net_popup.tree->node, 0);
		} else {
			wlr_scene_node_set_enabled(&m->statusbar.net_popup.tree->node, 0);
			m->statusbar.net_popup.visible = 0;
		}
	}
	if (m->statusbar.tray_menu.tree) {
		if (m->statusbar.tray_menu.visible && m->statusbar.area.height > 0) {
			int max_x = m->statusbar.area.width - m->statusbar.tray_menu.width;
			int menu_x = m->statusbar.tray_menu.x;
			if (max_x < 0)
				max_x = 0;
			if (menu_x > max_x)
				menu_x = max_x;
			if (menu_x < 0)
				menu_x = 0;
			m->statusbar.tray_menu.x = menu_x;
			m->statusbar.tray_menu.y = m->statusbar.area.height;
			wlr_scene_node_set_position(&m->statusbar.tray_menu.tree->node,
					m->statusbar.tray_menu.x, m->statusbar.tray_menu.y);
			wlr_scene_node_set_enabled(&m->statusbar.tray_menu.tree->node, 1);
		} else {
			wlr_scene_node_set_enabled(&m->statusbar.tray_menu.tree->node, 0);
			m->statusbar.tray_menu.visible = 0;
		}
	}
}

static int
wifi_network_exists(const char *ssid)
{
	WifiNetwork *it;

	if (!ssid || !*ssid)
		return 0;
	wl_list_for_each(it, &wifi_networks, link) {
		if (strncmp(it->ssid, ssid, sizeof(it->ssid)) == 0)
			return 1;
	}
	return 0;
}

static void
wifi_networks_insert_sorted(WifiNetwork *n)
{
	WifiNetwork *it;

	/* de-duplicate by SSID, keep strongest and mark secure if any entry is secure */
	wl_list_for_each(it, &wifi_networks, link) {
		if (strncmp(it->ssid, n->ssid, sizeof(it->ssid)) == 0) {
			if (n->strength > it->strength)
				it->strength = n->strength;
			if (n->secure)
				it->secure = 1;
			free(n);
			return;
		}
	}

	wl_list_for_each(it, &wifi_networks, link) {
		if (n->strength > it->strength) {
			wl_list_insert(it->link.prev, &n->link);
			return;
		}
	}
	wl_list_insert(wifi_networks.prev, &n->link);
}

static void
wifi_scan_finish(void)
{
	if (wifi_scan_event) {
		wl_event_source_remove(wifi_scan_event);
		wifi_scan_event = NULL;
	}
	if (wifi_scan_fd >= 0) {
		close(wifi_scan_fd);
		wifi_scan_fd = -1;
	}
	if (wifi_scan_pid > 0) {
		waitpid(wifi_scan_pid, NULL, WNOHANG);
		wifi_scan_pid = -1;
	}
	wifi_scan_inflight = 0;
}

static int
wifi_scan_event_cb(int fd, uint32_t mask, void *data)
{
	ssize_t n;
	char *saveptr = NULL;
	char *line;
	int discard_results;
	(void)data;

	discard_results = (!wifi_networks_accept_updates)
			|| (wifi_scan_generation != wifi_networks_generation);

	for (;;) {
		if (wifi_scan_len >= sizeof(wifi_scan_buf) - 1)
			break;
		n = read(fd, wifi_scan_buf + wifi_scan_len,
				sizeof(wifi_scan_buf) - 1 - wifi_scan_len);
		if (n > 0) {
			wifi_scan_len += (size_t)n;
			continue;
		} else if (n == 0) {
			break;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		} else {
			break;
		}
	}

	wifi_scan_buf[wifi_scan_len] = '\0';

	if (discard_results) {
		wifi_scan_finish();
		if (wifi_networks_accept_updates && !wifi_scan_inflight)
			request_wifi_scan();
		wifi_scan_plan_rescan();
		return 0;
	}

	if (!wifi_networks_freeze_existing)
		wifi_networks_clear();

	line = strtok_r(wifi_scan_buf, "\n", &saveptr);
	while (line) {
		char *sig_s, *sec_s;
		WifiNetwork *nentry;
		int strength = 0;
		int secure = 0;
		char ssid[128] = {0};

		sig_s = strchr(line, ':');
		if (sig_s) {
			*sig_s = '\0';
			sig_s++;
			sec_s = strchr(sig_s, ':');
			if (sec_s) {
				*sec_s = '\0';
				sec_s++;
			}
		} else {
			sec_s = NULL;
		}

		if (line[0])
			snprintf(ssid, sizeof(ssid), "%s", line);
		if (!ssid[0]) {
			line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}
		if (wifi_networks_freeze_existing && wifi_network_exists(ssid)) {
			line = strtok_r(NULL, "\n", &saveptr);
			continue;
		}
		if (sig_s)
			strength = atoi(sig_s);
		if (sec_s && *sec_s && strcmp(sec_s, "--") != 0)
			secure = 1;

		nentry = ecalloc(1, sizeof(*nentry));
		snprintf(nentry->ssid, sizeof(nentry->ssid), "%s", ssid);
		nentry->strength = strength;
		nentry->secure = secure;
		wifi_networks_insert_sorted(nentry);

		line = strtok_r(NULL, "\n", &saveptr);
	}

	wifi_scan_finish();

	{
		Monitor *m;
		wl_list_for_each(m, &mons, link) {
			if (m->statusbar.net_menu.visible) {
				net_menu_submenu_render(m);
				if (m->statusbar.net_menu.submenu_tree)
					wlr_scene_node_set_position(&m->statusbar.net_menu.submenu_tree->node,
							m->statusbar.net_menu.submenu_x, m->statusbar.net_menu.submenu_y);
			}
		}
	}
	wifi_scan_plan_rescan();
	return 0;
}

static void
wifi_scan_plan_rescan(void)
{
	if (wifi_scan_timer)
		wl_event_source_timer_update(wifi_scan_timer, 2000);
}

static int
wifi_scan_timer_cb(void *data)
{
	Monitor *m;
	int visible = 0;
	(void)data;

	if (wifi_scan_inflight)
		return 0;

	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.net_menu.visible) {
			visible = 1;
			break;
		}
	}

	if (visible)
		request_wifi_scan();

	return 0;
}

static int
cpu_popup_refresh_timeout(void *data)
{
	Monitor *m;
	int any_visible = 0;

	(void)data;

	wl_list_for_each(m, &mons, link) {
		CpuPopup *p = &m->statusbar.cpu_popup;
		if (!p || !p->tree || !p->visible)
			continue;
		p->suppress_refresh_until_ms = 0;
		p->refresh_data = 1;
		rendercpupopup(m);
		any_visible = 1;
	}

	if (any_visible)
		wl_event_source_timer_update(cpu_popup_refresh_timer, cpu_popup_refresh_interval_ms);

	return 0;
}

static void
schedule_cpu_popup_refresh(uint32_t ms)
{
	if (!event_loop)
		return;
	if (!cpu_popup_refresh_timer)
		cpu_popup_refresh_timer = wl_event_loop_add_timer(event_loop,
				cpu_popup_refresh_timeout, NULL);
	if (cpu_popup_refresh_timer)
		wl_event_source_timer_update(cpu_popup_refresh_timer, ms);
}

static void
net_menu_open(Monitor *m)
{
	const int offset = statusbar_module_padding;
	int desired_x, max_x;
	NetMenu *menu;

	if (!m || !m->showbar || !m->statusbar.net_menu.tree || !m->statusbar.sysicons.tree)
		return;
	if (!m->statusbar.area.width || !m->statusbar.area.height)
		return;

	menu = &m->statusbar.net_menu;
	net_menu_hide_all();
	wifi_networks_generation++;
	wifi_networks_accept_updates = 1;
	wifi_networks_freeze_existing = 1;
	wifi_networks_clear();
	if (m->statusbar.net_popup.tree) {
		wlr_scene_node_set_enabled(&m->statusbar.net_popup.tree->node, 0);
		m->statusbar.net_popup.visible = 0;
	}
	request_wifi_scan();
	net_menu_render(m);
	if (menu->width <= 0 || menu->height <= 0)
		return;

	desired_x = m->statusbar.sysicons.x - menu->width / 2 + m->statusbar.sysicons.width / 2;
	if (desired_x < 0)
		desired_x = 0;
	max_x = m->statusbar.area.width - menu->width;
	if (max_x < 0)
		max_x = 0;
	if (desired_x > max_x)
		desired_x = max_x;

	menu->x = desired_x;
	menu->y = m->statusbar.area.height + statusbar_module_padding;
	wlr_scene_node_set_position(&menu->tree->node, menu->x, menu->y);
	wlr_scene_node_set_enabled(&menu->tree->node, 1);
	menu->visible = 1;
	menu->hover = 0;

	menu->submenu_x = menu->x + menu->width + offset;
	menu->submenu_y = menu->y + offset;
	if (menu->submenu_tree) {
		net_menu_submenu_render(m);
		wlr_scene_node_set_position(&menu->submenu_tree->node, menu->submenu_x, menu->submenu_y);
	}
	wifi_scan_plan_rescan();
}

static void
net_menu_update_hover(Monitor *m, double cx, double cy)
{
	NetMenu *menu;
	int lx, ly;
	int row_h, padding;
	int prev_hover, prev_sub;
	int new_hover = -1, new_sub = -1;

	if (!m || !m->statusbar.net_menu.visible || !m->statusbar.net_menu.submenu_tree)
		return;
	menu = &m->statusbar.net_menu;
	lx = (int)floor(cx) - m->statusbar.area.x;
	ly = (int)floor(cy) - m->statusbar.area.y;
	padding = statusbar_module_padding;
	row_h = statusfont.height + 4;
	if (row_h < statusfont.height)
		row_h = statusfont.height;

	if (lx >= menu->x && lx < menu->x + menu->width &&
			ly >= menu->y && ly < menu->y + menu->height) {
		new_hover = 0;
	}

	if (menu->submenu_visible &&
			lx >= menu->submenu_x && lx < menu->submenu_x + menu->submenu_width &&
			ly >= menu->submenu_y && ly < menu->submenu_y + menu->submenu_height) {
		if (wl_list_empty(&wifi_networks)) {
			new_sub = -1;
		} else {
			int rel_y = ly - menu->submenu_y - padding;
			if (rel_y >= 0) {
				new_sub = rel_y / row_h;
			}
		}
	}

	prev_hover = menu->hover;
	prev_sub = menu->submenu_hover;
	menu->hover = new_hover;
	menu->submenu_hover = new_sub;
	if (menu->hover != prev_hover)
		net_menu_render(m);
	if (menu->submenu_hover != prev_sub) {
		net_menu_submenu_render(m);
		wlr_scene_node_set_position(&menu->submenu_tree->node, menu->submenu_x, menu->submenu_y);
	}
}

static WifiNetwork *
wifi_network_at_index(int idx)
{
	WifiNetwork *n;
	int i = 0;
	wl_list_for_each(n, &wifi_networks, link) {
		if (i == idx)
			return n;
		i++;
	}
	return NULL;
}

static int
net_menu_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	NetMenu *menu;
	int padding = statusbar_module_padding;
	int row_h;

	if (!m || !m->statusbar.net_menu.visible)
		return 0;
	menu = &m->statusbar.net_menu;
	row_h = statusfont.height + 4;
	if (row_h < statusfont.height)
		row_h = statusfont.height;

	if (lx >= menu->x && lx < menu->x + menu->width &&
			ly >= menu->y && ly < menu->y + menu->height) {
		/* main menu, only one entry */
		return 1;
	}

	if (menu->submenu_visible &&
			lx >= menu->submenu_x && lx < menu->submenu_x + menu->submenu_width &&
			ly >= menu->submenu_y && ly < menu->submenu_y + menu->submenu_height) {
		if (button != BTN_LEFT)
			return 1;
		if (!wl_list_empty(&wifi_networks)) {
			int rel_y = ly - menu->submenu_y - padding;
			int idx = rel_y / row_h;
			if (rel_y >= 0) {
				WifiNetwork *n = wifi_network_at_index(idx);
				if (n) {
					connect_wifi_with_prompt(n->ssid, n->secure);
					net_menu_hide_all();
					return 1;
				}
			}
		}
		return 1;
	}

	net_menu_hide_all();
	return 1;
}

static void
layoutstatusbar(Monitor *m, const struct wlr_box *area, struct wlr_box *client_area)
{
	int gap, bar_height;
	struct wlr_box bar_area = {0};

	if (!m || !m->statusbar.tree || !area || !client_area)
		return;

	if (!m->showbar) {
		wlr_scene_node_set_enabled(&m->statusbar.tree->node, 0);
		hidetagthumbnail(m);
		*client_area = *area;
		m->statusbar.area = (struct wlr_box){0};
		return;
	}

	gap = m->gaps ? gappx : 0;
	bar_height = MIN((int)statusbar_height, area->height);

	bar_area.x = area->x + gap;
	bar_area.y = area->y + statusbar_top_gap;
	bar_area.width = area->width - 2 * gap;
	bar_area.height = bar_height;

	if (bar_area.width < 0)
		bar_area.width = 0;
	if (bar_area.height < 0)
		bar_area.height = 0;

	wlr_scene_node_set_enabled(&m->statusbar.tree->node, 1);
	wlr_scene_node_set_position(&m->statusbar.tree->node, bar_area.x, bar_area.y);
	m->statusbar.area = bar_area;

	renderworkspaces(m, &m->statusbar.tags, bar_area.height);
	if (m->statusbar.traylabel.tree) {
		clearstatusmodule(&m->statusbar.traylabel);
		m->statusbar.traylabel.width = 0;
		wlr_scene_node_set_enabled(&m->statusbar.traylabel.tree->node, 0);
	}
	if (m->statusbar.sysicons.tree)
		rendertrayicons(m, bar_area.height);
	if (m->statusbar.cpu.tree)
		rendercpu(&m->statusbar.cpu, bar_area.height, cpu_text);
	if (m->statusbar.net.tree)
		rendernet(&m->statusbar.net, bar_area.height, net_text);
	if (m->statusbar.light.tree)
		renderlight(&m->statusbar.light, bar_area.height, light_text);
	if (m->statusbar.battery.tree)
		renderbattery(&m->statusbar.battery, bar_area.height, battery_text);
	if (m->statusbar.volume.tree)
		rendervolume(&m->statusbar.volume, bar_area.height, volume_text);
	if (m->statusbar.ram.tree)
		renderram(&m->statusbar.ram, bar_area.height, ram_text);
	if (m->statusbar.cpu_popup.tree && m->statusbar.cpu_popup.visible)
		rendercpupopup(m);
	if (m->statusbar.net_popup.tree && m->statusbar.net_popup.visible)
		rendernetpopup(m);
	positionstatusmodules(m);

	*client_area = *area;
	client_area->y = area->y + statusbar_top_gap + bar_area.height;
	client_area->height = area->height - bar_area.height - statusbar_top_gap;
	if (client_area->height < 0)
		client_area->height = 0;
}

static void
refreshstatusclock(void)
{
	time_t now;
	struct tm tm;
	char timestr[6] = {0};
	Monitor *m;
	int barh;

	now = time(NULL);
	if (now == (time_t)-1)
		return;
	if (!localtime_r(&now, &tm))
		return;
	if (!strftime(timestr, sizeof(timestr), "%H:%M", &tm))
		return;

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.clock.tree || !m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		renderclock(&m->statusbar.clock, barh, timestr);
		positionstatusmodules(m);
	}
}

static void
refreshstatuslight(void)
{
	Monitor *m;
	int barh;
	double percent, display;

	if (!backlight_paths_initialized) {
		backlight_available = findbacklightdevice(backlight_brightness_path,
				sizeof(backlight_brightness_path),
				backlight_max_path, sizeof(backlight_max_path));
		if (!backlight_available)
			backlight_writable = 0;
		backlight_paths_initialized = 1;
	}

	percent = backlight_percent();
	display = percent;

	if (percent >= 0.0) {
		light_last_percent = percent;
		display = percent;
	} else if (light_last_percent >= 0.0) {
		display = light_last_percent;
	}

	if (display < 0.0) {
		snprintf(light_text, sizeof(light_text), "--%%");
	} else {
		if (display > 100.0)
			display = 100.0;
		if (display < 0.0)
			display = 0.0;
		snprintf(light_text, sizeof(light_text), "%d%%", (int)lround(display));
	}

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.light.tree || !m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.light, barh, light_text,
					last_light_render, sizeof(last_light_render), &last_light_h)) {
			renderlight(&m->statusbar.light, barh, light_text);
			positionstatusmodules(m);
		}
	}
}

static void
refreshstatusnet(void)
	{
		Monitor *m;
		int barh;
		char iface[IF_NAMESIZE] = {0};
		char path[PATH_MAX];
		time_t now_sec = time(NULL);
		unsigned long long rx = 0, tx = 0;
	struct timespec now_ts = {0};
	double elapsed = 0.0;
	int rx_ok = 0, tx_ok = 0;
	double wifi_quality = -1.0;
	const char *icon_path = net_icon_no_conn_resolved[0] ? net_icon_no_conn_resolved : net_icon_no_conn;
	int link_speed = -1;
	int popup_active = 0;

	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.net_popup.visible) {
			popup_active = 1;
			break;
		}
	}
	net_available = findactiveinterface(iface, sizeof(iface), &net_is_wireless);
	if (!net_available) {
		snprintf(net_text, sizeof(net_text), "Net: --");
		snprintf(net_local_ip, sizeof(net_local_ip), "--");
		snprintf(net_down_text, sizeof(net_down_text), "--");
		snprintf(net_up_text, sizeof(net_up_text), "--");
		snprintf(net_ssid, sizeof(net_ssid), "--");
		net_last_wifi_quality = -1.0;
		net_link_speed_mbps = -1;
		net_last_down_bps = net_last_up_bps = -1.0;
		net_prev_valid = 0;
		net_prev_iface[0] = '\0';
		stop_ssid_fetch();
		ssid_last_time = 0;
	} else {
		int need_ssid;

		snprintf(net_iface, sizeof(net_iface), "%s", iface);
		if (strncmp(net_prev_iface, net_iface, sizeof(net_prev_iface)) != 0) {
			net_prev_valid = 0;
			net_prev_rx = net_prev_tx = 0;
			snprintf(net_prev_iface, sizeof(net_prev_iface), "%s", net_iface);
			if (net_is_wireless) {
				stop_ssid_fetch();
				ssid_last_time = 0;
			}
		}

		if (net_is_wireless) {
			need_ssid = (!net_ssid[0] || strcmp(net_ssid, "--") == 0 ||
					(now_sec != (time_t)-1 &&
					 (now_sec - ssid_last_time > 60 || ssid_last_time == 0)));
			if ((popup_active || need_ssid) && now_sec != (time_t)-1 &&
					!ssid_event && ssid_pid <= 0) {
				request_ssid_async(net_iface);
			}
			if (!net_ssid[0])
				snprintf(net_ssid, sizeof(net_ssid), "WiFi");
			snprintf(net_text, sizeof(net_text), "WiFi: %s", net_ssid);
		} else {
			stop_ssid_fetch();
			ssid_last_time = 0;
			snprintf(net_text, sizeof(net_text), "Wired");
			snprintf(net_ssid, sizeof(net_ssid), "Ethernet");
		}

		if (!net_is_wireless) {
			icon_path = net_icon_eth_resolved[0] ? net_icon_eth_resolved : net_icon_eth;
			if (readlinkspeedmbps(net_iface, &link_speed) == 0)
				net_link_speed_mbps = link_speed;
			else
				net_link_speed_mbps = -1;
			net_last_wifi_quality = -1.0;
		} else {
			wifi_quality = wireless_signal_percent(net_iface);
			if (wifi_quality < 0.0)
				wifi_quality = 50.0;
			icon_path = wifi_icon_for_quality(wifi_quality);
			net_last_wifi_quality = wifi_quality;
			if (readlinkspeedmbps(net_iface, &link_speed) == 0)
				net_link_speed_mbps = link_speed;
			else
				net_link_speed_mbps = -1;
		}

		if (!localip(net_iface, net_local_ip, sizeof(net_local_ip)))
			snprintf(net_local_ip, sizeof(net_local_ip), "--");

		if (popup_active)
			request_public_ip_async();

		clock_gettime(CLOCK_MONOTONIC, &now_ts);

		if (popup_active) {
			if (snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/rx_bytes", net_iface)
					< (int)sizeof(path))
				rx_ok = (readulong(path, &rx) == 0);
			if (snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/tx_bytes", net_iface)
					< (int)sizeof(path))
				tx_ok = (readulong(path, &tx) == 0);

			if (net_prev_valid) {
				elapsed = (now_ts.tv_sec - net_prev_ts.tv_sec)
					+ (now_ts.tv_nsec - net_prev_ts.tv_nsec) / 1e9;
			}
			net_last_down_bps = (net_prev_valid && rx_ok) ? net_bytes_to_rate(rx, net_prev_rx, elapsed) : -1.0;
			net_last_up_bps = (net_prev_valid && tx_ok) ? net_bytes_to_rate(tx, net_prev_tx, elapsed) : -1.0;
			format_speed(net_last_down_bps, net_down_text, sizeof(net_down_text));
			format_speed(net_last_up_bps, net_up_text, sizeof(net_up_text));

			if (rx_ok && tx_ok) {
				net_prev_rx = rx;
				net_prev_tx = tx;
				net_prev_ts = now_ts;
				net_prev_valid = 1;
			}
		}
	}
	set_net_icon_path(icon_path);

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.net.tree || !m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.net, barh, net_text,
					last_net_render, sizeof(last_net_render), &last_net_h)
				|| m->statusbar.net_popup.visible) {
			rendernet(&m->statusbar.net, barh, net_text);
			if (m->statusbar.net_popup.visible)
				rendernetpopup(m);
			positionstatusmodules(m);
		} else if (m->statusbar.net_popup.visible) {
			rendernetpopup(m);
		}
	}
}

static void
tray_remove_item(const char *service)
{
	TrayItem *it, *tmp;
	Monitor *m;

	if (!service)
		return;

	wl_list_for_each(m, &mons, link) {
		if (strcmp(m->statusbar.tray_menu.service, service) == 0)
			tray_menu_hide(m);
	}

	wl_list_for_each_safe(it, tmp, &tray_items, link) {
		if (strcmp(it->service, service) == 0) {
			wl_list_remove(&it->link);
			if (it->icon_buf)
				wlr_buffer_drop(it->icon_buf);
			free(it);
			tray_update_icons_text();
			sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
					"org.kde.StatusNotifierWatcher",
					"StatusNotifierItemUnregistered", "s", service);
			sd_bus_emit_signal(tray_bus, NULL, "/StatusNotifierWatcher",
					"org.freedesktop.StatusNotifierWatcher",
					"StatusNotifierItemUnregistered", "s", service);
			return;
		}
	}
}

static __attribute__((unused)) TrayItem *
tray_first_item(void)
{
	TrayItem *sample = NULL;

	if (wl_list_empty(&tray_items))
		return NULL;
	return wl_container_of(tray_items.next, sample, link);
}

static __attribute__((unused)) void
tray_item_activate(TrayItem *it, int button, int context_menu, int x, int y)
{
	const char *method;
	const char *ifaces[] = {
		"org.kde.StatusNotifierItem",
		"org.freedesktop.StatusNotifierItem",
	};
	const char *item_path;
	int r = -1;

	if (!tray_bus || !it)
		return;
	if (context_menu)
		method = "ContextMenu";
	else if (button == BTN_MIDDLE)
		method = "SecondaryActivate";
	else
		method = "Activate";
	tray_anchor_x = x;
	tray_anchor_y = y;
	tray_anchor_time_ms = monotonic_msec();
	if (context_menu) {
		/* ensure transient menus are placed near the icon even if GetLayout fails */
		tray_anchor_x = x;
		tray_anchor_y = y;
	}
	item_path = it->path[0] ? it->path : "/StatusNotifierItem";
	for (int attempt = 0; attempt < 2; attempt++) {
		for (size_t i = 0; i < LENGTH(ifaces); i++) {
			r = sd_bus_call_method(tray_bus, it->service, item_path,
					ifaces[i], method, NULL, NULL, "ii", x, y);
			if (r >= 0)
				return;
		}
		/* If bus is broken, try to re-init once */
		if (r == -EBADFD || r == -EPIPE || r == -ENOTCONN) {
			tray_init();
			if (!tray_bus)
				break;
		} else {
			break;
		}
	}
	wlr_log(WLR_ERROR, "tray: %s %s failed on %s%s: %s",
			method, it->service, item_path, it->path[0] ? "" : "(null)", strerror(-r));
}

static void
refreshstatusbattery(void)
{
	Monitor *m;
	int barh;
	double percent, display;
	const char *icon = battery_icon_100;

	if (!battery_path_initialized) {
		battery_available = findbatterydevice(battery_capacity_path,
				sizeof(battery_capacity_path));
		battery_path_initialized = 1;
	}

	percent = battery_percent();
	display = percent;

	if (percent >= 0.0) {
		battery_last_percent = percent;
		display = percent;
	} else if (battery_last_percent >= 0.0) {
		display = battery_last_percent;
	}

	if (display < 0.0) {
		snprintf(battery_text, sizeof(battery_text), "--%%");
	} else {
		if (display > 100.0)
			display = 100.0;
		if (display < 0.0)
			display = 0.0;
		snprintf(battery_text, sizeof(battery_text), "%d%%", (int)lround(display));
	}

	if (display >= 0.0) {
		if (display <= 25.0)
			icon = battery_icon_25;
		else if (display <= 50.0)
			icon = battery_icon_50;
		else if (display <= 75.0)
			icon = battery_icon_75;
		else
			icon = battery_icon_100;
	}
	if (strncmp(battery_icon_path, icon, sizeof(battery_icon_path)) != 0) {
		snprintf(battery_icon_path, sizeof(battery_icon_path), "%s", icon);
	}

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.battery.tree || !m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.battery, barh, battery_text,
					last_battery_render, sizeof(last_battery_render), &last_battery_h)) {
			renderbattery(&m->statusbar.battery, barh, battery_text);
			positionstatusmodules(m);
		}
	}
}

static void
refreshstatuscpu(void)
{
	double usage = cpuaverage();
	Monitor *m;
	int barh;

	if (usage >= 0.0)
		cpu_last_percent = usage;

	if (cpu_last_percent < 0.0)
		snprintf(cpu_text, sizeof(cpu_text), "--%%");
	else {
		int avg_disp = (cpu_last_percent < 1.0) ? 0 : (int)lround(cpu_last_percent);
		snprintf(cpu_text, sizeof(cpu_text), "%d%%", avg_disp);
	}

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.cpu.tree || !m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.cpu, barh, cpu_text,
					last_cpu_render, sizeof(last_cpu_render), &last_cpu_h)
				|| m->statusbar.cpu_popup.visible) {
			rendercpu(&m->statusbar.cpu, barh, cpu_text);
			if (m->statusbar.cpu_popup.visible)
				rendercpupopup(m);
			positionstatusmodules(m);
		}
	}
}

static void
refreshstatusram(void)
{
	double used_mb = ramused_mb();
	Monitor *m;
	int barh;

	if (used_mb >= 0.0)
		ram_last_mb = used_mb;

	if (ram_last_mb < 0.0) {
		snprintf(ram_text, sizeof(ram_text), "--");
	} else if (ram_last_mb >= 1024.0) {
		double gb = ram_last_mb / 1024.0;
		snprintf(ram_text, sizeof(ram_text), "%.1fGB", gb);
	} else {
		snprintf(ram_text, sizeof(ram_text), "%dMB", (int)lround(ram_last_mb));
	}

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.ram.tree || !m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.ram, barh, ram_text,
					last_ram_render, sizeof(last_ram_render), &last_ram_h)) {
			renderram(&m->statusbar.ram, barh, ram_text);
			positionstatusmodules(m);
		}
	}
}

static void
refreshstatusvolume(void)
{
	int is_headset = pipewire_sink_is_headset();
	double vol = speaker_active;
	Monitor *m;
	int barh;
	double display = vol;
	int force_render = 0;
	int use_muted_color = 0;
	const char *icon = volume_icon_speaker_100;

	if (vol < 0.0) {
		double read = pipewire_volume_percent(&is_headset);
		if (read >= 0.0) {
			vol = read;
			speaker_active = read;
		}
	}
	display = vol;

	if (display > volume_max_percent)
		display = volume_max_percent;
	if (display < 0.0 && volume_muted == 1)
		display = 0.0;

	if (volume_muted == 1) {
		use_muted_color = 1;
		display = display < 0.0 ? 0.0 : display;
	}

	if (display < 0.0) {
		snprintf(volume_text, sizeof(volume_text), "--%%");
	} else {
		if (display < 0.0)
			display = 0.0;
		if (display > volume_max_percent)
			display = volume_max_percent;
		if (volume_muted == 1)
			display = 0.0;
		snprintf(volume_text, sizeof(volume_text), "%d%%", (int)lround(display));
	}

	if (is_headset) {
		if (volume_muted == 1)
			icon = volume_icon_headset_muted;
		else
			icon = volume_icon_headset;
	} else if (volume_muted == 1) {
		icon = volume_icon_speaker_muted;
	} else if (display <= 25.0) {
		icon = volume_icon_speaker_25;
	} else if (display <= 75.0) {
		icon = volume_icon_speaker_50;
	} else {
		icon = volume_icon_speaker_100;
	}

	if (strncmp(volume_icon_path, icon, sizeof(volume_icon_path)) != 0) {
		snprintf(volume_icon_path, sizeof(volume_icon_path), "%s", icon);
		force_render = 1;
	}

	volume_text_color = use_muted_color ? statusbar_volume_muted_fg : statusbar_fg;
	if (use_muted_color != volume_last_color_is_muted) {
		force_render = 1;
	}
	volume_last_color_is_muted = use_muted_color;

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.volume.tree || !m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.volume, barh, volume_text,
					last_volume_render, sizeof(last_volume_render), &last_volume_h)
				|| force_render) {
			rendervolume(&m->statusbar.volume, barh, volume_text);
			positionstatusmodules(m);
		}
	}
}

static void
refreshstatusmic(void)
{
	double vol = microphone_active;
	Monitor *m;
	int barh;
	double display = vol;
	int force_render = 0;
	int use_muted_color = 0;
	const char *icon = mic_icon_unmuted;

	if (vol < 0.0) {
		double read = pipewire_mic_volume_percent();
		if (read >= 0.0) {
			vol = read;
			microphone_active = read;
			mic_last_percent = read;
		}
	}
	if (vol >= 0.0)
		mic_last_percent = vol;
	display = vol >= 0.0 ? vol : mic_last_percent;

	if (display > mic_max_percent)
		display = mic_max_percent;
	if (display < 0.0 && mic_muted == 1)
		display = 0.0;

	if (mic_muted == 1) {
		use_muted_color = 1;
		display = display < 0.0 ? 0.0 : display;
	}

	if (display < 0.0) {
		snprintf(mic_text, sizeof(mic_text), "--%%");
	} else {
		if (display < 0.0)
			display = 0.0;
		if (display > mic_max_percent)
			display = mic_max_percent;
		if (mic_muted == 1)
			display = 0.0;
		snprintf(mic_text, sizeof(mic_text), "%d%%", (int)lround(display));
	}

	if (mic_muted == 1)
		icon = mic_icon_muted;
	else
		icon = mic_icon_unmuted;

	if (strncmp(mic_icon_path, icon, sizeof(mic_icon_path)) != 0) {
		snprintf(mic_icon_path, sizeof(mic_icon_path), "%s", icon);
		force_render = 1;
	}

	mic_text_color = use_muted_color ? statusbar_mic_muted_fg : statusbar_fg;
	if (use_muted_color != mic_last_color_is_muted) {
		force_render = 1;
		mic_last_color_is_muted = use_muted_color;
	}

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.mic.tree || !m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.mic, barh, mic_text,
					last_mic_render, sizeof(last_mic_render), &last_mic_h)
				|| force_render) {
			rendermic(&m->statusbar.mic, barh, mic_text);
			positionstatusmodules(m);
		}
	}
}

static void
refreshstatusicons(void)
{
	Monitor *m;
	int barh;

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.sysicons.tree || !m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		rendertrayicons(m, barh);
		positionstatusmodules(m);
	}
}

static void
tray_update_icons_text(void)
{
	TrayItem *it;
	size_t off = 0;
	int running_width;
	char segment[256];

	snprintf(sysicons_text, sizeof(sysicons_text), "Tray");
	off = strlen(sysicons_text);
	if (off >= sizeof(sysicons_text))
		off = sizeof(sysicons_text) - 1;
	running_width = status_text_width(sysicons_text);

	wl_list_for_each(it, &tray_items, link) {
		const char *label = it->label[0] ? it->label : it->service;
		int n;
		it->x = running_width;
		if (off ? snprintf(segment, sizeof(segment), " %s", label)
				: snprintf(segment, sizeof(segment), "%s", label))
			it->w = status_text_width(segment);
		else
			it->w = status_text_width(label);
		if (it->w <= 0)
			it->w = statusbar_font_spacing;
		n = snprintf(sysicons_text + off, sizeof(sysicons_text) - off,
				off ? " %s" : "%s", label);
		if (n < 0)
			break;
		if ((size_t)n >= sizeof(sysicons_text) - off) {
			sysicons_text[sizeof(sysicons_text) - 1] = '\0';
			break;
		}
		off += (size_t)n;
		running_width += it->w;
	}

	refreshstatusicons();
}

static void
refreshstatustags(void)
{
	Monitor *m;
	int barh;

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.tags.tree || !m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		renderworkspaces(m, &m->statusbar.tags, barh);
		if (m->statusbar.traylabel.tree) {
			clearstatusmodule(&m->statusbar.traylabel);
			m->statusbar.traylabel.width = 0;
			wlr_scene_node_set_enabled(&m->statusbar.traylabel.tree->node, 0);
		}
		m->statusbar.tags.hover_tag = -1;
		for (int i = 0; i < TAGCOUNT; i++)
			m->statusbar.tags.hover_alpha[i] = 0.0f;
		positionstatusmodules(m);
	}
}

static void
seed_status_rng(void)
{
	struct timespec ts;

	if (status_rng_seeded)
		return;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		srand((unsigned)(ts.tv_sec ^ ts.tv_nsec));
	else
		srand((unsigned)time(NULL));
	status_rng_seeded = 1;
}

static int
status_should_render(StatusModule *module, int barh, const char *text,
		char *last_text, size_t last_len, int *last_h)
{
	if (!module || !text || !last_text || last_len == 0)
		return 1;

	if ((last_h && *last_h != barh) || last_text[0] == '\0'
			|| strncmp(last_text, text, last_len) != 0) {
		snprintf(last_text, last_len, "%s", text);
		if (last_h)
			*last_h = barh;
		return 1;
	}
	return 0;
}

static void
initial_status_refresh(void)
{
	apply_startup_defaults();
	refreshstatusclock();
	refreshstatuscpu();
	refreshstatusram();
	refreshstatuslight();
	refreshstatusmic();
	refreshstatusvolume();
	refreshstatusbattery();
	refreshstatusnet();
	refreshstatusicons();
	refreshstatustags();
}

static uint32_t
random_status_delay_ms(void)
{
	seed_status_rng();
	return 5000u + (uint32_t)(rand() % 5001);
}

static void
init_status_refresh_tasks(void)
{
	uint64_t now = monotonic_msec();
	uint32_t offset = 100;

	for (size_t i = 0; i < LENGTH(status_tasks); i++) {
		status_tasks[i].next_due_ms = now + offset;
		offset += 200; /* stagger initial fills to avoid clumping */
	}
}

static void
trigger_status_task_now(void (*fn)(void))
{
	uint64_t now = monotonic_msec();

	for (size_t i = 0; i < LENGTH(status_tasks); i++) {
		if (status_tasks[i].fn == fn) {
			status_tasks[i].next_due_ms = now;
			schedule_next_status_refresh();
			return;
		}
	}
}

static void
set_status_task_due(void (*fn)(void), uint64_t due_ms)
{
	for (size_t i = 0; i < LENGTH(status_tasks); i++) {
		if (status_tasks[i].fn == fn) {
			status_tasks[i].next_due_ms = due_ms;
			schedule_next_status_refresh();
			return;
		}
	}
}

static int
status_task_hover_active(void (*fn)(void))
{
	Monitor *m;

	if (fn == refreshstatuscpu) {
		return 0;
	}
	if (fn == refreshstatusnet) {
		wl_list_for_each(m, &mons, link) {
			if (m->showbar && m->statusbar.net_popup.visible)
				return 1;
		}
	}
	return 0;
}

static Monitor *
modal_visible_monitor(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (m->modal.visible)
			return m;
	}
	return NULL;
}

static void
modal_layout_metrics(int *btn_h, int *field_h, int *line_h, int *pad)
{
	int bh = statusfont.height > 0 ? statusfont.height + 16 : 48;
	int fh;
	int lh;
	int pd = 12;

	if (bh < 40)
		bh = 40;
	fh = bh > 40 ? bh - 8 : 36;
	lh = statusfont.height > 0 ? statusfont.height + 6 : 22;

	if (btn_h)
		*btn_h = bh;
	if (field_h)
		*field_h = fh;
	if (line_h)
		*line_h = lh;
	if (pad)
		*pad = pd;
}

static int
modal_max_visible_lines(Monitor *m)
{
	ModalOverlay *mo;
	int btn_h, field_h, line_h, pad;
	int line_y, avail;

	if (!m)
		return 0;
	mo = &m->modal;
	modal_layout_metrics(&btn_h, &field_h, &line_h, &pad);

	line_y = btn_h + pad + field_h + pad;
	avail = mo->height - line_y - pad;
	if (avail <= 0 || line_h <= 0)
		return 0;
	return avail / line_h;
}

static void
modal_ensure_selection_visible(Monitor *m)
{
	ModalOverlay *mo;
	int active;
	int max_lines;
	int max_scroll;
	int sel;

	if (!m)
		return;
	mo = &m->modal;
	active = mo->active_idx;
	if (active < 0 || active > 2)
		return;

	max_lines = modal_max_visible_lines(m);
	if (max_lines <= 0) {
		mo->scroll[active] = 0;
		return;
	}
	if (mo->result_count[active] <= max_lines) {
		mo->scroll[active] = 0;
		return;
	}

	max_scroll = mo->result_count[active] - max_lines;
	sel = mo->selected[active];

	if (sel < 0) {
		if (mo->scroll[active] > max_scroll)
			mo->scroll[active] = max_scroll;
		if (mo->scroll[active] < 0)
			mo->scroll[active] = 0;
		return;
	}

	if (sel < mo->scroll[active])
		mo->scroll[active] = sel;
	else if (sel >= mo->scroll[active] + max_lines)
		mo->scroll[active] = sel - max_lines + 1;

	if (mo->scroll[active] > max_scroll)
		mo->scroll[active] = max_scroll;
	if (mo->scroll[active] < 0)
		mo->scroll[active] = 0;
}

static void
modal_file_search_clear_results(Monitor *m)
{
	ModalOverlay *mo;

	if (!m)
		return;
	mo = &m->modal;
		mo->result_count[1] = 0;
		for (int i = 0; i < (int)LENGTH(mo->results[1]); i++) {
			mo->results[1][i][0] = '\0';
			mo->result_entry_idx[1][i] = -1;
			mo->file_results_name[i][0] = '\0';
			mo->file_results_path[i][0] = '\0';
			mo->file_results_mtime[i] = 0;
	}
	mo->selected[1] = -1;
	mo->scroll[1] = 0;
	mo->file_search_fallback = 0;
}

static int
ci_contains(const char *hay, const char *needle)
{
	size_t hlen, nlen;

	if (!needle || !*needle)
		return 1;
	if (!hay || !*hay)
		return 0;
	hlen = strlen(hay);
	nlen = strlen(needle);
	if (nlen > hlen)
		return 0;
	for (size_t i = 0; i + nlen <= hlen; i++) {
		size_t j = 0;
		for (; j < nlen; j++) {
			char a = (char)tolower((unsigned char)hay[i + j]);
			char b = (char)tolower((unsigned char)needle[j]);
			if (a != b)
				break;
		}
		if (j == nlen)
			return 1;
	}
	return 0;
}

static int
modal_handle_key(Monitor *m, uint32_t mods, xkb_keysym_t sym)
{
	ModalOverlay *mo;

	if (!m)
		return 0;
	mo = &m->modal;

	if (sym == XKB_KEY_Escape) {
		modal_hide_all();
		return 1;
	}

	if (sym == XKB_KEY_BackSpace && mo->search_len[mo->active_idx] > 0 && mods == 0) {
		mo->search_len[mo->active_idx]--;
		mo->search[mo->active_idx][mo->search_len[mo->active_idx]] = '\0';
		return 1;
	}
	if (sym == XKB_KEY_Down && mo->result_count[mo->active_idx] > 0) {
		int sel = mo->selected[mo->active_idx];
		if (sel < 0)
			sel = 0;
		else if (sel + 1 < mo->result_count[mo->active_idx])
			sel++;
		else
			sel = 0; /* wrap to top */
		mo->selected[mo->active_idx] = sel;
		modal_ensure_selection_visible(m);
		return 1;
	}
	if (sym == XKB_KEY_Up && mo->result_count[mo->active_idx] > 0) {
		int sel = mo->selected[mo->active_idx];
		if (sel < 0)
			sel = mo->result_count[mo->active_idx] - 1;
		else if (sel > 0)
			sel--;
		else
			sel = mo->result_count[mo->active_idx] - 1; /* wrap to bottom */
		mo->selected[mo->active_idx] = sel;
		modal_ensure_selection_visible(m);
		return 1;
	}
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		int sel = mo->selected[mo->active_idx];
		if (mo->active_idx == 0 && sel >= 0 && sel < mo->result_count[0]) {
			int idx = mo->result_entry_idx[0][sel];
			if (idx >= 0 && idx < desktop_entry_count) {
				char cmd_str[512];
				char *cmd[4] = { "sh", "-c", cmd_str, NULL };
				Arg arg;
				desktop_entries[idx].used++;
				snprintf(cmd_str, sizeof(cmd_str), "%s", desktop_entries[idx].exec[0]
						? desktop_entries[idx].exec : desktop_entries[idx].name);
				arg.v = cmd;
				spawn(&arg);
				modal_hide_all();
			}
		} else if (mo->active_idx == 1 && sel >= 0 && sel < mo->result_count[1]) {
			const char *path = mo->file_results_path[sel];
			if (path && *path) {
				char *cmd[] = { "xdg-open", (char *)path, NULL };
				Arg arg = { .v = cmd };
				spawn(&arg);
				modal_hide_all();
			}
		}
		return 1;
	}
	if (sym >= 0x20 && sym <= 0x7e &&
			!(mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_LOGO))) {
		int len = mo->search_len[mo->active_idx];
		if (len + 1 < (int)sizeof(mo->search[0])) {
			mo->search[mo->active_idx][len] = (char)sym;
			mo->search[mo->active_idx][len + 1] = '\0';
			mo->search_len[mo->active_idx] = len + 1;
		}
		return 1;
	}

	return 0;
}

static void
modal_file_search_stop(Monitor *m)
{
	ModalOverlay *mo;

	if (!m)
		return;
	mo = &m->modal;

	if (mo->file_search_timer)
		wl_event_source_timer_update(mo->file_search_timer, 0);
	if (mo->file_search_event) {
		wl_event_source_remove(mo->file_search_event);
		mo->file_search_event = NULL;
	}
	if (mo->file_search_fd >= 0) {
		close(mo->file_search_fd);
		mo->file_search_fd = -1;
	}
	if (mo->file_search_pid > 0) {
		wlr_log(WLR_INFO, "modal file search stop: killing pid=%d", mo->file_search_pid);
		kill(mo->file_search_pid, SIGTERM);
		waitpid(mo->file_search_pid, NULL, WNOHANG);
		mo->file_search_pid = -1;
	}
	mo->file_search_len = 0;
	mo->file_search_buf[0] = '\0';
	if (mo->file_search_timer) {
		wl_event_source_remove(mo->file_search_timer);
		mo->file_search_timer = NULL;
	}
}

static void
shorten_path_display(const char *full, char *out, size_t len)
{
	const char *home = getenv("HOME");
	const char *p = full;
	size_t flen;
	size_t keep;
	const char *tail;

	if (!out || len == 0)
		return;
	out[0] = '\0';
	if (!p)
		return;

	if (home && *home && strncmp(full, home, strlen(home)) == 0 && strlen(home) < strlen(full)) {
		static char tmp[PATH_MAX];
		size_t hlen = strlen(home);
		size_t rem = strlen(full) - hlen;
		if (hlen + 1 + rem < sizeof(tmp)) {
			tmp[0] = '~';
			memcpy(tmp + 1, full + hlen, rem + 1);
			p = tmp;
		}
	}

	flen = strlen(p);
	if (flen < len) {
		snprintf(out, len, "%s", p);
		return;
	}
	if (len < 5) {
		snprintf(out, len, "%s", p + (flen - (len - 1)));
		return;
	}

	keep = (len - 4) / 2;
	tail = p + flen - (len - keep - 4);
	if (tail < p)
		tail = p;
	snprintf(out, len, "%.*s...%s", (int)keep, p, tail);
}

static void
modal_file_search_add_line(Monitor *m, const char *line)
{
	ModalOverlay *mo;
	int idx;
	const char *tab1;
	const char *tab2;
	size_t name_len, path_len;
	char name[128];
	char path[PATH_MAX];
	char dir[PATH_MAX];
	char when[32] = {0};
	char path_disp[128] = {0};
	double epoch = 0.0;
	time_t t;
	struct tm tm;
	char needle[256];

	if (!m || !line || !*line)
		return;
	mo = &m->modal;
	if (mo->result_count[1] >= (int)LENGTH(mo->results[1]))
		return;

	tab1 = strchr(line, '\t');
	if (!tab1)
		return;
	tab2 = strchr(tab1 + 1, '\t');
	if (!tab2)
		return;

	name_len = (size_t)(tab1 - line);
	path_len = (size_t)(tab2 - (tab1 + 1));
	if (name_len == 0 || path_len == 0)
		return;
	if (name_len >= sizeof(name))
		name_len = sizeof(name) - 1;
	if (path_len >= sizeof(path))
		path_len = sizeof(path) - 1;

	snprintf(name, sizeof(name), "%.*s", (int)name_len, line);
	snprintf(path, sizeof(path), "%.*s", (int)path_len, tab1 + 1);
	snprintf(dir, sizeof(dir), "%s", path);
	{
		char *slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = '\0';
		} else if (slash) {
			*(slash + 1) = '\0';
		} else {
			snprintf(dir, sizeof(dir), ".");
		}
	}

	epoch = strtod(tab2 + 1, NULL);
	t = (time_t)epoch;
	if (localtime_r(&t, &tm))
		strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
	shorten_path_display(dir, path_disp, sizeof(path_disp));

	/* Always filter locally to allow any substring/character combination to match */
	const char *q = mo->file_search_last;
	int qlen = q ? (int)strlen(q) : 0;
	int qstart = 0, qend = qlen;
	if (qlen >= (int)sizeof(needle))
		qlen = (int)sizeof(needle) - 1;
	while (qstart < qlen && isspace((unsigned char)q[qstart]))
		qstart++;
	while (qend > qstart && isspace((unsigned char)q[qend - 1]))
		qend--;
	qlen = qend - qstart;
	if (qlen <= 0)
		return;
	for (int i = 0; i < qlen; i++)
		needle[i] = (char)q[qstart + i];
	needle[qlen] = '\0';
	if (!ci_contains(name, needle) && !ci_contains(path, needle))
		return;

	wlr_log(WLR_INFO, "modal file search result: name='%s' dir='%s'", name, path_disp[0] ? path_disp : dir);

	idx = mo->result_count[1];
	snprintf(mo->results[1][idx], sizeof(mo->results[1][idx]),
			"%s  %s", name, path_disp[0] ? path_disp : dir);
	snprintf(mo->file_results_name[idx], sizeof(mo->file_results_name[idx]), "%s", name);
	snprintf(mo->file_results_path[idx], sizeof(mo->file_results_path[idx]), "%s", path);
	mo->file_results_mtime[idx] = t;
	mo->result_entry_idx[1][idx] = -1;
	mo->result_count[1]++;
	if (mo->selected[1] < 0)
		mo->selected[1] = idx;
	if (mo->result_count[1] >= (int)LENGTH(mo->results[1]))
		modal_file_search_stop(m);
}

static void
modal_file_search_flush_buffer(Monitor *m)
{
	ModalOverlay *mo;
	size_t start = 0;

	if (!m)
		return;
	mo = &m->modal;
	for (size_t i = 0; i < mo->file_search_len; i++) {
		if (mo->file_search_buf[i] == '\n') {
			mo->file_search_buf[i] = '\0';
			modal_file_search_add_line(m, mo->file_search_buf + start);
			if (mo->result_count[1] >= (int)LENGTH(mo->results[1])) {
				modal_file_search_stop(m);
				start = mo->file_search_len;
				break;
			}
			start = i + 1;
		}
	}

	if (start > 0) {
		size_t remaining = mo->file_search_len - start;
		memmove(mo->file_search_buf, mo->file_search_buf + start, remaining);
		mo->file_search_len = remaining;
		mo->file_search_buf[mo->file_search_len] = '\0';
	} else if (mo->file_search_len >= sizeof(mo->file_search_buf) - 1) {
		/* avoid getting stuck with an overfull buffer if no newline arrives */
		mo->file_search_len = 0;
		mo->file_search_buf[0] = '\0';
	}
}

static void
modal_file_search_start_mode(Monitor *m, int fallback)
{
	ModalOverlay *mo;
	const char *home_env;
	char home[PATH_MAX];
	const char *roots[128]; /* search roots; deduped */
	char root_bufs[128][PATH_MAX];
	int root_count = 0;
	char pattern[MODAL_RESULT_LEN];
	int pipefd[2] = {-1, -1};
	struct stat st = {0};
	pid_t pid;
	int nlen;

	if (!m)
		return;
	mo = &m->modal;

	modal_file_search_stop(m);
	modal_file_search_clear_results(m);

	if (mo->search_len[1] < modal_file_search_minlen) {
		mo->file_search_last[0] = '\0';
		return;
	}
	mo->file_search_fallback = 1; /* always filter locally */

	/* helper to add unique, existing directories */
#define ADD_ROOT(PATHSTR) \
	do { \
		const char *_path = (PATHSTR); \
		int _i; \
		int idx; \
		if ((int)LENGTH(roots) > root_count && _path && *_path && stat(_path, &st) == 0 && S_ISDIR(st.st_mode)) { \
			int exists = 0; \
			for (_i = 0; _i < root_count; _i++) { \
				if (strcmp(roots[_i], _path) == 0) { exists = 1; break; } \
			} \
			if (!exists) { \
				idx = root_count; \
				snprintf(root_bufs[idx], sizeof(root_bufs[0]), "%s", _path); \
				roots[idx] = root_bufs[idx]; \
				root_count++; \
			} \
		} \
	} while (0)

	home_env = getenv("HOME");
	if (home_env && *home_env &&
			snprintf(home, sizeof(home), "%s", home_env) < (int)sizeof(home))
		ADD_ROOT(home);
	else {
		struct passwd *pw = getpwuid(getuid());
		if (pw && pw->pw_dir && *pw->pw_dir &&
				snprintf(home, sizeof(home), "%s", pw->pw_dir) < (int)sizeof(home))
			ADD_ROOT(home);
	}
	/* add mounted nfs/smb exports from mountinfo */
	{
		FILE *fp = fopen("/proc/self/mountinfo", "r");
		char line[1024];
		int i;
		char *p;
		char *sep;
		char *fs;
		if (fp) {
			while (fgets(line, sizeof(line), fp)) {
				char mnt[PATH_MAX] = {0};
				char fstype[64] = {0};
				p = strchr(line, ' ');
				for (i = 0; p && i < 3; i++)
					p = strchr(p + 1, ' ');
				if (p) {
					p++;
					sep = strchr(p, ' ');
					if (sep && (size_t)(sep - p) < sizeof(mnt)) {
						memcpy(mnt, p, (size_t)(sep - p));
						mnt[sep - p] = '\0';
						p = strchr(sep + 1, ' ');
						if (p) {
							p++;
							fs = strchr(p, ' ');
							if (fs && (size_t)(fs - p) < sizeof(fstype)) {
								memcpy(fstype, p, (size_t)(fs - p));
								fstype[fs - p] = '\0';
							}
						}
					}
				}
				if (fstype[0]) {
					if ((strncmp(fstype, "nfs", 3) == 0 ||
							strncmp(fstype, "cifs", 4) == 0 ||
							strncmp(fstype, "smb", 3) == 0) && mnt[0])
						ADD_ROOT(mnt);
				}
			}
			fclose(fp);
		}
	}
#ifdef __linux__
	/* fallback to /proc/mounts if mountinfo unavailable */
	if (root_count == 0) {
		FILE *fp = fopen("/proc/mounts", "r");
		char line[1024];
		if (fp) {
			while (fgets(line, sizeof(line), fp)) {
				char src[256], mnt[PATH_MAX], fstype[64];
				src[0] = mnt[0] = fstype[0] = '\0';
				if (sscanf(line, "%255s %1023s %63s", src, mnt, fstype) == 3) {
					if ((strncmp(fstype, "nfs", 3) == 0 ||
							strncmp(fstype, "cifs", 4) == 0 ||
							strncmp(fstype, "smb", 3) == 0) && mnt[0])
						ADD_ROOT(mnt);
				}
			}
			fclose(fp);
		}
	}
#endif

		/* common extra roots on NixOS/desktops if they exist */
		ADD_ROOT("/run/media");
		ADD_ROOT("/run/mounts");
		ADD_ROOT("/mnt");
		ADD_ROOT("/media");

	if (root_count == 0)
		ADD_ROOT("/");
#undef ADD_ROOT

	nlen = mo->search_len[1];
	/* Trim whitespace, then build find pattern ("*<escaped query>*" or "*" in fallback) */
	if (nlen > (int)sizeof(pattern) - 1)
		nlen = (int)sizeof(pattern) - 1;
	char query[256];
	if (nlen > (int)sizeof(query) - 1)
		nlen = (int)sizeof(query) - 1;
	memcpy(query, mo->search[1], (size_t)nlen);
	query[nlen] = '\0';
	int start = 0, end = nlen;
	while (start < nlen && isspace((unsigned char)query[start]))
		start++;
	while (end > start && isspace((unsigned char)query[end - 1]))
		end--;
	nlen = end - start;
	if (nlen <= 0) {
		mo->file_search_last[0] = '\0';
		return;
	}
	{
		const char *q = query + start;
		(void)fallback;
		(void)q;
		snprintf(pattern, sizeof(pattern), "*");
	}

	if (pipe(pipefd) != 0) {
		wlr_log(WLR_ERROR, "modal file search: pipe failed: %s", strerror(errno));
		mo->file_search_last[0] = '\0';
		return;
	}
	/* keep read FD out of launched apps and avoid blocking reads */
	fcntl(pipefd[0], F_SETFD, fcntl(pipefd[0], F_GETFD) | FD_CLOEXEC);
	fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);

	pid = fork();
	if (pid == 0) {
		char *argv[256];
		int argc = 0;

		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[1]);
		setsid();

		argv[argc++] = "find";
		for (int i = 0; i < root_count && argc < (int)LENGTH(argv) - 20; i++) {
			argv[argc++] = (char *)roots[i];
		}
		argv[argc++] = "-path";
		argv[argc++] = "*/.*";
		argv[argc++] = "-prune";
		argv[argc++] = "-o";
		argv[argc++] = "-type";
		argv[argc++] = "f";
		argv[argc++] = "-iname";
		argv[argc++] = pattern;
		argv[argc++] = "-printf";
		argv[argc++] = "%f\t%p\t%T@\\n";
		argv[argc] = NULL;
		execvp("find", argv);
		_exit(127);
	} else if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		wlr_log(WLR_ERROR, "modal file search: fork failed: %s", strerror(errno));
		mo->file_search_last[0] = '\0';
		return;
	}

	close(pipefd[1]);
	mo->file_search_pid = pid;
	mo->file_search_fd = pipefd[0];
	fcntl(mo->file_search_fd, F_SETFL, fcntl(mo->file_search_fd, F_GETFL) | O_NONBLOCK);
	mo->file_search_len = 0;
	mo->file_search_buf[0] = '\0';
	mo->file_search_event = wl_event_loop_add_fd(event_loop, mo->file_search_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP, modal_file_search_event, m);
	if (!mo->file_search_event) {
		wlr_log(WLR_ERROR, "modal file search: failed to watch results fd");
		modal_file_search_stop(m);
		mo->file_search_last[0] = '\0';
		return;
	}

	snprintf(mo->file_search_last, sizeof(mo->file_search_last), "%s", mo->search[1]);
	wlr_log(WLR_INFO, "modal file search start: '%s' (len=%d, roots=%d, fd=%d pid=%d event=%p fallback=%d)",
			mo->file_search_last, mo->search_len[1], root_count, mo->file_search_fd, mo->file_search_pid, (void *)mo->file_search_event, mo->file_search_fallback);
}

static void
modal_file_search_start(Monitor *m)
{
	modal_file_search_start_mode(m, 1);
}

static int
modal_file_search_event(int fd, uint32_t mask, void *data)
{
	Monitor *m = data;
	ModalOverlay *mo;
	char buf[512];
	ssize_t n;

	if (!m)
		return 0;
	(void)mask;
	mo = &m->modal;
	if (fd != mo->file_search_fd)
		return 0;

	wlr_log(WLR_INFO, "modal file search event: mask=0x%x pid=%d fd=%d", mask, mo->file_search_pid, fd);

	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			wlr_log(WLR_INFO, "modal file search read %zd bytes (buf_len=%zu)", n, mo->file_search_len);
			if (mo->file_search_len >= sizeof(mo->file_search_buf) - 1)
				modal_file_search_flush_buffer(m);
			if (mo->file_search_len < sizeof(mo->file_search_buf) - 1) {
				size_t avail = sizeof(mo->file_search_buf) - 1 - mo->file_search_len;
				if ((size_t)n > avail)
					n = (ssize_t)avail;
				memcpy(mo->file_search_buf + mo->file_search_len, buf, (size_t)n);
				mo->file_search_len += (size_t)n;
				mo->file_search_buf[mo->file_search_len] = '\0';
			}
			modal_file_search_flush_buffer(m);
			if (mo->file_search_fd < 0 || mo->file_search_pid <= 0)
				break;
		} else if (n == 0) {
			modal_file_search_flush_buffer(m);
			if (mo->file_search_len > 0 && mo->file_search_len < sizeof(mo->file_search_buf)) {
				mo->file_search_buf[mo->file_search_len] = '\0';
				modal_file_search_add_line(m, mo->file_search_buf);
				mo->file_search_len = 0;
				mo->file_search_buf[0] = '\0';
			}
			wlr_log(WLR_INFO, "modal file search done: '%s' (%d results)", mo->file_search_last, mo->result_count[1]);
			modal_file_search_stop(m);
			break;
		} else {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			modal_file_search_stop(m);
			wlr_log(WLR_ERROR, "modal file search read error: %s", strerror(errno));
			break;
		}
	}

	if (mo->result_count[1] > 0 && mo->selected[1] < 0)
		mo->selected[1] = 0;
	if (mo->active_idx == 1 && mo->result_count[1] > 0)
		modal_ensure_selection_visible(m);
	if (m->modal.visible)
		modal_render(m);
	return 0;
}

static void
modal_truncate_to_width(const char *src, char *dst, size_t len, int max_px)
{
	size_t n;

	if (!dst || len == 0) {
		return;
	}
	dst[0] = '\0';
	if (!src || !*src || max_px <= 0) {
		return;
	}

	snprintf(dst, len, "%s", src);
	if (status_text_width(dst) <= max_px)
		return;

	n = strlen(dst);
	if (len < 4) {
		dst[0] = '\0';
		return;
	}

	while (n > 0 && status_text_width(dst) > max_px) {
		n--;
		if (n < 3) {
			dst[0] = '\0';
			return;
		}
		dst[n] = '\0';
	}

	if (strlen(dst) + 3 < len) {
		strncat(dst, "...", len - strlen(dst) - 1);
	}
}

static int
desktop_entry_cmp_used(const void *a, const void *b)
{
	int ia = *(const int *)a;
	int ib = *(const int *)b;
	int ua = desktop_entries[ia].used;
	int ub = desktop_entries[ib].used;
	int namecmp;

	if (ua != ub)
		return (ua < ub) ? 1 : -1; /* more used first */

	namecmp = strcasecmp(desktop_entries[ia].name, desktop_entries[ib].name);
	if (namecmp != 0)
		return namecmp;

	return ia - ib;
}

static int
modal_match_name(const char *haystack, const char *needle)
{
	if (!needle || !*needle)
		return 1;
	if (!haystack || !*haystack)
		return 0;

	return strstr(haystack, needle) != NULL;
}

static void
strip_trailing_space(char *s)
{
	size_t len;

	if (!s)
		return;
	len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
				s[len - 1] == ' ' || s[len - 1] == '\t')) {
		s[len - 1] = '\0';
		len--;
	}
}

static int
desktop_entry_exists(const char *name)
{
	if (!name || !*name)
		return 0;
	for (int i = 0; i < desktop_entry_count; i++) {
		if (!strcasecmp(desktop_entries[i].name, name))
			return 1;
	}
	return 0;
}

static int
appdir_exists(char appdirs[][PATH_MAX], int count, const char *path)
{
	for (int i = 0; i < count; i++) {
		if (!strcmp(appdirs[i], path))
			return 1;
	}
	return 0;
}

static void
load_desktop_dir_rec(const char *dir, int depth)
{
	struct dirent *ent;
	DIR *d;

	if (!dir || !*dir || depth > 5 || desktop_entry_count >= (int)LENGTH(desktop_entries))
		return;

	d = opendir(dir);
	if (!d)
		return;

	while ((ent = readdir(d))) {
		char path[PATH_MAX];
		FILE *fp;
		char line[512];
		char name[256] = {0};
		char exec[512] = {0};
		int nodisplay = 0;
		int hidden = 0;
		int isdir = 0;
		int in_main_section = 0;

		if (desktop_entry_count >= (int)LENGTH(desktop_entries))
			break;

		if (ent->d_type == DT_DIR) {
			isdir = 1;
		} else if (ent->d_type == DT_UNKNOWN || ent->d_type == DT_LNK) {
			struct stat st = {0};
			if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int)sizeof(path))
				continue;
			if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
				isdir = 1;
		}

		if (isdir) {
			if (ent->d_name[0] == '.' || depth >= 5)
				continue;
			if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int)sizeof(path))
				continue;
			load_desktop_dir_rec(path, depth + 1);
			continue;
		}

		if (!strstr(ent->d_name, ".desktop"))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int)sizeof(path))
			continue;
		fp = fopen(path, "r");
		if (!fp)
			continue;

		while (fgets(line, sizeof(line), fp)) {
			if (line[0] == '[') {
				if (!strncmp(line, "[Desktop Entry]", 15)) {
					in_main_section = 1;
					continue;
				}
				if (in_main_section)
					break; /* stop at first other section */
				continue;
			}

			if (!in_main_section)
				continue;

			if (!strncmp(line, "Name=", 5)) {
				snprintf(name, sizeof(name), "%s", line + 5);
			} else if (!strncmp(line, "Name[", 5) && !name[0]) {
				char *eq = strchr(line, '=');
				if (eq && eq[1])
					snprintf(name, sizeof(name), "%s", eq + 1);
			} else if (!strncmp(line, "Exec=", 5)) {
				snprintf(exec, sizeof(exec), "%s", line + 5);
			} else if (!strncmp(line, "NoDisplay=", 10)) {
				const char *val = line + 10;
				if (!strncasecmp(val, "true", 4))
					nodisplay = 1;
			} else if (!strncmp(line, "Hidden=", 7)) {
				const char *val = line + 7;
				if (!strncasecmp(val, "true", 4))
					hidden = 1;
			}
		}
		fclose(fp);

		strip_trailing_space(name);
		strip_trailing_space(exec);

		for (size_t k = 0; exec[k]; k++) {
			if (exec[k] == '%') {
				exec[k] = '\0';
				break;
			}
		}
		strip_trailing_space(exec);

		if (nodisplay || hidden || !name[0] || !exec[0])
			continue;
		if (desktop_entry_exists(name))
			continue;

		snprintf(desktop_entries[desktop_entry_count].name,
				sizeof(desktop_entries[desktop_entry_count].name),
				"%s", name);
		snprintf(desktop_entries[desktop_entry_count].exec,
				sizeof(desktop_entries[desktop_entry_count].exec),
				"%s", exec);
		memset(desktop_entries[desktop_entry_count].name_lower, 0,
				sizeof(desktop_entries[desktop_entry_count].name_lower));
		for (size_t j = 0; j < strlen(desktop_entries[desktop_entry_count].name) &&
				j < sizeof(desktop_entries[desktop_entry_count].name_lower) - 1; j++) {
			desktop_entries[desktop_entry_count].name_lower[j] =
				(char)tolower((unsigned char)desktop_entries[desktop_entry_count].name[j]);
		}
		desktop_entry_count++;
	}
	closedir(d);
}

static void
load_desktop_dir(const char *dir)
{
	load_desktop_dir_rec(dir, 0);
}

static void
ensure_desktop_entries_loaded(void)
{
	char appdirs[128][PATH_MAX];
	int appdir_count = 0;
	const char *home = getenv("HOME");
	const char *user = getenv("USER");
	const char *xdg_data_home = getenv("XDG_DATA_HOME");
	const char *defaults[] = {
		"/usr/share/applications",
		"/usr/local/share/applications",
		"/var/lib/flatpak/exports/share/applications",
		"/opt/share/applications",
		"/run/current-system/sw/share/applications",
		"/nix/var/nix/profiles/system/sw/share/applications",
		"/nix/profile/share/applications",
		"/nix/var/nix/profiles/default/share/applications",
		NULL
	};
	char buf[PATH_MAX];
	char xdg_buf[4096] = {0};
	char *saveptr = NULL;
	const char *xdg_data_dirs;

	if (desktop_entries_loaded)
		return;

	memset(appdirs, 0, sizeof(appdirs));

	for (size_t i = 0; defaults[i]; i++) {
		if (appdir_count >= (int)LENGTH(appdirs))
			break;
		if (appdir_exists(appdirs, appdir_count, defaults[i]))
			continue;
		snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", defaults[i]);
	}

	if (xdg_data_home && *xdg_data_home) {
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/applications", xdg_data_home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
	}

	if (home) {
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.local/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.local/share/flatpak/exports/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.nix-profile/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.local/state/nix/profiles/profile/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.local/state/nix/profiles/default/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "%s/.local/state/nix/profiles/home-manager/share/applications", home) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
	}

	if (user && appdir_count < (int)LENGTH(appdirs)) {
		if (snprintf(buf, sizeof(buf), "/etc/profiles/per-user/%s/share/applications", user) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "/nix/var/nix/profiles/per-user/%s/profile/share/applications", user) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "/nix/var/nix/profiles/per-user/%s/home-manager/share/applications", user) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count < (int)LENGTH(appdirs) &&
				snprintf(buf, sizeof(buf), "/nix/var/nix/profiles/per-user/%s/sw/share/applications", user) < (int)sizeof(buf) &&
				!appdir_exists(appdirs, appdir_count, buf))
			snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
	}

	xdg_data_dirs = getenv("XDG_DATA_DIRS");
	if (!xdg_data_dirs || !*xdg_data_dirs)
		xdg_data_dirs = "/usr/local/share:/usr/share";
	snprintf(xdg_buf, sizeof(xdg_buf), "%s", xdg_data_dirs);
	for (char *tok = strtok_r(xdg_buf, ":", &saveptr); tok; tok = strtok_r(NULL, ":", &saveptr)) {
		if (!*tok || appdir_count >= (int)LENGTH(appdirs))
			continue;
		if (snprintf(buf, sizeof(buf), "%s/applications", tok) >= (int)sizeof(buf))
			continue;
		if (appdir_exists(appdirs, appdir_count, buf))
			continue;
		snprintf(appdirs[appdir_count++], sizeof(appdirs[0]), "%s", buf);
		if (appdir_count >= (int)LENGTH(appdirs))
			break;
	}

	for (int i = 0; i < appdir_count && desktop_entry_count < (int)LENGTH(desktop_entries); i++)
		load_desktop_dir(appdirs[i]);

	desktop_entries_loaded = 1;
}

static void
modal_update_results(Monitor *m)
{
	ModalOverlay *mo;
	int active;

	if (!m)
		return;
	mo = &m->modal;
	if (mo->active_idx < 0 || mo->active_idx > 2)
		mo->active_idx = 0;
	active = mo->active_idx;

	if (active != 1 && mo->file_search_pid > 0)
		modal_file_search_stop(m);

	if (active == 1) {
		if (mo->search_len[1] < modal_file_search_minlen) {
			modal_file_search_stop(m);
			modal_file_search_clear_results(m);
			mo->file_search_last[0] = '\0';
			return;
		}
		if (strncmp(mo->search[1], mo->file_search_last, sizeof(mo->file_search_last)) != 0) {
			modal_file_search_stop(m);
			modal_file_search_start(m);
		} else if (mo->file_search_pid <= 0 && mo->result_count[1] == 0 && !mo->file_search_fallback) {
			/* No search running and no results; retry current query once */
			modal_file_search_stop(m);
			modal_file_search_start(m);
		}
		if (mo->result_count[1] > 0) {
			if (mo->selected[1] < 0)
				mo->selected[1] = 0;
			modal_ensure_selection_visible(m);
		} else {
			mo->selected[1] = -1;
			mo->scroll[1] = 0;
		}
		return;
	}

	if (active != 0) {
		mo->selected[active] = -1;
		mo->scroll[active] = 0;
		return;
	}

	{
		int prev_entry = -1;
		int order[LENGTH(desktop_entries)];
		int order_count;

		if (mo->selected[0] >= 0 && mo->selected[0] < mo->result_count[0])
			prev_entry = mo->result_entry_idx[0][mo->selected[0]];

		mo->result_count[0] = 0;
		for (int i = 0; i < (int)LENGTH(mo->results[0]); i++)
			mo->results[0][i][0] = '\0';
		for (int i = 0; i < (int)LENGTH(mo->result_entry_idx[0]); i++)
			mo->result_entry_idx[0][i] = -1;

		ensure_desktop_entries_loaded();
		order_count = desktop_entry_count;
		if (order_count > (int)LENGTH(order))
			order_count = (int)LENGTH(order);
		for (int i = 0; i < order_count; i++)
			order[i] = i;
		qsort(order, order_count, sizeof(order[0]), desktop_entry_cmp_used);

		if (mo->search_len[0] <= 0) {
			for (int i = 0; i < order_count && mo->result_count[0] < (int)LENGTH(mo->results[0]); i++) {
				int idx = order[i];
				snprintf(mo->results[0][mo->result_count[0]],
						sizeof(mo->results[0][mo->result_count[0]]),
						"%s", desktop_entries[idx].name);
				mo->result_entry_idx[0][mo->result_count[0]] = idx;
				mo->result_count[0]++;
			}
		} else {
			char needle[256] = {0};
			int nlen = mo->search_len[0];
			if (nlen >= (int)sizeof(needle))
				nlen = (int)sizeof(needle) - 1;
			for (int i = 0; i < nlen; i++)
				needle[i] = (char)tolower((unsigned char)mo->search[0][i]);
			needle[nlen] = '\0';

			/* exact matches first */
			for (int i = 0; i < order_count && mo->result_count[0] < (int)LENGTH(mo->results[0]); i++) {
				int idx = order[i];
				if (strcmp(desktop_entries[idx].name_lower, needle) == 0) {
					snprintf(mo->results[0][mo->result_count[0]],
							sizeof(mo->results[0][mo->result_count[0]]),
							"%s", desktop_entries[idx].name);
					mo->result_entry_idx[0][mo->result_count[0]] = idx;
					mo->result_count[0]++;
				}
			}
			/* then prefix matches */
			for (int i = 0; i < order_count && mo->result_count[0] < (int)LENGTH(mo->results[0]); i++) {
				int idx = order[i];
				if (strncmp(desktop_entries[idx].name_lower, needle, (size_t)nlen) == 0 &&
						strcmp(desktop_entries[idx].name_lower, needle) != 0) {
					snprintf(mo->results[0][mo->result_count[0]],
							sizeof(mo->results[0][mo->result_count[0]]),
							"%s", desktop_entries[idx].name);
					mo->result_entry_idx[0][mo->result_count[0]] = idx;
					mo->result_count[0]++;
				}
			}
			/* finally substring matches */
			for (int i = 0; i < order_count && mo->result_count[0] < (int)LENGTH(mo->results[0]); i++) {
				int idx = order[i];
				if (strstr(desktop_entries[idx].name_lower, needle) &&
						strncmp(desktop_entries[idx].name_lower, needle, (size_t)nlen) != 0) {
					snprintf(mo->results[0][mo->result_count[0]],
							sizeof(mo->results[0][mo->result_count[0]]),
							"%s", desktop_entries[idx].name);
					mo->result_entry_idx[0][mo->result_count[0]] = idx;
					mo->result_count[0]++;
				}
			}
		}

		mo->selected[0] = -1;
		if (mo->result_count[0] > 0) {
			int found_prev = 0;
			if (prev_entry >= 0) {
				for (int i = 0; i < mo->result_count[0]; i++) {
					if (mo->result_entry_idx[0][i] == prev_entry) {
						mo->selected[0] = i;
						found_prev = 1;
						break;
					}
				}
			}
			if (!found_prev) {
				mo->selected[0] = 0;
				mo->scroll[0] = 0;
			}
		}
	}

	if (mo->result_count[0] > 0)
		modal_ensure_selection_visible(m);
	else {
		mo->selected[0] = -1;
		mo->scroll[0] = 0;
	}
}

static void
modal_prewarm(Monitor *m)
{
	int prev_idx;

	if (!m || !m->modal.tree || m->modal.visible)
		return;

	ensure_desktop_entries_loaded();

	prev_idx = m->modal.active_idx;
	m->modal.active_idx = 0;
	m->modal.search[0][0] = '\0';
	m->modal.search_len[0] = 0;
	modal_update_results(m);
	m->modal.active_idx = prev_idx;
	m->modal.selected[0] = -1;
	m->modal.scroll[0] = 0;
}

static void
modal_render(Monitor *m)
{
	static const char *labels[] = {
		"App Launcher", "File Search", "Grep Search"
	};
	ModalOverlay *mo;
	int btn_h, btn_w, last_w, field_h, line_h, pad;
	struct wlr_scene_node *node, *tmp;

	if (!m || !m->modal.tree)
		return;

	mo = &m->modal;
	if (!mo->visible || mo->width <= 0 || mo->height <= 0)
		return;

	modal_layout_metrics(&btn_h, &field_h, &line_h, &pad);
	btn_w = mo->width / 3;
	if (btn_w <= 0)
		btn_w = mo->width;
	last_w = mo->width - btn_w * 2;
	if (last_w < btn_w)
		last_w = btn_w;
	if (mo->active_idx < 0 || mo->active_idx > 2)
		mo->active_idx = 0;
	modal_update_results(m);

	/* clear existing non-bg nodes */
	wl_list_for_each_safe(node, tmp, &mo->tree->children, link) {
		if (mo->bg && node == &mo->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	/* redraw background */
	if (mo->bg) {
		wl_list_for_each_safe(node, tmp, &mo->bg->children, link)
			wlr_scene_node_destroy(node);
		drawrect(mo->bg, 0, 0, mo->width, mo->height, statusbar_popup_bg);
		/* 1px black border */
		if (mo->width > 0 && mo->height > 0) {
			const float border[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			drawrect(mo->bg, 0, 0, mo->width, 1, border); /* top */
			drawrect(mo->bg, 0, mo->height - 1, mo->width, 1, border); /* bottom */
			drawrect(mo->bg, 0, 0, 1, mo->height, border); /* left */
			if (mo->width > 1)
				drawrect(mo->bg, mo->width - 1, 0, 1, mo->height, border); /* right */
		}
	}

	for (int i = 0; i < 3; i++) {
		int x = i * btn_w;
		int w = (i == 2) ? last_w : btn_w;
		const float *col = (mo->active_idx == i) ? statusbar_tag_active_bg : statusbar_tag_bg;
		struct wlr_scene_tree *btn = wlr_scene_tree_create(mo->tree);
		StatusModule mod = {0};
		int text_w;
		int text_x;

		if (btn) {
			wlr_scene_node_set_position(&btn->node, x, 0);
			drawrect(btn, 0, 0, w, btn_h, col);
			mod.tree = btn;
			text_w = status_text_width(labels[i]);
			text_x = (w - text_w) / 2;
			if (text_x < 4)
				text_x = 4;
			tray_render_label(&mod, labels[i], text_x, btn_h, statusbar_fg);
		}
	}

	/* search field */
	{
		int field_x = pad;
		int field_y = btn_h + pad;
		int field_w = mo->width - pad * 2;
		const float field_bg[4] = {0.1f, 0.1f, 0.1f, 0.5f};
		const float border[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		const char *text = mo->search[mo->active_idx];
		const char *to_draw = (text && *text) ? text : "Type to search...";
		struct wlr_scene_tree *row;
		StatusModule mod = {0};
		int text_x;
		int text_w;

		if (field_w < 20)
			field_w = 20;
		if (field_h < 20)
			field_h = 20;

		if (mo->bg) {
			drawrect(mo->bg, field_x, field_y, field_w, field_h, field_bg);
			drawrect(mo->bg, field_x, field_y, field_w, 1, border);
			drawrect(mo->bg, field_x, field_y + field_h - 1, field_w, 1, border);
			drawrect(mo->bg, field_x, field_y, 1, field_h, border);
			drawrect(mo->bg, field_x + field_w - 1, field_y, 1, field_h, border);
		}

		row = wlr_scene_tree_create(mo->tree);
		if (row) {
			wlr_scene_node_set_position(&row->node, 0, field_y);
			mod.tree = row;
			text_w = status_text_width(to_draw);
			text_x = field_x + (field_w - text_w) / 2;
			if (text_x < field_x + 6)
				text_x = field_x + 6;
			tray_render_label(&mod, to_draw, text_x, field_h, statusbar_fg);
		}
	}

	/* results list */
	if (mo->active_idx >= 0 && mo->active_idx < 3 && mo->result_count[mo->active_idx] > 0) {
		int line_y = btn_h + pad + field_h + pad;
		int max_lines = modal_max_visible_lines(m);
		int start = mo->scroll[mo->active_idx];
		int max_start;
		int col_name_w = 0, col_path_w = 0, col_gap = 12;

		if (max_lines <= 0)
			return;
		max_start = mo->result_count[mo->active_idx] - max_lines;
		if (max_start < 0)
			max_start = 0;
		if (start < 0)
			start = 0;
		if (start > max_start)
			start = max_start;
		mo->scroll[mo->active_idx] = start;

		if (mo->active_idx == 1) {
			int total_w = mo->width - pad * 2 - 4;
			int name_min = 80;
			int path_min = 120;
			col_name_w = total_w / 2;
			if (col_name_w < name_min)
				col_name_w = name_min;
			col_path_w = total_w - col_name_w - col_gap;
			if (col_path_w < path_min) {
				col_path_w = path_min;
				col_name_w = total_w - col_gap - col_path_w;
			}
			if (col_name_w < name_min)
				col_name_w = name_min;
		}

		for (int i = 0; i < max_lines && (start + i) < mo->result_count[mo->active_idx]; i++) {
			int idx = start + i;
			struct wlr_scene_tree *row = wlr_scene_tree_create(mo->tree);
			StatusModule mod = {0};
			char text_buf[MODAL_RESULT_LEN];
			int max_px = mo->width - pad * 2 - 8;
			char name_buf[128];
			char path_buf[128];
			char short_path[128];
			const char *name = mo->file_results_name[idx];
			const char *path = mo->file_results_path[idx];
			char path_dir[PATH_MAX];
			int path_w = 0;
			int path_x = 0;
			char *slash = NULL;

			if (!row)
				continue;
			wlr_scene_node_set_position(&row->node, 0, line_y + i * line_h);
			mod.tree = row;
			if (mo->selected[mo->active_idx] == idx)
				drawrect(row, pad - 2, 0, mo->width - pad * 2 + 4, line_h, statusbar_tag_active_bg);

			if (mo->active_idx == 1) {
				if (path) {
					snprintf(path_dir, sizeof(path_dir), "%s", path);
					slash = strrchr(path_dir, '/');
					if (slash && slash != path_dir)
						*slash = '\0';
					else if (slash)
						*(slash + 1) = '\0';
				} else {
					snprintf(path_dir, sizeof(path_dir), ".");
				}
				shorten_path_display(path_dir, short_path, sizeof(short_path));
				modal_truncate_to_width(name ? name : "", name_buf, sizeof(name_buf), col_name_w);
				modal_truncate_to_width(short_path, path_buf, sizeof(path_buf), col_path_w);
				path_w = status_text_width(path_buf);
				path_x = mo->width - pad - path_w;
				if (path_x < pad + 10)
					path_x = pad + 10;
				tray_render_label(&mod, name_buf[0] ? name_buf : "--", pad, line_h, statusbar_fg);
				tray_render_label(&mod, path_buf[0] ? path_buf : "--", path_x, line_h, statusbar_fg);
			} else {
				if (max_px < 40)
					max_px = 40;
				modal_truncate_to_width(mo->results[mo->active_idx][idx],
						text_buf, sizeof(text_buf), max_px);
				if (!text_buf[0])
					snprintf(text_buf, sizeof(text_buf), "%s", mo->results[mo->active_idx][idx]);
				tray_render_label(&mod, text_buf, pad, line_h, statusbar_fg);
			}
		}
	}
}

static void
schedule_next_status_refresh(void)
{
	uint64_t now = monotonic_msec();
	uint64_t next = UINT64_MAX;

	if (!status_cpu_timer)
		return;

	for (size_t i = 0; i < LENGTH(status_tasks); i++) {
		if (status_tasks[i].next_due_ms < next)
			next = status_tasks[i].next_due_ms;
	}

	if (next == UINT64_MAX)
		return;

	if (next <= now)
		wl_event_source_timer_update(status_cpu_timer, 1);
	else
		wl_event_source_timer_update(status_cpu_timer, (int)(next - now));
}

static void
schedule_status_timer(void)
{
	struct timespec ts;
	double now, next;
	int ms;

	if (!status_timer)
		return;

	clock_gettime(CLOCK_REALTIME, &ts);
	now = ts.tv_sec + ts.tv_nsec / 1e9;
	next = ceil(now / 60.0) * 60.0;
	ms = (int)((next - now) * 1000.0);
	if (ms < 1)
		ms = 1;

	wl_event_source_timer_update(status_timer, ms);
}

static int
updatestatuscpu(void *data)
{
	size_t chosen = 0;
	uint64_t best = 0;
	uint64_t now = monotonic_msec();
	int found = 0;
	uint64_t since_motion = last_pointer_motion_ms ? now - last_pointer_motion_ms : UINT64_MAX;

	(void)data;

	if (since_motion < 8) {
		wl_event_source_timer_update(status_cpu_timer, 8 - (int)since_motion);
		return 0;
	}

	for (size_t i = 0; i < LENGTH(status_tasks); i++) {
		if (!found || status_tasks[i].next_due_ms < best) {
			best = status_tasks[i].next_due_ms;
			chosen = i;
			found = 1;
		}
	}

	if (!found) {
		schedule_next_status_refresh();
		return 0;
	}

	if (best > now) {
		schedule_next_status_refresh();
		return 0;
	}

	status_tasks[chosen].fn();
	if (status_tasks[chosen].fn == refreshstatusnet) {
		uint64_t delay_ms = 60000;
		uint64_t allow_fast_after = now;
		int popup_active = 0;
		Monitor *m;

		wl_list_for_each(m, &mons, link) {
			if (m->showbar && m->statusbar.net_popup.visible) {
				popup_active = 1;
				if (m->statusbar.net_popup.suppress_refresh_until_ms > allow_fast_after)
					allow_fast_after = m->statusbar.net_popup.suppress_refresh_until_ms;
			}
		}

		if (popup_active) {
			if (allow_fast_after > now)
				delay_ms = allow_fast_after - now;
			else
				delay_ms = 1000;
		}
		status_tasks[chosen].next_due_ms = now + delay_ms;
	} else if (status_task_hover_active(status_tasks[chosen].fn)) {
		status_tasks[chosen].next_due_ms = now + STATUS_FAST_MS;
	} else {
		status_tasks[chosen].next_due_ms = now + random_status_delay_ms();
	}
	schedule_next_status_refresh();
	return 0;
}

static int
updatehoverfade(void *data)
{
	int need_more = 0;
	Monitor *mon;
	float step;

	(void)data;

	if (statusbar_hover_fade_ms <= 0)
		return 0;

	step = 16.0f / (float)statusbar_hover_fade_ms;

	wl_list_for_each(mon, &mons, link) {
		Monitor *m = mon;
		int barh;
		if (!m->showbar || !m->statusbar.tags.tree)
			continue;
		if (m->statusbar.tags.hover_tag < 0 && m->statusbar.tags.tagmask == 0)
			continue;

		for (int i = 0; i < TAGCOUNT; i++) {
			float target = (m->statusbar.tags.hover_tag == i) ? 1.0f : 0.0f;
			float alpha = m->statusbar.tags.hover_alpha[i];
			if (target > alpha) {
				alpha += step;
				if (alpha > target)
					alpha = target;
			} else if (target < alpha) {
				alpha -= step;
				if (alpha < target)
					alpha = target;
			}
			if (alpha != m->statusbar.tags.hover_alpha[i])
				need_more = 1;
			m->statusbar.tags.hover_alpha[i] = alpha;
		}

		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		renderworkspaces(m, &m->statusbar.tags, barh);
		positionstatusmodules(m);
	}

	if (need_more && status_hover_timer)
		wl_event_source_timer_update(status_hover_timer, 16);

	return 0;
}

static void
updatecpuhover(Monitor *m, double cx, double cy)
{
	int lx, ly;
	int inside = 0;
	int popup_hover = 0;
	int was_visible;
	CpuPopup *p;
	int popup_x;
	int new_hover = -1;
	uint64_t now = monotonic_msec();
	int need_refresh = 0;
	int stale_refresh = 0;

	if (!m || !m->showbar || !m->statusbar.cpu.tree || !m->statusbar.cpu_popup.tree) {
		if (m && m->statusbar.cpu_popup.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.cpu_popup.tree->node, 0);
			m->statusbar.cpu_popup.visible = 0;
			m->statusbar.cpu_popup.hover_idx = -1;
		}
		return;
	}

	p = &m->statusbar.cpu_popup;
	lx = (int)floor(cx) - m->statusbar.area.x;
	ly = (int)floor(cy) - m->statusbar.area.y;

	popup_x = m->statusbar.cpu.x;
	if (p->width > 0 && m->statusbar.area.width > 0) {
		int max_x = m->statusbar.area.width - p->width;
		if (max_x < 0)
			max_x = 0;
		if (popup_x > max_x)
			popup_x = max_x;
		if (popup_x < 0)
			popup_x = 0;
	}

	if (p->visible && p->width > 0 && p->height > 0 &&
			lx >= popup_x &&
			lx < popup_x + p->width &&
			ly >= m->statusbar.area.height &&
			ly < m->statusbar.area.height + p->height) {
		popup_hover = 1;
	}

	if (lx >= m->statusbar.cpu.x &&
			lx < m->statusbar.cpu.x + m->statusbar.cpu.width &&
			ly >= 0 && ly < m->statusbar.area.height &&
			m->statusbar.cpu.width > 0) {
		inside = 1;
	} else if (popup_hover) {
		inside = 1;
	}

	was_visible = p->visible;

	if (inside) {
		if (!was_visible) {
			/* Delay heavy popup refresh until pointer lingers for 2s */
			p->suppress_refresh_until_ms = now + 2000;
		}
		p->visible = 1;
		wlr_scene_node_set_enabled(&p->tree->node, 1);
		wlr_scene_node_set_position(&p->tree->node,
				popup_x, m->statusbar.area.height);
		new_hover = cpu_popup_hover_index(m, p);
		stale_refresh = (p->last_fetch_ms == 0 ||
				now < p->last_fetch_ms ||
				(now - p->last_fetch_ms) >= cpu_popup_refresh_interval_ms);
		need_refresh = (!was_visible || stale_refresh) &&
				(p->suppress_refresh_until_ms == 0 ||
				 now >= p->suppress_refresh_until_ms);
		if (need_refresh)
			p->refresh_data = 1;
		if (new_hover != p->hover_idx || !was_visible || need_refresh) {
			int allow_render = 1;
			if (!need_refresh && was_visible && new_hover != p->hover_idx &&
					p->last_render_ms > 0 && now >= p->last_render_ms &&
					now - p->last_render_ms < 16)
				allow_render = 0;
			if (allow_render) {
				p->hover_idx = new_hover;
				rendercpupopup(m);
			}
		}
		if (!was_visible)
			schedule_cpu_popup_refresh(2000);
	} else if (p->visible) {
		p->visible = 0;
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->hover_idx = -1;
		p->refresh_data = 0;
		p->last_render_ms = 0;
		p->suppress_refresh_until_ms = 0;
	}
}

static void
updatenethover(Monitor *m, double cx, double cy)
{
	int lx, ly;
	int inside = 0;
	int was_visible;
	StatusModule *icon_mod;
	NetPopup *p;
	uint64_t now = monotonic_msec();

	if (!m || !m->showbar || !m->statusbar.net_popup.tree
			|| (m->statusbar.net_menu.visible)) {
		if (m && m->statusbar.net_popup.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.net_popup.tree->node, 0);
			m->statusbar.net_popup.visible = 0;
		}
		return;
	}

	p = &m->statusbar.net_popup;
	icon_mod = &m->statusbar.sysicons;
	lx = (int)floor(cx) - m->statusbar.area.x;
	ly = (int)floor(cy) - m->statusbar.area.y;

	if (icon_mod->width > 0 &&
		lx >= icon_mod->x &&
		lx < icon_mod->x + icon_mod->width &&
		ly >= 0 && ly < m->statusbar.area.height) {
		inside = 1;
	}
	/* Allow anchor data when icon is temporarily zero-sized */
	if (!inside && p->anchor_w > 0) {
		int lx_abs = (int)floor(cx);
		int ly_abs = (int)floor(cy);
		int ax0 = m->statusbar.area.x + p->anchor_x;
		int ay0 = m->statusbar.area.y;
		if (lx_abs >= ax0 && lx_abs < ax0 + p->anchor_w &&
				ly_abs >= ay0 && ly_abs < ay0 + m->statusbar.area.height)
			inside = 1;
	}

	/* Keep visible while hovering popup itself */
	if (!inside && p->visible && p->width > 0 && p->height > 0) {
		int px0 = m->statusbar.area.x + (icon_mod->width > 0 ? icon_mod->x : p->anchor_x);
		int py0 = m->statusbar.area.y + m->statusbar.area.height;
		int px1 = px0 + p->width;
		int py1 = py0 + p->height;
		int cx_i = (int)floor(cx);
		int cy_i = (int)floor(cy);
		if (cx_i >= px0 && cx_i < px1 && cy_i >= py0 && cy_i < py1)
			inside = 1;
	}

	was_visible = p->visible;

	if (inside) {
		if (!was_visible) {
			p->suppress_refresh_until_ms = now + 2000;
			set_status_task_due(refreshstatusnet, p->suppress_refresh_until_ms);
		}
		p->visible = 1;
		wlr_scene_node_set_enabled(&p->tree->node, 1);
		wlr_scene_node_set_position(&p->tree->node,
				icon_mod->width > 0 ? icon_mod->x : p->anchor_x,
				m->statusbar.area.height);
		if (!was_visible) {
			p->anchor_x = icon_mod->width > 0 ? icon_mod->x : p->anchor_x;
			p->anchor_y = m->statusbar.area.height;
			p->anchor_w = icon_mod->width > 0 ? icon_mod->width : p->anchor_w;
			rendernetpopup(m);
		}
	} else if (p->visible) {
		p->visible = 0;
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->suppress_refresh_until_ms = 0;
		set_status_task_due(refreshstatusnet, now + 60000);
	}
}

static int
updatestatusclock(void *data)
{
	(void)data;
	refreshstatusclock();
	schedule_status_timer();
	return 0;
}

void
arrange(Monitor *m)
{
	Client *c;

	if (!m->wlr_output->enabled)
		return;

	wl_list_for_each(c, &clients, link) {
		if (c->mon == m) {
			wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, m));
			client_set_suspended(c, !VISIBLEON(c, m));
		}
	}

	wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
			(c = focustop(m)) && c->isfullscreen);

	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));

	/* We move all clients (except fullscreen and unmanaged) to LyrTile while
	 * in floating layout to avoid "real" floating clients be always on top */
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || c->scene->node.parent == layers[LyrFS])
			continue;

		wlr_scene_node_reparent(&c->scene->node,
				(!m->lt[m->sellt]->arrange && c->isfloating)
						? layers[LyrTile]
						: (m->lt[m->sellt]->arrange && c->isfloating)
								? layers[LyrFloat]
								: c->scene->node.parent);
	}

	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
	motionnotify(0, NULL, 0, 0, 0, 0);
	checkidleinhibitor(NULL);
	refreshstatustags();
	warpcursor(focustop(selmon));
}

void
arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive)
{
	LayerSurface *l;
	struct wlr_box full_area = m->m;

	wl_list_for_each(l, list, link) {
		struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;

		if (!layer_surface->initialized)
			continue;

		if (exclusive != (layer_surface->current.exclusive_zone > 0))
			continue;

		wlr_scene_layer_surface_v1_configure(l->scene_layer, &full_area, usable_area);
		wlr_scene_node_set_position(&l->popups->node, l->scene->node.x, l->scene->node.y);
	}
}

void
arrangelayers(Monitor *m)
{
	int i;
	struct wlr_box usable_area = m->m;
	struct wlr_box client_area;
	struct wlr_box old_w = m->w;
	LayerSurface *l;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	if (!m->wlr_output->enabled)
		return;

	/* Arrange exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 1);

	client_area = usable_area;
	layoutstatusbar(m, &usable_area, &client_area);

	if (!wlr_box_equal(&client_area, &old_w)) {
		m->w = client_area;
		arrange(m);
	} else {
		positionstatusmodules(m);
	}

	/* Arrange non-exlusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 0);

	refreshstatusclock();

	/* Find topmost keyboard interactive layer, if such a layer exists */
	for (i = 0; i < (int)LENGTH(layers_above_shell); i++) {
		wl_list_for_each_reverse(l, &m->layers[layers_above_shell[i]], link) {
			if (locked || !l->layer_surface->current.keyboard_interactive || !l->mapped)
				continue;
			/* Deactivate the focused client. */
			focusclient(NULL, 0);
			exclusive_focus = l;
			client_notify_enter(l->layer_surface->surface, wlr_seat_get_keyboard(seat));
			return;
		}
	}
}

static int
scrollsteps(const struct wlr_pointer_axis_event *event)
{
	int steps;

	if (!event)
		return 0;

	if (event->delta_discrete > 0)
		steps = 1;
	else if (event->delta_discrete < 0)
		steps = -1;
	else if (event->delta > 0.0)
		steps = 1;
	else if (event->delta < 0.0)
		steps = -1;
	else
		steps = 0;

	if (event->relative_direction == WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED)
		steps = -steps;

	return steps;
}

static int
adjust_backlight_by_steps(int steps)
{
	double percent;
	double delta;

	if (steps == 0)
		return 0;

	backlight_available = findbacklightdevice(backlight_brightness_path,
			sizeof(backlight_brightness_path),
			backlight_max_path, sizeof(backlight_max_path));

	percent = backlight_percent();
	if (percent < 0.0)
		percent = light_last_percent;
	if (percent < 0.0)
		percent = light_cached_percent >= 0.0 ? light_cached_percent : 50.0; /* fallback default */

	delta = (double)steps * light_step;

	/* First try relative adjustment via helper and return on success */
	if (set_backlight_relative(delta) == 0) {
		double newp = backlight_percent();
		if (newp >= 0.0)
			percent = newp;
		else
			percent = percent + delta;
		if (percent < 0.0)
			percent = 0.0;
		if (percent > 100.0)
			percent = 100.0;
		light_last_percent = percent;
		light_cached_percent = percent;
		refreshstatuslight();
		return 1;
	}

	/* Fall back to absolute write */
	{
		double target = percent + delta;
		if (target < 0.0)
			target = 0.0;
		if (target > 100.0)
			target = 100.0;

		if (set_backlight_percent(target) != 0) {
			light_last_percent = target;
			light_cached_percent = target;
			refreshstatuslight();
			return 1;
		}
		light_last_percent = target;
		light_cached_percent = target;
	}

	refreshstatuslight();
	return 1;
}

static int
adjust_volume_by_steps(int steps)
{
	double vol;
	int is_headset;
	uint64_t now;

	if (steps == 0)
		return 0;

	is_headset = pipewire_sink_is_headset();
	volume_invalidate_cache(is_headset); /* force fresh read if needed */

	vol = speaker_active >= 0.0 ? speaker_active : volume_last_for_type(is_headset);
	if (vol < 0.0)
		vol = pipewire_volume_percent(&is_headset);
	if (vol < 0.0)
		return 0;

	if (volume_muted == 1) {
		int mute_res = system("wpctl set-mute @DEFAULT_AUDIO_SINK@ 0");
		(void)mute_res;
		volume_muted = 0;
	}

	vol += (double)steps * volume_step;
	if (vol < 0.0)
		vol = 0.0;
	if (vol > volume_max_percent)
		vol = volume_max_percent;

	if (set_pipewire_volume(vol) != 0)
		return 0;

	now = monotonic_msec();
	volume_cache_store(is_headset, vol, 0, now);
	speaker_active = vol;
	if (is_headset) {
		volume_cached_headset_muted = 0;
	} else {
		volume_cached_speaker_muted = 0;
	}
	refreshstatusvolume();
	return 1;
}

static int
adjust_mic_by_steps(int steps)
{
	double vol;
	double target;

	if (steps == 0)
		return 0;

	mic_last_read_ms = 0;
	vol = microphone_active >= 0.0 ? microphone_active : mic_last_percent;
	if (vol < 0.0)
		vol = pipewire_mic_volume_percent();
	if (vol < 0.0)
		return 0;

	if (mic_muted == 1) {
		int mute_res = system("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 0");
		(void)mute_res;
		mic_muted = 0;
	}

	target = vol + (double)steps * mic_step;
	if (target < 0.0)
		target = 0.0;
	if (target > mic_max_percent)
		target = mic_max_percent;

	if (set_pipewire_mic_volume(target) != 0)
		return 0;

	mic_last_percent = target;
	mic_cached = target;
	mic_cached_muted = 0;
	mic_muted = 0;
	mic_last_read_ms = monotonic_msec();
	microphone_active = target;
	refreshstatusmic();
	return 1;
}

static int
handlestatusscroll(struct wlr_pointer_axis_event *event)
{
	Monitor *m;
	int lx, ly, steps;

	if (!event || locked)
		return 0;

	if (event->orientation != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return 0;

	m = xytomon(cursor->x, cursor->y);
	if (!m)
		return 0;
	selmon = m;

	if (!m->showbar || !m->statusbar.area.width || !m->statusbar.area.height)
		return 0;

	lx = (int)floor(cursor->x) - m->statusbar.area.x;
	ly = (int)floor(cursor->y) - m->statusbar.area.y;
	if (lx < 0 || ly < 0 || lx >= m->statusbar.area.width || ly >= m->statusbar.area.height)
		return 0;

	steps = scrollsteps(event);
	if (steps == 0)
		return 0;

	if (m->statusbar.light.width > 0 &&
			lx >= m->statusbar.light.x &&
			lx < m->statusbar.light.x + m->statusbar.light.width) {
		return adjust_backlight_by_steps(steps);
	}

	if (m->statusbar.mic.width > 0 &&
			lx >= m->statusbar.mic.x &&
			lx < m->statusbar.mic.x + m->statusbar.mic.width) {
		return adjust_mic_by_steps(steps);
	}

	if (m->statusbar.volume.width > 0 &&
			lx >= m->statusbar.volume.x &&
			lx < m->statusbar.volume.x + m->statusbar.volume.width) {
		return adjust_volume_by_steps(steps);
	}

	return 0;
}

void
axisnotify(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct wlr_pointer_axis_event *event = data;
	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	if (!handlestatusscroll(event)) {
		/* Notify the client with pointer focus of the axis event. */
		wlr_seat_pointer_notify_axis(seat,
				event->time_msec, event->orientation, event->delta,
				event->delta_discrete, event->source, event->relative_direction);
	}
}

void
buttonpress(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_button_event *event = data;
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	Client *c, *target = NULL;
	const Button *b;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	switch (event->state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		cursor_mode = CurPressed;
		selmon = xytomon(cursor->x, cursor->y);
		if (locked)
			break;

		/* Modal overlay eats clicks; outside closes it */
		{
			Monitor *modal_mon = modal_visible_monitor();
			if (modal_mon) {
				ModalOverlay *mo = &modal_mon->modal;
				int inside = cursor->x >= mo->x && cursor->x < mo->x + mo->width &&
						cursor->y >= mo->y && cursor->y < mo->y + mo->height;
				if (!inside)
					modal_hide(modal_mon);
				return;
			}
		}

		if (selmon && selmon->statusbar.tray_menu.visible) {
			int lx = (int)lround(cursor->x - selmon->statusbar.area.x);
			int ly = (int)lround(cursor->y - selmon->statusbar.area.y);
			TrayMenu *menu = &selmon->statusbar.tray_menu;
			int relx = lx - menu->x;
			int rely = ly - menu->y;
			if (relx >= 0 && rely >= 0 &&
					relx < menu->width && rely < menu->height) {
				TrayMenuEntry *entry = tray_menu_entry_at(selmon, relx, rely);
				if (entry)
					tray_menu_send_event(menu, entry, event->time_msec);
				tray_menu_hide_all();
				return;
			} else {
				tray_menu_hide_all();
			}
		}

		if (selmon && selmon->statusbar.net_menu.visible) {
			int lx = (int)lround(cursor->x - selmon->statusbar.area.x);
			int ly = (int)lround(cursor->y - selmon->statusbar.area.y);
			if (net_menu_handle_click(selmon, lx, ly, event->button))
				return;
		}

		if (selmon && selmon->showbar) {
			int lx = (int)lround(cursor->x - selmon->statusbar.area.x);
			int ly = (int)lround(cursor->y - selmon->statusbar.area.y);
			StatusModule *tags = &selmon->statusbar.tags;
			StatusModule *mic = &selmon->statusbar.mic;
			StatusModule *vol = &selmon->statusbar.volume;
			StatusModule *cpu = &selmon->statusbar.cpu;
			StatusModule *sys = &selmon->statusbar.sysicons;

			if (selmon->statusbar.cpu_popup.visible) {
				if (cpu_popup_handle_click(selmon, lx, ly, event->button))
					return;
			}

			if (lx >= 0 && ly >= 0 &&
					lx < selmon->statusbar.area.width &&
					ly < selmon->statusbar.area.height) {
				if (event->button == BTN_RIGHT &&
						sys->width > 0 &&
						lx >= sys->x && lx < sys->x + sys->width) {
					net_menu_hide_all();
					net_menu_open(selmon);
					return;
				}

				if (cpu->width > 0 && lx >= cpu->x && lx < cpu->x + cpu->width) {
					Arg arg = { .v = btopcmd };
					spawn(&arg);
					return;
				}

				if (mic->width > 0 && lx >= mic->x && lx < mic->x + mic->width) {
					toggle_pipewire_mic_mute();
					return;
				}

				if (vol->width > 0 && lx >= vol->x && lx < vol->x + vol->width) {
					toggle_pipewire_mute();
					return;
				}

				if (event->button == BTN_LEFT && lx < tags->width) {
					for (int i = 0; i < tags->box_count; i++) {
						int bx = tags->box_x[i];
						int bw = tags->box_w[i];
						if (lx >= bx && lx < bx + bw) {
							Arg arg = { .ui = 1u << tags->box_tag[i] };
							view(&arg);
							return;
						}
					}
				}
			}
		}

		/* Change focus if the button was _pressed_ over a client */
		xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
			focusclient(c, 1);

		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		for (b = buttons; b < END(buttons); b++) {
			if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
					event->button == b->button && b->func) {
				b->func(&b->arg);
				return;
			}
		}
		break;
	case WL_POINTER_BUTTON_STATE_RELEASED:
		/* If you released any buttons, we exit interactive move/resize mode. */
		/* TODO: should reset to the pointer focus's current setcursor */
		if (!locked && cursor_mode != CurNormal && cursor_mode != CurPressed) {
			c = grabc;
			if (c && c->was_tiled && !strcmp(selmon->ltsymbol, "|w|")) {
				if (cursor_mode == CurMove && c->isfloating) {
					target = xytoclient(cursor->x, cursor->y);

					if (target && !target->isfloating && !target->isfullscreen)
						insert_client(selmon, target, c);
					else
						selmon->root = create_client_node(c);

					setfloating(c, 0);
					arrange(selmon);

				} else if (cursor_mode == CurResize && !c->isfloating) {
					resizing_from_mouse = 0;
				}
			} else {
				if (cursor_mode == CurResize && resizing_from_mouse)
					resizing_from_mouse = 0;
				resize_last_time = 0;
			}
			/* Default behaviour */
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
			cursor_mode = CurNormal;
			/* Drop the window off on its new monitor */
			selmon = xytomon(cursor->x, cursor->y);
			setmon(grabc, selmon, 0);
			grabc = NULL;
			return;
		}
		cursor_mode = CurNormal;
		break;
	}
	/* If the event wasn't handled by the compositor, notify the client with
	 * pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(seat,
			event->time_msec, event->button, event->state);
}

void
chvt(const Arg *arg)
{
	wlr_session_change_vt(session, arg->ui);
}

void
checkidleinhibitor(struct wlr_surface *exclude)
{
	int inhibited = 0, unused_lx, unused_ly;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
		struct wlr_surface *surface = wlr_surface_get_root_surface(inhibitor->surface);
		struct wlr_scene_tree *tree = surface->data;
		if (exclude != surface && (bypass_surface_visibility || (!tree
				|| wlr_scene_node_coords(&tree->node, &unused_lx, &unused_ly)))) {
			inhibited = 1;
			break;
		}
	}

	wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);
}

void
cleanup(void)
{
	TrayItem *it, *tmp;

	cleanuplisteners();
	drop_net_icon_buffer();
	drop_cpu_icon_buffer();
	drop_clock_icon_buffer();
	drop_light_icon_buffer();
	drop_ram_icon_buffer();
	drop_battery_icon_buffer();
	drop_mic_icon_buffer();
	drop_volume_icon_buffer();
	stop_public_ip_fetch();
	stop_ssid_fetch();
	wifi_scan_finish();
	net_menu_hide_all();
	wifi_networks_clear();
	last_clock_render[0] = last_cpu_render[0] = last_ram_render[0] = '\0';
	last_light_render[0] = last_volume_render[0] = last_mic_render[0] = last_battery_render[0] = '\0';
	last_net_render[0] = '\0';
	last_clock_h = last_cpu_h = last_ram_h = last_light_h = last_volume_h = last_mic_h = last_battery_h = last_net_h = 0;
	if (wifi_scan_timer) {
		wl_event_source_remove(wifi_scan_timer);
		wifi_scan_timer = NULL;
	}
	if (status_timer) {
		wl_event_source_remove(status_timer);
		status_timer = NULL;
	}
	if (status_cpu_timer) {
		wl_event_source_remove(status_cpu_timer);
		status_cpu_timer = NULL;
	}
	if (status_hover_timer) {
		wl_event_source_remove(status_hover_timer);
		status_hover_timer = NULL;
	}
	if (cpu_popup_refresh_timer) {
		wl_event_source_remove(cpu_popup_refresh_timer);
		cpu_popup_refresh_timer = NULL;
	}
	tray_menu_hide_all();
	if (tray_event) {
		wl_event_source_remove(tray_event);
		tray_event = NULL;
	}
	if (tray_vtable_slot)
		sd_bus_slot_unref(tray_vtable_slot);
	if (tray_fdo_vtable_slot)
		sd_bus_slot_unref(tray_fdo_vtable_slot);
	if (tray_name_slot)
		sd_bus_slot_unref(tray_name_slot);
	if (tray_bus)
		sd_bus_unref(tray_bus);
	wl_list_for_each_safe(it, tmp, &tray_items, link) {
		if (it->icon_buf)
			wlr_buffer_drop(it->icon_buf);
		wl_list_remove(&it->link);
		free(it);
	}
#ifdef XWAYLAND
	wlr_xwayland_destroy(xwayland);
	xwayland = NULL;
#endif
	/* Stop systemd target */
	if (fork() == 0) {
		setsid();
		if (has_dwl_session_target()) {
			execvp("systemctl", (char *const[]) {
				"systemctl", "--user", "stop", "dwl-session.target", NULL
			});
		}
		exit(1);
	}

	wl_display_destroy_clients(dpy);
	if (child_pid > 0) {
		kill(-child_pid, SIGTERM);
		waitpid(child_pid, NULL, 0);
	}
	freestatusfont();
	if (fcft_initialized) {
		fcft_fini();
		fcft_initialized = 0;
	}
	wlr_xcursor_manager_destroy(cursor_mgr);

	destroykeyboardgroup(&kb_group->destroy, NULL);

	/* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
	 * Destroy it until it's fixed on the wlroots side */
	wlr_backend_destroy(backend);

	wl_display_destroy(dpy);
	/* Destroy after the wayland display (when the monitors are already destroyed)
	   to avoid destroying them with an invalid scene output. */
	wlr_scene_node_destroy(&scene->tree.node);
}

void
cleanupmon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy);
	LayerSurface *l, *tmp;
	size_t i;

	modal_file_search_stop(m);

	/* m->layers[i] are intentionally not unlinked */
	for (i = 0; i < LENGTH(m->layers); i++) {
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->layer_surface);
	}

	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->link);
	wl_list_remove(&m->request_state.link);
	if (m->lock_surface)
		destroylocksurface(&m->destroy_lock_surface, NULL);
	m->wlr_output->data = NULL;
	wlr_output_layout_remove(output_layout, m->wlr_output);
	wlr_scene_output_destroy(m->scene_output);

	if (m->statusbar.tree)
		wlr_scene_node_destroy(&m->statusbar.tree->node);
	if (m->modal.tree)
		wlr_scene_node_destroy(&m->modal.tree->node);
	destroy_tree(m);
	closemon(m);
	wlr_scene_node_destroy(&m->fullscreen_bg->node);
	free(m);
}

void
cleanuplisteners(void)
{
	wl_list_remove(&cursor_axis.link);
	wl_list_remove(&cursor_button.link);
	wl_list_remove(&cursor_frame.link);
	wl_list_remove(&cursor_motion.link);
	wl_list_remove(&cursor_motion_absolute.link);
	wl_list_remove(&gpu_reset.link);
	wl_list_remove(&new_idle_inhibitor.link);
	wl_list_remove(&layout_change.link);
	wl_list_remove(&new_input_device.link);
	wl_list_remove(&new_virtual_keyboard.link);
	wl_list_remove(&new_virtual_pointer.link);
	wl_list_remove(&new_pointer_constraint.link);
	wl_list_remove(&new_output.link);
	wl_list_remove(&new_xdg_toplevel.link);
	wl_list_remove(&new_xdg_decoration.link);
	wl_list_remove(&new_xdg_popup.link);
	wl_list_remove(&new_layer_surface.link);
	wl_list_remove(&output_mgr_apply.link);
	wl_list_remove(&output_mgr_test.link);
	wl_list_remove(&output_power_mgr_set_mode.link);
	wl_list_remove(&request_activate.link);
	wl_list_remove(&request_cursor.link);
	wl_list_remove(&request_set_psel.link);
	wl_list_remove(&request_set_sel.link);
	wl_list_remove(&request_set_cursor_shape.link);
	wl_list_remove(&request_start_drag.link);
	wl_list_remove(&start_drag.link);
	wl_list_remove(&new_session_lock.link);
#ifdef XWAYLAND
	wl_list_remove(&new_xwayland_surface.link);
	wl_list_remove(&xwayland_ready.link);
#endif
}

void
closemon(Monitor *m)
{
	/* update selmon if needed and
	 * move closed monitor's clients to the focused one */
	Client *c;
	int i = 0, nmons = wl_list_length(&mons);
	if (!nmons) {
		selmon = NULL;
	} else if (m == selmon) {
		do /* don't switch to disabled mons */
			selmon = wl_container_of(mons.next, selmon, link);
		while (!selmon->wlr_output->enabled && i++ < nmons);

		if (!selmon->wlr_output->enabled)
			selmon = NULL;
	}

	wl_list_for_each(c, &clients, link) {
		if (c->isfloating && c->geom.x > m->m.width)
			resize(c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
					.width = c->geom.width, .height = c->geom.height}, 0);
		if (c->mon == m)
			setmon(c, selmon, c->tags);
	}
	focusclient(focustop(selmon), 1);
	printstatus();
}

void
commitlayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;
	struct wlr_scene_tree *scene_layer = layers[layermap[layer_surface->current.layer]];
	struct wlr_layer_surface_v1_state old_state;

	if (l->layer_surface->initial_commit) {
		client_set_scale(layer_surface->surface, l->mon->wlr_output->scale);

		/* Temporarily set the layer's current state to pending
		 * so that we can easily arrange it */
		old_state = l->layer_surface->current;
		l->layer_surface->current = l->layer_surface->pending;
		arrangelayers(l->mon);
		l->layer_surface->current = old_state;
		return;
	}

	if (layer_surface->current.committed == 0 && l->mapped == layer_surface->surface->mapped)
		return;
	l->mapped = layer_surface->surface->mapped;

	if (scene_layer != l->scene->node.parent) {
		wlr_scene_node_reparent(&l->scene->node, scene_layer);
		wl_list_remove(&l->link);
		wl_list_insert(&l->mon->layers[layer_surface->current.layer], &l->link);
		wlr_scene_node_reparent(&l->popups->node, (layer_surface->current.layer
				< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer));
	}

	arrangelayers(l->mon);
}

void
commitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, commit);

	if (c->surface.xdg->initial_commit) {
		/*
		 * Get the monitor this client will be rendered on
		 * Note that if the user set a rule in which the client is placed on
		 * a different monitor based on its title, this will likely select
		 * a wrong monitor.
		 */
		applyrules(c);
		if (c->mon) {
			client_set_scale(client_surface(c), c->mon->wlr_output->scale);
		}
		setmon(c, NULL, 0); /* Make sure to reapply rules in mapnotify() */

		wlr_xdg_toplevel_set_wm_capabilities(c->surface.xdg->toplevel,
				WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
		if (c->decoration)
			requestdecorationmode(&c->set_decoration_mode, c->decoration);
		wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, 0, 0);
		return;
	}

	/* mark a pending resize as completed */
	if (c->resize && c->resize <= c->surface.xdg->current.configure_serial) {
		c->resize = 0;
		c->pending_resize_w = c->pending_resize_h = -1;
	}

	resize(c, c->geom, (c->isfloating && !c->isfullscreen));
}

void
commitpopup(struct wl_listener *listener, void *data)
{
	struct wlr_surface *surface = data;
	struct wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
	LayerSurface *l = NULL;
	Client *c = NULL;
	struct wlr_box box;
	int type = -1;

	if (!popup->base->initial_commit)
		return;

	type = toplevel_from_wlr_surface(popup->base->surface, &c, &l);
	if (!popup->parent || type < 0)
		return;
	popup->base->surface->data = wlr_scene_xdg_surface_create(
			popup->parent->data, popup->base);
	if ((l && !l->mon) || (c && !c->mon)) {
		wlr_xdg_popup_destroy(popup);
		return;
	}
	box = type == LayerShell ? l->mon->m : c->mon->w;
	box.x -= (type == LayerShell ? l->scene->node.x : c->geom.x);
	box.y -= (type == LayerShell ? l->scene->node.y : c->geom.y);
	wlr_xdg_popup_unconstrain_from_box(popup, &box);
	wl_list_remove(&listener->link);
	free(listener);
}

void
createdecoration(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	Client *c = deco->toplevel->base->data;
	c->decoration = deco;

	LISTEN(&deco->events.request_mode, &c->set_decoration_mode, requestdecorationmode);
	LISTEN(&deco->events.destroy, &c->destroy_decoration, destroydecoration);

	requestdecorationmode(&c->set_decoration_mode, deco);
}

void
createidleinhibitor(struct wl_listener *listener, void *data)
{
	struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
	LISTEN_STATIC(&idle_inhibitor->events.destroy, destroyidleinhibitor);

	checkidleinhibitor(NULL);
}

void
createkeyboard(struct wlr_keyboard *keyboard)
{
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(keyboard, kb_group->wlr_group->keyboard.keymap);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(kb_group->wlr_group, keyboard);
}

KeyboardGroup *
createkeyboardgroup(void)
{
	KeyboardGroup *group = ecalloc(1, sizeof(*group));
	struct xkb_context *context;
	struct xkb_keymap *keymap;
	struct xkb_rule_names names;

	group->wlr_group = wlr_keyboard_group_create();
	group->wlr_group->data = group;

	/* Prepare an XKB keymap and assign it to the keyboard group. */
	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	names = getxkbrules();
	if (!(keymap = xkb_keymap_new_from_names(context, &names,
				XKB_KEYMAP_COMPILE_NO_FLAGS)))
		die("failed to compile keymap");

	wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

	wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard, repeat_rate, repeat_delay);

	/* Set up listeners for keyboard events */
	LISTEN(&group->wlr_group->keyboard.events.key, &group->key, keypress);
	LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers, keypressmod);

	group->key_repeat_source = wl_event_loop_add_timer(event_loop, keyrepeat, group);

	/* A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same wlr_keyboard_group, which provides a single wlr_keyboard interface for
	 * all of them. Set this combined wlr_keyboard as the seat keyboard.
	 */
	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	return group;
}

void
createlayersurface(struct wl_listener *listener, void *data)
{
	struct wlr_layer_surface_v1 *layer_surface = data;
	LayerSurface *l;
	struct wlr_surface *surface = layer_surface->surface;
	struct wlr_scene_tree *scene_layer = layers[layermap[layer_surface->pending.layer]];

	if (!layer_surface->output
			&& !(layer_surface->output = selmon ? selmon->wlr_output : NULL)) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	l = layer_surface->data = ecalloc(1, sizeof(*l));
	l->type = LayerShell;
	LISTEN(&surface->events.commit, &l->surface_commit, commitlayersurfacenotify);
	LISTEN(&surface->events.unmap, &l->unmap, unmaplayersurfacenotify);
	LISTEN(&layer_surface->events.destroy, &l->destroy, destroylayersurfacenotify);

	l->layer_surface = layer_surface;
	l->mon = layer_surface->output->data;
	l->scene_layer = wlr_scene_layer_surface_v1_create(scene_layer, layer_surface);
	l->scene = l->scene_layer->tree;
	l->popups = surface->data = wlr_scene_tree_create(layer_surface->current.layer
			< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer);
	l->scene->node.data = l->popups->node.data = l;

	wl_list_insert(&l->mon->layers[layer_surface->pending.layer],&l->link);
	wlr_surface_send_enter(surface, layer_surface->output);
}

void
createlocksurface(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	Monitor *m = lock_surface->output->data;
	struct wlr_scene_tree *scene_tree = lock_surface->surface->data
			= wlr_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
	m->lock_surface = lock_surface;

	wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
	wlr_session_lock_surface_v1_configure(lock_surface, m->m.width, m->m.height);

	LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface, destroylocksurface);

	if (m == selmon)
		client_notify_enter(lock_surface->surface, wlr_seat_get_keyboard(seat));
}

static struct wlr_output_mode *
bestmode(struct wlr_output *output)
{
	struct wlr_output_mode *mode, *best = NULL;
	int best_area = 0, best_refresh = 0;

	wl_list_for_each(mode, &output->modes, link) {
		int area = mode->width * mode->height;
		if (!best || area > best_area || (area == best_area && mode->refresh > best_refresh)) {
			best = mode;
			best_area = area;
			best_refresh = mode->refresh;
		}
	}

	if (!best)
		best = wlr_output_preferred_mode(output);

	return best;
}

static void
initstatusbar(Monitor *m)
{
	if (!m)
		return;

	wl_list_init(&m->statusbar.tray_menu.entries);
	if (!wifi_networks_initialized) {
		wl_list_init(&wifi_networks);
		wifi_networks_initialized = 1;
	}
	wl_list_init(&m->statusbar.net_menu.entries);
	wl_list_init(&m->statusbar.net_menu.networks);
	m->showbar = 1;
	m->statusbar.area = (struct wlr_box){0};
	m->statusbar.tree = wlr_scene_tree_create(layers[LyrTop]);
	if (m->statusbar.tree) {
		m->statusbar.tags.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.tags.tree) {
			m->statusbar.tags.bg = wlr_scene_tree_create(m->statusbar.tags.tree);
			m->statusbar.tags.hover_tag = -1;
			for (int i = 0; i < TAGCOUNT; i++)
				m->statusbar.tags.hover_alpha[i] = 0.0f;
		}
		m->statusbar.traylabel.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.traylabel.tree)
			m->statusbar.traylabel.bg = wlr_scene_tree_create(m->statusbar.traylabel.tree);
		m->statusbar.sysicons.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.sysicons.tree)
			m->statusbar.sysicons.bg = wlr_scene_tree_create(m->statusbar.sysicons.tree);
		m->statusbar.tray_menu.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.tray_menu.tree) {
			m->statusbar.tray_menu.bg = wlr_scene_tree_create(m->statusbar.tray_menu.tree);
			m->statusbar.tray_menu.visible = 0;
			wlr_scene_node_set_enabled(&m->statusbar.tray_menu.tree->node, 0);
		}
		m->statusbar.net_menu.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.net_menu.tree) {
			m->statusbar.net_menu.bg = wlr_scene_tree_create(m->statusbar.net_menu.tree);
			m->statusbar.net_menu.submenu_tree = wlr_scene_tree_create(m->statusbar.tree);
			if (m->statusbar.net_menu.submenu_tree)
				m->statusbar.net_menu.submenu_bg = wlr_scene_tree_create(m->statusbar.net_menu.submenu_tree);
			m->statusbar.net_menu.visible = 0;
			m->statusbar.net_menu.submenu_visible = 0;
			wlr_scene_node_set_enabled(&m->statusbar.net_menu.tree->node, 0);
			if (m->statusbar.net_menu.submenu_tree)
				wlr_scene_node_set_enabled(&m->statusbar.net_menu.submenu_tree->node, 0);
		}
		m->statusbar.cpu.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.cpu.tree)
			m->statusbar.cpu.bg = wlr_scene_tree_create(m->statusbar.cpu.tree);
	m->statusbar.net.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.net.tree) {
		m->statusbar.net.bg = wlr_scene_tree_create(m->statusbar.net.tree);
	}
	m->statusbar.battery.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.battery.tree)
		m->statusbar.battery.bg = wlr_scene_tree_create(m->statusbar.battery.tree);
	m->statusbar.light.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.light.tree)
		m->statusbar.light.bg = wlr_scene_tree_create(m->statusbar.light.tree);
	m->statusbar.mic.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.mic.tree)
		m->statusbar.mic.bg = wlr_scene_tree_create(m->statusbar.mic.tree);
	m->statusbar.volume.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.volume.tree)
		m->statusbar.volume.bg = wlr_scene_tree_create(m->statusbar.volume.tree);
	m->statusbar.ram.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.ram.tree)
		m->statusbar.ram.bg = wlr_scene_tree_create(m->statusbar.ram.tree);
	m->statusbar.clock.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.clock.tree)
		m->statusbar.clock.bg = wlr_scene_tree_create(m->statusbar.clock.tree);
	m->statusbar.cpu_popup.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.cpu_popup.tree) {
		m->statusbar.cpu_popup.bg = wlr_scene_tree_create(m->statusbar.cpu_popup.tree);
		m->statusbar.cpu_popup.visible = 0;
		m->statusbar.cpu_popup.hover_idx = -1;
		m->statusbar.cpu_popup.refresh_data = 1;
		wlr_scene_node_set_enabled(&m->statusbar.cpu_popup.tree->node, 0);
	}
		m->statusbar.net_popup.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.net_popup.tree) {
			m->statusbar.net_popup.bg = wlr_scene_tree_create(m->statusbar.net_popup.tree);
			m->statusbar.net_popup.visible = 0;
			wlr_scene_node_set_enabled(&m->statusbar.net_popup.tree->node, 0);
		}
	}
	if (!m->modal.tree) {
		m->modal.tree = wlr_scene_tree_create(layers[LyrTop]);
		if (m->modal.tree) {
			m->modal.bg = wlr_scene_tree_create(m->modal.tree);
			m->modal.visible = 0;
			m->modal.active_idx = -1;
			for (int i = 0; i < 3; i++) {
				m->modal.search[i][0] = '\0';
				m->modal.search_len[i] = 0;
				m->modal.result_count[i] = 0;
				for (int j = 0; j < (int)LENGTH(m->modal.results[i]); j++)
					m->modal.results[i][j][0] = '\0';
				m->modal.selected[i] = -1;
				m->modal.scroll[i] = 0;
			}
				for (int i = 0; i < (int)LENGTH(m->modal.file_results_path); i++) {
					m->modal.file_results_name[i][0] = '\0';
					m->modal.file_results_path[i][0] = '\0';
					m->modal.file_results_mtime[i] = 0;
				}
			m->modal.file_search_pid = -1;
	m->modal.file_search_fd = -1;
	m->modal.file_search_event = NULL;
	m->modal.file_search_timer = NULL;
	m->modal.file_search_len = 0;
	m->modal.file_search_buf[0] = '\0';
	m->modal.file_search_last[0] = '\0';
			wlr_scene_node_set_enabled(&m->modal.tree->node, 0);
		}
	}
}

void
createmon(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct wlr_output *wlr_output = data;
	const MonitorRule *r;
	size_t i;
	struct wlr_output_state state;
	struct wlr_output_mode *mode;
	Monitor *m;
	Client *c;

	if (!wlr_output_init_render(wlr_output, alloc, drw))
		return;

	m = wlr_output->data = ecalloc(1, sizeof(*m));
	m->wlr_output = wlr_output;

	for (i = 0; i < LENGTH(m->layers); i++)
	wl_list_init(&m->layers[i]);
	initstatusbar(m);

	wlr_output_state_init(&state);
	/* Initialize monitor state using configured rules */
	m->gaps = gaps;

	m->tagset[0] = m->tagset[1] = 1;
	for (r = monrules; r < END(monrules); r++) {
		if (!r->name || strstr(wlr_output->name, r->name)) {
			m->m.x = r->x;
			m->m.y = r->y;
			m->mfact = r->mfact;
			m->nmaster = r->nmaster;
			m->lt[0] = r->lt;
			m->lt[1] = &layouts[LENGTH(layouts) > 1 && r->lt != &layouts[1]];
			strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));
			wlr_output_state_set_scale(&state, r->scale);
			wlr_output_state_set_transform(&state, r->rr);
			break;
		}
	}

	/* The mode is a tuple of (width, height, refresh rate), and each
	 * monitor supports only a specific set of modes. Pick the highest
	 * resolution and refresh rate available. */
	if ((mode = bestmode(wlr_output)))
		wlr_output_state_set_mode(&state, mode);

	/* Set up event listeners */
	LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
	LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);
	LISTEN(&wlr_output->events.request_state, &m->request_state, requestmonstate);

	wlr_output_state_set_enabled(&state, 1);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	wl_list_insert(&mons, &m->link);
	printstatus();
	init_tree(m);
	modal_prewarm(m);

	/* The xdg-protocol specifies:
	 *
	 * If the fullscreened surface is not opaque, the compositor must make
	 * sure that other screen content not part of the same surface tree (made
	 * up of subsurfaces, popups or similarly coupled surfaces) are not
	 * visible below the fullscreened surface.
	 *
	 */
	/* updatemons() will resize and set correct position */
	m->fullscreen_bg = wlr_scene_rect_create(layers[LyrFS], 0, 0, fullscreen_bg);
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

	/* Adds this to the output layout in the order it was configured.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	m->scene_output = wlr_scene_output_create(scene, wlr_output);
	if (m->m.x == -1 && m->m.y == -1)
		wlr_output_layout_add_auto(output_layout, wlr_output);
	else
		wlr_output_layout_add(output_layout, wlr_output, m->m.x, m->m.y);

	wl_list_for_each(c, &clients, link) {
		if (c->output && strcmp(wlr_output->name, c->output) == 0)
			c->mon = m;
	}
	updatemons(NULL, NULL);
}

void
createnotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client creates a new toplevel (application window). */
	struct wlr_xdg_toplevel *toplevel = data;
	Client *c = NULL;

	/* Allocate a Client for this surface */
	c = toplevel->base->data = ecalloc(1, sizeof(*c));
	c->surface.xdg = toplevel->base;
	c->bw = borderpx;

	LISTEN(&toplevel->base->surface->events.commit, &c->commit, commitnotify);
	LISTEN(&toplevel->base->surface->events.map, &c->map, mapnotify);
	LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&toplevel->events.destroy, &c->destroy, destroynotify);
	LISTEN(&toplevel->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&toplevel->events.request_maximize, &c->maximize, maximizenotify);
	LISTEN(&toplevel->events.set_title, &c->set_title, updatetitle);
}

void
createpointer(struct wlr_pointer *pointer)
{
	struct libinput_device *device;
	if (wlr_input_device_is_libinput(&pointer->base)
			&& (device = wlr_libinput_get_device_handle(&pointer->base))) {

		if (libinput_device_config_tap_get_finger_count(device)) {
			libinput_device_config_tap_set_enabled(device, tap_to_click);
			libinput_device_config_tap_set_drag_enabled(device, tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(device, drag_lock);
			libinput_device_config_tap_set_button_map(device, button_map);
		}

		if (libinput_device_config_scroll_has_natural_scroll(device))
			libinput_device_config_scroll_set_natural_scroll_enabled(device, natural_scrolling);

		if (libinput_device_config_dwt_is_available(device))
			libinput_device_config_dwt_set_enabled(device, disable_while_typing);

		if (libinput_device_config_left_handed_is_available(device))
			libinput_device_config_left_handed_set(device, left_handed);

		if (libinput_device_config_middle_emulation_is_available(device))
			libinput_device_config_middle_emulation_set_enabled(device, middle_button_emulation);

		if (libinput_device_config_scroll_get_methods(device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method(device, scroll_method);

		if (libinput_device_config_click_get_methods(device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
			libinput_device_config_click_set_method(device, click_method);

		if (libinput_device_config_send_events_get_modes(device))
			libinput_device_config_send_events_set_mode(device, send_events_mode);

		if (libinput_device_config_accel_is_available(device)) {
			libinput_device_config_accel_set_profile(device, accel_profile);
			libinput_device_config_accel_set_speed(device, accel_speed);
		}
	}

	wlr_cursor_attach_input_device(cursor, &pointer->base);
}

void
createpointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = ecalloc(1, sizeof(*pointer_constraint));
	pointer_constraint->constraint = data;
	LISTEN(&pointer_constraint->constraint->events.destroy,
			&pointer_constraint->destroy, destroypointerconstraint);
}

void
createpopup(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client (either xdg-shell or layer-shell)
	 * creates a new popup. */
	struct wlr_xdg_popup *popup = data;
	LISTEN_STATIC(&popup->base->surface->events.commit, commitpopup);
}

void
cursorconstrain(struct wlr_pointer_constraint_v1 *constraint)
{
	if (active_constraint == constraint)
		return;

	if (active_constraint)
		wlr_pointer_constraint_v1_send_deactivated(active_constraint);

	active_constraint = constraint;
	wlr_pointer_constraint_v1_send_activated(constraint);
}

void
cursorframe(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(seat);
}

void
cursorwarptohint(void)
{
	Client *c = NULL;
	double sx = active_constraint->current.cursor_hint.x;
	double sy = active_constraint->current.cursor_hint.y;

	toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
	if (c && active_constraint->current.cursor_hint.enabled) {
		wlr_cursor_warp(cursor, NULL, sx + c->geom.x + c->bw, sy + c->geom.y + c->bw);
		wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
	}
}

void
destroydecoration(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, destroy_decoration);

	wl_list_remove(&c->destroy_decoration.link);
	wl_list_remove(&c->set_decoration_mode.link);
}

void
destroydragicon(struct wl_listener *listener, void *data)
{
	/* Focus enter isn't sent during drag, so refocus the focused node. */
	focusclient(focustop(selmon), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
	wl_list_remove(&listener->link);
	free(listener);
}

void
destroyidleinhibitor(struct wl_listener *listener, void *data)
{
	/* `data` is the wlr_surface of the idle inhibitor being destroyed,
	 * at this point the idle inhibitor is still in the list of the manager */
	checkidleinhibitor(wlr_surface_get_root_surface(data));
	wl_list_remove(&listener->link);
	free(listener);
}

void
destroylayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, destroy);

	wl_list_remove(&l->link);
	wl_list_remove(&l->destroy.link);
	wl_list_remove(&l->unmap.link);
	wl_list_remove(&l->surface_commit.link);
	wlr_scene_node_destroy(&l->scene->node);
	wlr_scene_node_destroy(&l->popups->node);
	free(l);
}

void
destroylock(SessionLock *lock, int unlock)
{
	wlr_seat_keyboard_notify_clear_focus(seat);
	if ((locked = !unlock))
		goto destroy;

	wlr_scene_node_set_enabled(&locked_bg->node, 0);

	focusclient(focustop(selmon), 0);
	motionnotify(0, NULL, 0, 0, 0, 0);

destroy:
	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);

	wlr_scene_node_destroy(&lock->scene->node);
	cur_lock = NULL;
	free(lock);
}

void
destroylocksurface(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
	struct wlr_session_lock_surface_v1 *surface, *lock_surface = m->lock_surface;

	m->lock_surface = NULL;
	wl_list_remove(&m->destroy_lock_surface.link);

	if (lock_surface->surface != seat->keyboard_state.focused_surface)
		return;

	if (locked && cur_lock && !wl_list_empty(&cur_lock->surfaces)) {
		surface = wl_container_of(cur_lock->surfaces.next, surface, link);
		client_notify_enter(surface->surface, wlr_seat_get_keyboard(seat));
	} else if (!locked) {
		focusclient(focustop(selmon), 1);
	} else {
		wlr_seat_keyboard_clear_focus(seat);
	}
}

void
destroynotify(struct wl_listener *listener, void *data)
{
	/* Called when the xdg_toplevel is destroyed. */
	Client *c = wl_container_of(listener, c, destroy);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->fullscreen.link);
	/* We check if the destroyed client was part of any tiled_list, to catch
	 * client removals even if they would not be currently managed by btrtile */
	if (selmon && selmon->root)
		remove_client(selmon, c);
#ifdef XWAYLAND
	if (c->type != XDGShell) {
		wl_list_remove(&c->activate.link);
		wl_list_remove(&c->associate.link);
		wl_list_remove(&c->configure.link);
		wl_list_remove(&c->minimize.link);
		wl_list_remove(&c->dissociate.link);
		wl_list_remove(&c->set_hints.link);
	} else
#endif
	{
		wl_list_remove(&c->commit.link);
		wl_list_remove(&c->map.link);
		wl_list_remove(&c->unmap.link);
		wl_list_remove(&c->maximize.link);
	}
	free(c);
}

void
destroypointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = wl_container_of(listener, pointer_constraint, destroy);

	if (active_constraint == pointer_constraint->constraint) {
		cursorwarptohint();
		active_constraint = NULL;
	}

	wl_list_remove(&pointer_constraint->destroy.link);
	free(pointer_constraint);
}

void
destroysessionlock(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, destroy);
	destroylock(lock, 0);
}

void
destroykeyboardgroup(struct wl_listener *listener, void *data)
{
	KeyboardGroup *group = wl_container_of(listener, group, destroy);
	wl_event_source_remove(group->key_repeat_source);
	wl_list_remove(&group->key.link);
	wl_list_remove(&group->modifiers.link);
	wl_list_remove(&group->destroy.link);
	wlr_keyboard_group_destroy(group->wlr_group);
	free(group);
}

Monitor *
dirtomon(enum wlr_direction dir)
{
	struct wlr_output *next;
	if (!wlr_output_layout_get(output_layout, selmon->wlr_output))
		return selmon;
	if ((next = wlr_output_layout_adjacent_output(output_layout,
			dir, selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	if ((next = wlr_output_layout_farthest_output(output_layout,
			dir ^ (WLR_DIRECTION_LEFT|WLR_DIRECTION_RIGHT),
			selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	return selmon;
}

void
focusclient(Client *c, int lift)
{
	struct wlr_surface *old = seat->keyboard_state.focused_surface;
	int unused_lx, unused_ly, old_client_type;
	Client *old_c = NULL;
	LayerSurface *old_l = NULL;

	if (locked)
		return;

	/* Warp cursor to center of client if it is outside */
	if (lift)
		warpcursor(c);

	/* Raise client in stacking order if requested */
	if (c && lift)
		wlr_scene_node_raise_to_top(&c->scene->node);

	if (c && client_surface(c) == old)
		return;

	if ((old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l)) == XDGShell) {
		struct wlr_xdg_popup *popup, *tmp;
		wl_list_for_each_safe(popup, tmp, &old_c->surface.xdg->popups, link)
			wlr_xdg_popup_destroy(popup);
	}

	/* Put the new client atop the focus stack and select its monitor */
	if (c && !client_is_unmanaged(c)) {
		wl_list_remove(&c->flink);
		wl_list_insert(&fstack, &c->flink);
		selmon = c->mon;
		c->isurgent = 0;

		/* Don't change border color if there is an exclusive focus or we are
		 * handling a drag operation */
		if (!exclusive_focus && !seat->drag)
			client_set_border_color(c, focuscolor);
	}

	/* Deactivate old client if focus is changing */
	if (old && (!c || client_surface(c) != old)) {
		/* If an overlay is focused, don't focus or activate the client,
		 * but only update its position in fstack to render its border with focuscolor
		 * and focus it after the overlay is closed. */
		if (old_client_type == LayerShell && wlr_scene_node_coords(
					&old_l->scene->node, &unused_lx, &unused_ly)
				&& old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
			return;
		} else if (old_c && old_c == exclusive_focus && client_wants_focus(old_c)) {
			return;
		/* Don't deactivate old client if the new one wants focus, as this causes issues with winecfg
		 * and probably other clients */
		} else if (old_c && !client_is_unmanaged(old_c) && (!c || !client_wants_focus(c))) {
			client_set_border_color(old_c, bordercolor);

			client_activate_surface(old, 0);
		}
	}
	printstatus();

	if (!c) {
		/* With no client, all we have left is to clear focus */
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}

	/* Change cursor surface */
	motionnotify(0, NULL, 0, 0, 0, 0);

	/* Have a client, so focus its top-level wlr_surface */
	client_notify_enter(client_surface(c), wlr_seat_get_keyboard(seat));

	/* Activate the new client */
	client_activate_surface(client_surface(c), 1);
}

void
focusmon(const Arg *arg)
{
	int i = 0, nmons = wl_list_length(&mons);
	if (nmons) {
		do /* don't switch to disabled mons */
			selmon = dirtomon(arg->i);
		while (!selmon->wlr_output->enabled && i++ < nmons);
	}
	focusclient(focustop(selmon), 1);
}

void
focusstack(const Arg *arg)
{
	/* Focus the next or previous client (in tiling order) on selmon */
	Client *c, *sel = focustop(selmon);
	if (!sel || (sel->isfullscreen && !client_has_children(sel)))
		return;
	if (arg->i > 0) {
		wl_list_for_each(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break; /* found it */
		}
	} else {
		wl_list_for_each_reverse(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break; /* found it */
		}
	}
	/* If only one client is visible on selmon, then c == sel */
	focusclient(c, 1);
}

/* We probably should change the name of this: it sounds like it
 * will focus the topmost client of this mon, when actually will
 * only return that client */
Client *
focustop(Monitor *m)
{
	Client *c;
	wl_list_for_each(c, &fstack, flink) {
		if (VISIBLEON(c, m))
			return c;
	}
	return NULL;
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, fullscreen);
	setfullscreen(c, client_wants_fullscreen(c));
}

void
gpureset(struct wl_listener *listener, void *data)
{
	struct wlr_renderer *old_drw = drw;
	struct wlr_allocator *old_alloc = alloc;
	struct Monitor *m;
	if (!(drw = wlr_renderer_autocreate(backend)))
		die("couldn't recreate renderer");

	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't recreate allocator");

	wl_list_remove(&gpu_reset.link);
	wl_signal_add(&drw->events.lost, &gpu_reset);

	wlr_compositor_set_renderer(compositor, drw);

	wl_list_for_each(m, &mons, link) {
		wlr_output_init_render(m->wlr_output, alloc, drw);
	}

	wlr_allocator_destroy(old_alloc);
	wlr_renderer_destroy(old_drw);
}

void
handlesig(int signo)
{
	if (signo == SIGCHLD)
		while (waitpid(-1, NULL, WNOHANG) > 0);
	else if (signo == SIGINT || signo == SIGTERM)
		quit(NULL);
}

void
incnmaster(const Arg *arg)
{
	if (!arg || !selmon)
		return;
	selmon->nmaster = MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

void
inputdevice(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct wlr_input_device *device = data;
	uint32_t caps;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		createkeyboard(wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		createpointer(wlr_pointer_from_input_device(device));
		break;
	default:
		/* TODO handle other input device types */
		break;
	}

	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In dwl we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	/* TODO do we actually require a cursor? */
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&kb_group->wlr_group->devices))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(seat, caps);
}

int
keybinding(uint32_t mods, xkb_keysym_t sym)
{
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 */
	const Key *k;
	for (k = keys; k < END(keys); k++) {
		if (CLEANMASK(mods) == CLEANMASK(k->mod)
				&& sym == k->keysym && k->func) {
			k->func(&k->arg);
			return 1;
		}
	}
	return 0;
}

void
keypress(struct wl_listener *listener, void *data)
{
	int i;
	/* This event is raised when a key is pressed or released. */
	KeyboardGroup *group = wl_container_of(listener, group, key);
	struct wlr_keyboard_key_event *event = data;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			group->wlr_group->keyboard.xkb_state, keycode, &syms);

	int handled = 0;
	uint32_t mods = wlr_keyboard_get_modifiers(&group->wlr_group->keyboard);
	Monitor *modal_mon = modal_visible_monitor();

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	/* On _press_ if there is no active screen locker,
	 * attempt to process a compositor keybinding. */
	if (!locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (modal_mon) {
			int consumed = 0;
			for (i = 0; i < nsyms; i++) {
				if (modal_handle_key(modal_mon, mods, syms[i])) {
					handled = 1;
					consumed = 1;
				}
			}
			if (consumed) {
				modal_update_results(modal_mon);
				modal_render(modal_mon);
			}
		}
		for (i = 0; i < nsyms; i++)
			handled = keybinding(mods, syms[i]) || handled;
	}

	if (handled && group->wlr_group->keyboard.repeat_info.delay > 0) {
		group->mods = mods;
		group->keysyms = syms;
		group->nsyms = nsyms;
		wl_event_source_timer_update(group->key_repeat_source,
				group->wlr_group->keyboard.repeat_info.delay);
	} else {
		group->nsyms = 0;
		wl_event_source_timer_update(group->key_repeat_source, 0);
	}

	if (handled)
		return;

	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Pass unhandled keycodes along to the client. */
	wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
}

void
keypressmod(struct wl_listener *listener, void *data)
{
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	KeyboardGroup *group = wl_container_of(listener, group, modifiers);

	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(seat,
			&group->wlr_group->keyboard.modifiers);
}

int
keyrepeat(void *data)
{
	KeyboardGroup *group = data;
	Monitor *modal_mon = modal_visible_monitor();
	int modal_consumed = 0;
	int i;
	if (!group->nsyms || group->wlr_group->keyboard.repeat_info.rate <= 0)
		return 0;

	wl_event_source_timer_update(group->key_repeat_source,
			1000 / group->wlr_group->keyboard.repeat_info.rate);

	for (i = 0; i < group->nsyms; i++) {
		if (modal_mon && modal_handle_key(modal_mon, group->mods, group->keysyms[i]))
			modal_consumed = 1;
		else
			keybinding(group->mods, group->keysyms[i]);
	}

	if (modal_consumed) {
		modal_update_results(modal_mon);
		modal_render(modal_mon);
	}

	return 0;
}

void
killclient(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		client_send_close(sel);
}

void
locksession(struct wl_listener *listener, void *data)
{
	struct wlr_session_lock_v1 *session_lock = data;
	SessionLock *lock;
	wlr_scene_node_set_enabled(&locked_bg->node, 1);
	if (cur_lock) {
		wlr_session_lock_v1_destroy(session_lock);
		return;
	}
	lock = session_lock->data = ecalloc(1, sizeof(*lock));
	focusclient(NULL, 0);

	lock->scene = wlr_scene_tree_create(layers[LyrBlock]);
	cur_lock = lock->lock = session_lock;
	locked = 1;

	LISTEN(&session_lock->events.new_surface, &lock->new_surface, createlocksurface);
	LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
	LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

	wlr_session_lock_v1_send_locked(session_lock);
}

void
mapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is mapped, or ready to display on-screen. */
	Client *p = NULL;
	Client *w, *c = wl_container_of(listener, c, map);
	Monitor *m;
	int i;

	/* Create scene tree for this client and its border */
	c->scene = client_surface(c)->data = wlr_scene_tree_create(layers[LyrTile]);
	/* Enabled later by a call to arrange() */
	wlr_scene_node_set_enabled(&c->scene->node, client_is_unmanaged(c));
	c->scene_surface = c->type == XDGShell
			? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
			: wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
	c->scene->node.data = c->scene_surface->node.data = c;

	client_get_geometry(c, &c->geom);

	/* Handle unmanaged clients first so we can return prior create borders */
	if (client_is_unmanaged(c)) {
		/* Unmanaged clients always are floating */
		wlr_scene_node_reparent(&c->scene->node, layers[LyrFloat]);
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		client_set_size(c, c->geom.width, c->geom.height);
		if (client_wants_focus(c)) {
			focusclient(c, 1);
			exclusive_focus = c;
		}
		goto unset_fullscreen;
	}

	for (i = 0; i < 4; i++) {
		c->border[i] = wlr_scene_rect_create(c->scene, 0, 0,
				c->isurgent ? urgentcolor : bordercolor);
		c->border[i]->node.data = c;
	}

	/* Initialize client geometry with room for border */
	client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	c->geom.width += 2 * c->bw;
	c->geom.height += 2 * c->bw;

	/* Insert this client into client lists. */
	wl_list_insert(&clients, &c->link);
	wl_list_insert(&fstack, &c->flink);

	/* Set initial monitor, tags, floating status, and focus:
	 * we always consider floating, clients that have parent and thus
	 * we set the same tags and monitor as its parent.
	 * If there is no parent, apply rules */
	if ((p = client_get_parent(c))) {
		c->isfloating = 1;
		if (tray_anchor_time_ms && monotonic_msec() - tray_anchor_time_ms <= 1000) {
			c->geom.x = tray_anchor_x - c->geom.width / 2;
			c->geom.y = tray_anchor_y;
		} else if (p->mon) {
			c->geom.x = (p->mon->w.width - c->geom.width) / 2 + p->mon->m.x;
			c->geom.y = (p->mon->w.height - c->geom.height) / 2 + p->mon->m.y;
		}
		setmon(c, p->mon, p->tags);
	} else {
		applyrules(c);
		if (tray_anchor_time_ms && monotonic_msec() - tray_anchor_time_ms <= 1000) {
			Monitor *am = xytomon(tray_anchor_x, tray_anchor_y);
			if (!am)
				am = selmon;
			if (am) {
				c->isfloating = 1;
				c->geom.x = tray_anchor_x - c->geom.width / 2;
				c->geom.y = tray_anchor_y;
				setmon(c, am, c->tags);
			}
		}
	}
	free(c->output);
	c->output = strdup(c->mon->wlr_output->name);
	if (c->output == NULL)
		die("oom");
	printstatus();

unset_fullscreen:
	m = c->mon ? c->mon : xytomon(c->geom.x, c->geom.y);
	wl_list_for_each(w, &clients, link) {
		if (w != c && w != p && w->isfullscreen && m == w->mon && (w->tags & c->tags))
			setfullscreen(w, 0);
	}
}

void
maximizenotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on
	 * client-side decorations. dwl doesn't support maximization, but
	 * to conform to xdg-shell protocol we still must send a configure.
	 * Since xdg-shell protocol v5 we should ignore request of unsupported
	 * capabilities, just schedule a empty configure when the client uses <5
	 * protocol version
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply. */
	Client *c = wl_container_of(listener, c, maximize);
	if (c->surface.xdg->initialized
			&& wl_resource_get_version(c->surface.xdg->toplevel->resource)
					< XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
		wlr_xdg_surface_schedule_configure(c->surface.xdg);
}

void
monocle(Monitor *m)
{
	Client *c;
	int n = 0;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		resize(c, m->w, 0);
		n++;
	}
	if (n)
		snprintf(m->ltsymbol, LENGTH(m->ltsymbol), "[%d]", n);
	if ((c = focustop(m)))
		wlr_scene_node_raise_to_top(&c->scene->node);
}

void
motionabsolute(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. Also, some hardware emits these events. */
	struct wlr_pointer_motion_absolute_event *event = data;
	double lx, ly, dx, dy;

	if (!event->time_msec) /* this is 0 with virtual pointers */
		wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);

	wlr_cursor_absolute_to_layout_coords(cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
	dx = lx - cursor->x;
	dy = ly - cursor->y;
	last_pointer_motion_ms = monotonic_msec();
	motionnotify(event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

static int __attribute__((unused))
node_contains_client(LayoutNode *node, Client *c)
{
	if (!node)
		return 0;
	if (node->is_client_node)
		return node->client == c;
	return node_contains_client(node->left, c) || node_contains_client(node->right, c);
}

static int
subtree_bounds(LayoutNode *node, Monitor *m, struct wlr_box *out)
{
	struct wlr_box lbox, rbox;
	int hasl, hasr, right, bottom;
	struct wlr_box tmp;

	if (!out)
		out = &tmp;
	if (!node)
		return 0;
	if (node->is_client_node) {
		Client *c = node->client;
		if (!c || !VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			return 0;
		*out = c->geom;
		return 1;
	}

	hasl = subtree_bounds(node->left, m, &lbox);
	hasr = subtree_bounds(node->right, m, &rbox);
	if (!hasl && !hasr)
		return 0;
	if (hasl && !hasr) {
		*out = lbox;
		return 1;
	}
	if (!hasl && hasr) {
		*out = rbox;
		return 1;
	}

	out->x = MIN(lbox.x, rbox.x);
	out->y = MIN(lbox.y, rbox.y);
	right = MAX(lbox.x + lbox.width, rbox.x + rbox.width);
	bottom = MAX(lbox.y + lbox.height, rbox.y + rbox.height);
	out->width = right - out->x;
	out->height = bottom - out->y;
	return 1;
}

static __attribute__((unused)) LayoutNode *
ancestor_split(LayoutNode *node, int want_vert)
{
	if (!node)
		return NULL;
	node = node->split_node;
	while (node) {
		if (!node->is_client_node && node->is_split_vertically == (unsigned int)want_vert)
			return node;
		node = node->split_node;
	}
	return NULL;
}

static LayoutNode *
closest_split_node(LayoutNode *client_node, int want_vert, double pointer,
		double *out_ratio, struct wlr_box *out_box, double *out_dist)
{
	LayoutNode *n = client_node ? client_node->split_node : NULL;
	LayoutNode *best = NULL;
	double best_dist = HUGE_VAL;
	struct wlr_box box;

	while (n) {
		if (!n->is_client_node && n->is_split_vertically == (unsigned int)want_vert) {
			if (subtree_bounds(n, selmon, &box)) {
				double ratio = n->split_ratio;
				double div = want_vert ? box.x + box.width * ratio
						: box.y + box.height * ratio;
				double dist = fabs(pointer - div);

				if (dist < best_dist) {
					best_dist = dist;
					best = n;
					if (out_ratio)
						*out_ratio = ratio;
					if (out_box)
						*out_box = box;
				}
			}
		}
		n = n->split_node;
	}

	if (out_dist)
		*out_dist = best ? best_dist : HUGE_VAL;
	return best;
}

static void
apply_resize_axis_choice(void)
{
	resize_use_v = resize_split_node != NULL;
	resize_use_h = resize_split_node_h != NULL;
}

static int
resize_should_update(uint32_t time)
{
	uint32_t elapsed;
	double dist;

	if (!resizing_from_mouse || time == 0 || resize_interval_ms == 0)
		return 1;

	if (resize_last_time == 0) {
		resize_last_time = time;
		resize_last_x = cursor->x;
		resize_last_y = cursor->y;
		return 1;
	}

	elapsed = time - resize_last_time;
	dist = fabs(cursor->x - resize_last_x) + fabs(cursor->y - resize_last_y);

	if (elapsed < resize_interval_ms && dist < resize_min_pixels)
		return 0;

	resize_last_time = time;
	resize_last_x = cursor->x;
	resize_last_y = cursor->y;
	return 1;
}

static const char *
resize_cursor_from_dirs(int dx, int dy)
{
	if (dx == 0 && dy == -1)
		return "n-resize";
	if (dx == 0 && dy == 1)
		return "s-resize";
	if (dx == -1 && dy == 0)
		return "w-resize";
	if (dx == 1 && dy == 0)
		return "e-resize";
	if (dx == -1 && dy == -1)
		return "nw-resize";
	if (dx == 1 && dy == -1)
		return "ne-resize";
	if (dx == -1 && dy == 1)
		return "sw-resize";
	if (dx == 1 && dy == 1)
		return "se-resize";
	return "default";
}

static const char *
pick_resize_handle(const Client *c, double cx, double cy)
{
	double left = cx - c->geom.x;
	double right = c->geom.x + c->geom.width - cx;
	double top = cy - c->geom.y;
	double bottom = c->geom.y + c->geom.height - cy;
	double corner_thresh = MIN(24.0, MIN(c->geom.width, c->geom.height) / 3.0);
	int hx = (left <= right) ? -1 : 1;
	int hy = (top <= bottom) ? -1 : 1;

	resize_dir_x = 0;
	resize_dir_y = 0;
	resize_use_v = resize_use_h = 0;

	if (MIN(left, right) <= corner_thresh && MIN(top, bottom) <= corner_thresh) {
		/* Close to a corner: resize both axes. */
		resize_dir_x = hx;
		resize_dir_y = hy;
	} else {
		if (MIN(left, right) <= corner_thresh)
			resize_dir_x = hx;
		if (MIN(top, bottom) <= corner_thresh)
			resize_dir_y = hy;
		if (!resize_dir_x && !resize_dir_y) {
			/* Otherwise pick the nearest axis so resize still happens. */
			if (MIN(left, right) <= MIN(top, bottom))
				resize_dir_x = hx;
			else
				resize_dir_y = hy;
		}
	}

	return resize_cursor_from_dirs(resize_dir_x, resize_dir_y);
}

void
motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
		double dx_unaccel, double dy_unaccel)
{
	int tiled = 0;
	double sx = 0, sy = 0, sx_confined, sy_confined;
	Client *c = NULL, *w = NULL;
	LayerSurface *l = NULL;
	struct wlr_surface *surface = NULL;
	struct wlr_pointer_constraint_v1 *constraint;

	/* Find the client under the pointer and send the event along. */
	xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

	if (cursor_mode == CurPressed && !seat->drag
			&& surface != seat->pointer_state.focused_surface
			&& toplevel_from_wlr_surface(seat->pointer_state.focused_surface, &w, &l) >= 0) {
		c = w;
		surface = seat->pointer_state.focused_surface;
		sx = cursor->x - (l ? l->scene->node.x : w->geom.x);
		sy = cursor->y - (l ? l->scene->node.y : w->geom.y);
	}

	/* time is 0 in internal calls meant to restore pointer focus. */
	if (time) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
				relative_pointer_mgr, seat, (uint64_t)time * 1000,
				dx, dy, dx_unaccel, dy_unaccel);

		wl_list_for_each(constraint, &pointer_constraints->constraints, link)
			cursorconstrain(constraint);

		if (active_constraint && cursor_mode != CurResize && cursor_mode != CurMove) {
			toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
			if (c && active_constraint->surface == seat->pointer_state.focused_surface) {
				sx = cursor->x - c->geom.x - c->bw;
				sy = cursor->y - c->geom.y - c->bw;
				if (wlr_region_confine(&active_constraint->region, sx, sy,
						sx + dx, sy + dy, &sx_confined, &sy_confined)) {
					dx = sx_confined - sx;
					dy = sy_confined - sy;
				}

				if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
					return;
			}
		}

		wlr_cursor_move(cursor, device, dx, dy);
		wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

		/* Update selmon (even while dragging a window) */
		if (sloppyfocus)
			selmon = xytomon(cursor->x, cursor->y);
	}

	/* Update drag icon's position */
	wlr_scene_node_set_position(&drag_icon->node, (int)round(cursor->x), (int)round(cursor->y));

	/* Hover feedback for tag boxes */
		if (selmon && selmon->showbar) {
			updatetaghover(selmon, cursor->x, cursor->y);
			updatecpuhover(selmon, cursor->x, cursor->y);
			updatenethover(selmon, cursor->x, cursor->y);
			net_menu_update_hover(selmon, cursor->x, cursor->y);
			if (!selmon->statusbar.net_menu.visible)
				net_menu_hide_all();
		}

		/* Skip if internal call or already resizing */
		if (time == 0 && resizing_from_mouse)
			goto focus;

	tiled = grabc && !grabc->isfloating && !grabc->isfullscreen;
	last_pointer_motion_ms = monotonic_msec();
	if (cursor_mode == CurMove) {
		/* Move the grabbed client to the new position. */
		if (grabc && grabc->isfloating) {
			resize(grabc, (struct wlr_box){
				.x = (int)round(cursor->x) - grabcx,
				.y = (int)round(cursor->y) - grabcy,
				.width = grabc->geom.width,
				.height = grabc->geom.height
			}, 1);
			return;
		}
	} else if (cursor_mode == CurResize) {
			if (!resize_should_update(time))
				goto focus;

			if (tiled && resizing_from_mouse) {
				double ratio, ratio_h;
				int changed = 0;
				double dx_total = cursor->x - resize_start_x;
				double dy_total = cursor->y - resize_start_y;
				LayoutNode *client_node;

				if (!resize_split_node && !resize_split_node_h) {
					client_node = find_client_node(selmon->root, grabc);
					if (!client_node)
						goto focus;

					resize_use_v = resize_use_h = 0;

					resize_split_node = closest_split_node(client_node, 1, cursor->x,
							&resize_start_ratio_v, &resize_start_box_v, NULL);
					if (!resize_split_node)
						resize_start_ratio_v = 0.5;

					resize_split_node_h = closest_split_node(client_node, 0, cursor->y,
							&resize_start_ratio_h, &resize_start_box_h, NULL);
					if (!resize_split_node_h)
						resize_start_ratio_h = 0.5;

					apply_resize_axis_choice();
				}

				if (resize_use_v && resize_split_node && resize_start_box_v.width > 0) {
					double current = resize_split_node->split_ratio;
					ratio = resize_start_ratio_v + dx_total / resize_start_box_v.width;
						if (ratio < 0.05f)
							ratio = 0.05f;
						if (ratio > 0.95f)
							ratio = 0.95f;
						if (fabs(ratio - current) >= resize_ratio_epsilon) {
							resize_split_node->split_ratio = (float)ratio;
							changed = 1;
						}
					}

				if (resize_use_h && resize_split_node_h && resize_start_box_h.height > 0) {
					double current_h = resize_split_node_h->split_ratio;
					ratio_h = resize_start_ratio_h + dy_total / resize_start_box_h.height;
						if (ratio_h < 0.05f)
							ratio_h = 0.05f;
						if (ratio_h > 0.95f)
							ratio_h = 0.95f;
						if (fabs(ratio_h - current_h) >= resize_ratio_epsilon) {
							resize_split_node_h->split_ratio = (float)ratio_h;
							changed = 1;
						}
					}

			if (changed)
				arrange(selmon);

		} else if (grabc && grabc->isfloating) {
			double dx_total = cursor->x - resize_start_x;
			double dy_total = cursor->y - resize_start_y;
			int minw = 1 + 2 * (int)grabc->bw;
			int minh = 1 + 2 * (int)grabc->bw;
			int dw = (int)lround(dx_total);
			int dh = (int)lround(dy_total);
			struct wlr_box box = resize_start_box_f;

			if (resize_dir_x) {
				if (resize_dir_x > 0) {
					if (box.width + dw < minw)
						dw = minw - box.width;
					box.width += dw;
				} else {
					if (box.width - dw < minw)
						dw = box.width - minw;
					box.x += dw;
					box.width -= dw;
				}
			}

			if (resize_dir_y) {
				if (resize_dir_y > 0) {
					if (box.height + dh < minh)
						dh = minh - box.height;
					box.height += dh;
				} else {
					if (box.height - dh < minh)
						dh = box.height - minh;
					box.y += dh;
					box.height -= dh;
				}
			}

			resize(grabc, box, 1);
			return;
		}
	}

focus:
	/* If there's no client surface under the cursor, set the cursor image to a
	 * default. This is what makes the cursor image appear when you move it
	 * off of a client or over its border. */
	if (!surface && !seat->drag)
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

	pointerfocus(c, surface, sx, sy, time);
}

void
motionrelative(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	last_pointer_motion_ms = monotonic_msec();
	motionnotify(event->time_msec, &event->pointer->base, event->delta_x, event->delta_y,
			event->unaccel_dx, event->unaccel_dy);
}

void
moveresize(const Arg *arg)
{
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
	if (!grabc || client_is_unmanaged(grabc) || grabc->isfullscreen)
		return;

	cursor_mode = arg->ui;
	grabc->was_tiled = (!grabc->isfloating && !grabc->isfullscreen);

	if (grabc->was_tiled) {
		switch (cursor_mode) {
		case CurMove:
			{
				struct wlr_box start_geom = grabc->geom;
				setfloating(grabc, 1);
				/* Anchor to the original cursor offset within the window */
				grabcx = (int)round(cursor->x) - start_geom.x;
				grabcy = (int)round(cursor->y) - start_geom.y;
				/* Keep the window anchored under the cursor when leaving tiling. */
				resize(grabc, start_geom, 1);
				wlr_cursor_set_xcursor(cursor, cursor_mgr, "fleur");
				break;
			}
		case CurResize:
			{
				struct wlr_box start_geom = grabc->geom;
				const char *cursor_name = pick_resize_handle(grabc, cursor->x, cursor->y);
				double start_x = cursor->x, start_y = cursor->y;
				grabcx = (int)round(cursor->x);
				grabcy = (int)round(cursor->y);

				resize_start_box_v = (struct wlr_box){0};
				resize_start_box_h = (struct wlr_box){0};
				resize_start_box_f = start_geom;
				resize_start_x = start_x;
				resize_start_y = start_y;
				resize_start_ratio_v = resize_start_ratio_h = 0.0;
				resize_split_node = NULL;
				resize_split_node_h = NULL;
				resize_last_time = 0;
				resize_last_x = start_x;
				resize_last_y = start_y;
				resizing_from_mouse = 1;
				/* Keep geometry unchanged as we switch to floating resize. */
				resize(grabc, start_geom, 1);
				wlr_cursor_set_xcursor(cursor, cursor_mgr, cursor_name);
			}
			break;
		}
	} else {
		/* Default floating logic */
		/* Float the window and tell motionnotify to grab it */
		setfloating(grabc, 1);
		switch (cursor_mode) {
		case CurMove:
			grabcx = (int)round(cursor->x) - grabc->geom.x;
			grabcy = (int)round(cursor->y) - grabc->geom.y;
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "fleur");
			break;
		case CurResize:
			{
				const char *cursor_name = pick_resize_handle(grabc, cursor->x, cursor->y);
				double start_x = cursor->x, start_y = cursor->y;
				grabcx = (int)round(cursor->x);
				grabcy = (int)round(cursor->y);

				resize_start_box_v = (struct wlr_box){0};
				resize_start_box_h = (struct wlr_box){0};
				resize_start_box_f = grabc->geom;
				resize_start_x = start_x;
				resize_start_y = start_y;
				resize_start_ratio_v = resize_start_ratio_h = 0.0;
				resize_split_node = NULL;
				resize_split_node_h = NULL;
				resize_last_time = 0;
				resize_last_x = start_x;
				resize_last_y = start_y;
				resizing_from_mouse = 1;
				wlr_cursor_set_xcursor(cursor, cursor_mgr, cursor_name);
			}
			break;
		}
	}
}

void
outputmgrapply(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 0);
}

void
outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test)
{
	/*
	 * Called when a client such as wlr-randr requests a change in output
	 * configuration. This is only one way that the layout can be changed,
	 * so any Monitor information should be updated by updatemons() after an
	 * output_layout.change event, not here.
	 */
	struct wlr_output_configuration_head_v1 *config_head;
	int ok = 1;

	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		Monitor *m = wlr_output->data;
		struct wlr_output_state state;

		/* Ensure displays previously disabled by wlr-output-power-management-v1
		 * are properly handled*/
		m->asleep = 0;

		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, config_head->state.enabled);
		if (!config_head->state.enabled)
			goto apply_or_test;

		if (config_head->state.mode)
			wlr_output_state_set_mode(&state, config_head->state.mode);
		else
			wlr_output_state_set_custom_mode(&state,
					config_head->state.custom_mode.width,
					config_head->state.custom_mode.height,
					config_head->state.custom_mode.refresh);

		wlr_output_state_set_transform(&state, config_head->state.transform);
		wlr_output_state_set_scale(&state, config_head->state.scale);
		wlr_output_state_set_adaptive_sync_enabled(&state,
				config_head->state.adaptive_sync_enabled);

apply_or_test:
		ok &= test ? wlr_output_test_state(wlr_output, &state)
				: wlr_output_commit_state(wlr_output, &state);

		/* Don't move monitors if position wouldn't change. This avoids
		 * wlroots marking the output as manually configured.
		 * wlr_output_layout_add does not like disabled outputs */
		if (!test && wlr_output->enabled && (m->m.x != config_head->state.x || m->m.y != config_head->state.y))
			wlr_output_layout_add(output_layout, wlr_output,
					config_head->state.x, config_head->state.y);

		wlr_output_state_finish(&state);
	}

	if (ok)
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);

	/* https://codeberg.org/dwl/dwl/issues/577 */
	updatemons(NULL, NULL);
}

void
outputmgrtest(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 1);
}

void
pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
		uint32_t time)
{
	struct timespec now;

	if (surface != seat->pointer_state.focused_surface &&
			sloppyfocus && time && c && !client_is_unmanaged(c))
		focusclient(c, 0);

	/* If surface is NULL, clear pointer focus */
	if (!surface) {
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	}

	if (!time) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	}

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

static void
updatetaghover(Monitor *m, double cx, double cy)
{
	StatusModule *tags;
	int lx, ly, hover = -1;
	int bar_h;

	if (!m || !m->showbar)
		return;

	tags = &m->statusbar.tags;
	if (!tags->tree || m->statusbar.area.width <= 0 || m->statusbar.area.height <= 0)
		return;

	lx = (int)floor(cx) - m->statusbar.area.x;
	ly = (int)floor(cy) - m->statusbar.area.y;
	if (lx >= 0 && ly >= 0 && lx < m->statusbar.area.width && ly < m->statusbar.area.height
			&& lx < tags->width) {
		for (int i = 0; i < tags->box_count; i++) {
			if (lx >= tags->box_x[i] && lx < tags->box_x[i] + tags->box_w[i]) {
				hover = tags->box_tag[i];
				break;
			}
		}
	}

	if (hover == tags->hover_tag)
		return;

	tags->hover_tag = hover;
	bar_h = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
	if (statusbar_hover_fade_ms <= 0) {
		for (int i = 0; i < TAGCOUNT; i++)
			tags->hover_alpha[i] = (tags->hover_tag == i) ? 1.0f : 0.0f;
		renderworkspaces(m, tags, bar_h);
		positionstatusmodules(m);
		return;
	}

	renderworkspaces(m, tags, bar_h);
	positionstatusmodules(m);
	if (status_hover_timer)
		wl_event_source_timer_update(status_hover_timer, 0);
	else
		schedule_hover_timer();
}

static void
schedule_hover_timer(void)
{
	Monitor *mon;

	if (!status_hover_timer)
		return;

	wl_list_for_each(mon, &mons, link) {
		Monitor *m = mon;
		int active = (m->statusbar.tags.hover_tag >= 0);
		for (int i = 0; i < TAGCOUNT && !active; i++) {
			if (m->statusbar.tags.hover_alpha[i] > 0.0f) {
				active = 1;
				break;
			}
		}
		if (active) {
			wl_event_source_timer_update(status_hover_timer, 16);
			return;
		}
	}
}

void
printstatus(void)
{
	Monitor *m = NULL;
	Client *c;
	uint32_t occ, urg, sel;

	wl_list_for_each(m, &mons, link) {
		occ = urg = 0;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			occ |= c->tags;
			if (c->isurgent)
				urg |= c->tags;
		}
		if ((c = focustop(m))) {
			printf("%s title %s\n", m->wlr_output->name, client_get_title(c));
			printf("%s appid %s\n", m->wlr_output->name, client_get_appid(c));
			printf("%s fullscreen %d\n", m->wlr_output->name, c->isfullscreen);
			printf("%s floating %d\n", m->wlr_output->name, c->isfloating);
			sel = c->tags;
		} else {
			printf("%s title \n", m->wlr_output->name);
			printf("%s appid \n", m->wlr_output->name);
			printf("%s fullscreen \n", m->wlr_output->name);
			printf("%s floating \n", m->wlr_output->name);
			sel = 0;
		}

		printf("%s selmon %u\n", m->wlr_output->name, m == selmon);
		printf("%s tags %"PRIu32" %"PRIu32" %"PRIu32" %"PRIu32"\n",
			m->wlr_output->name, occ, m->tagset[m->seltags], sel, urg);
		printf("%s layout %s\n", m->wlr_output->name, m->ltsymbol);
	}
	fflush(stdout);
}

void
powermgrsetmode(struct wl_listener *listener, void *data)
{
	struct wlr_output_power_v1_set_mode_event *event = data;
	struct wlr_output_state state = {0};
	Monitor *m = event->output->data;

	if (!m)
		return;

	m->gamma_lut_changed = 1; /* Reapply gamma LUT when re-enabling the ouput */
	wlr_output_state_set_enabled(&state, event->mode);
	wlr_output_commit_state(m->wlr_output, &state);

	m->asleep = !event->mode;
	updatemons(NULL, NULL);
}

void
setsticky(Client *c, int sticky)
{
	if (sticky && !c->issticky) {
		c->issticky = 1;
	} else if (!sticky && c->issticky) {
		c->issticky = 0;
		arrange(c->mon);
	}
}

void
quit(const Arg *arg)
{
	wl_display_terminate(dpy);
}

void
rendermon(struct wl_listener *listener, void *data)
{
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	Monitor *m = wl_container_of(listener, m, frame);
	struct wlr_output_state pending = {0};
	struct timespec now;

	wlr_scene_output_commit(m->scene_output, NULL);

	/* Let clients know a frame has been rendered */
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);
	wlr_output_state_finish(&pending);
}

void
requestdecorationmode(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_decoration_mode);
	if (c->surface.xdg->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(c->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
requeststartdrag(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(seat, event->origin,
			event->serial))
		wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

void
requestmonstate(struct wl_listener *listener, void *data)
{
	struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(event->output, event->state);
	updatemons(NULL, NULL);
}

void
resize(Client *c, struct wlr_box geo, int interact)
{
	struct wlr_box *bbox;
	struct wlr_box clip;
	int reqw, reqh;

	if (!c->mon || !client_surface(c)->mapped)
		return;

	bbox = interact ? &sgeom : &c->mon->w;

	client_set_bounds(c, geo.width, geo.height);
	c->geom = geo;
	applybounds(c, bbox);

	/* Update scene-graph, including borders */
	wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	wlr_scene_rect_set_size(c->border[0], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[1], c->geom.width, c->bw);
	wlr_scene_rect_set_size(c->border[2], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_rect_set_size(c->border[3], c->bw, c->geom.height - 2 * c->bw);
	wlr_scene_node_set_position(&c->border[1]->node, 0, c->geom.height - c->bw);
	wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
	wlr_scene_node_set_position(&c->border[3]->node, c->geom.width - c->bw, c->bw);

	reqw = c->geom.width - 2 * c->bw;
	reqh = c->geom.height - 2 * c->bw;

	/*
	 * Avoid flooding heavy clients: only send a new configure when there isn't
	 * already one pending. While waiting for an ack we just remember the
	 * latest requested size.
	 */
	if (!c->resize)
		c->resize = client_set_size(c, reqw, reqh);
	c->pending_resize_w = reqw;
	c->pending_resize_h = reqh;

	client_get_clip(c, &clip);
	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
}

void
run(const char *startup_cmd)
{
	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(dpy);
	if (!socket)
		die("startup: display_add_socket_auto");
	setenv("WAYLAND_DISPLAY", socket, 1);

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(backend))
		die("startup: backend_start");

	/* Now that the socket exists and the backend is started, run the startup command */
	if (startup_cmd) {
		int piperw[2];
		if (pipe(piperw) < 0)
			die("startup: pipe:");
		if ((child_pid = fork()) < 0)
			die("startup: fork:");
		if (child_pid == 0) {
			setsid();
			dup2(piperw[0], STDIN_FILENO);
			close(piperw[0]);
			close(piperw[1]);
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, NULL);
			die("startup: execl:");
		}
		dup2(piperw[1], STDOUT_FILENO);
		close(piperw[1]);
		close(piperw[0]);
	}

	/* Import environment variables then start systemd target */
	if (fork() == 0) {
		pid_t import_pid;
		setsid();

		/* First: import environment variables */
		import_pid = fork();
		if (import_pid == 0) {
			execvp("systemctl", (char *const[]) {
				"systemctl", "--user", "import-environment",
				"DISPLAY", "WAYLAND_DISPLAY", NULL
			});
			exit(1);
		}

		/* Wait for import to complete */
		waitpid(import_pid, NULL, 0);

		/* Second: start target */
		if (has_dwl_session_target()) {
			execvp("systemctl", (char *const[]) {
				"systemctl", "--user", "start", "dwl-session.target", NULL
			});
		}

		exit(1);
	}

	/* Mark stdout as non-blocking to avoid the startup script
	 * causing dwl to freeze when a user neither closes stdin
	 * nor consumes standard input in his startup script */

	if (fd_set_nonblock(STDOUT_FILENO) < 0)
		close(STDOUT_FILENO);

	printstatus();

	/* At this point the outputs are initialized, choose initial selmon based on
	 * cursor position, and set default cursor image */
	selmon = xytomon(cursor->x, cursor->y);

	/* TODO hack to get cursor to display in its initial location (100, 100)
	 * instead of (0, 0) and then jumping. Still may not be fully
	 * initialized, as the image/coordinates are not transformed for the
	 * monitor when displayed here */
	wlr_cursor_warp_closest(cursor, NULL, cursor->x, cursor->y);
	wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wl_display_run(dpy);
}

void
set_adaptive_sync(Monitor *m, int enable)
{
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;

	if (!m || !m->wlr_output || !m->wlr_output->enabled
			|| !fullscreen_adaptive_sync_enabled)
		return;

	config = wlr_output_configuration_v1_create();
	config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

	/* Set and commit the adaptive sync state change */
	wlr_output_state_init(&state);
	wlr_output_state_set_adaptive_sync_enabled(&state, enable);
	wlr_output_commit_state(m->wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Broadcast the adaptive sync state change to output_mgr */
	config_head->state.adaptive_sync_enabled = enable;
	wlr_output_manager_v1_set_configuration(output_mgr, config);
}

void
setcursor(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* If we're "grabbing" the cursor, don't use the client's image, we will
	 * restore it after "grabbing" sending a leave event, followed by a enter
	 * event, which will result in the client requesting set the cursor surface */
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided surface as the cursor image. It will set the
	 * hardware cursor on the output that it's currently on and continue to
	 * do so as the cursor moves between outputs. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_surface(cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
}

void
setcursorshape(struct wl_listener *listener, void *data)
{
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided cursor shape. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_xcursor(cursor, cursor_mgr,
				wlr_cursor_shape_v1_name(event->shape));
}

void
setfloating(Client *c, int floating)
{
	Client *p = client_get_parent(c);
	c->isfloating = floating;
	/* If in floating layout do not change the client's layer */
	if (!c->mon || !client_surface(c)->mapped || !c->mon->lt[c->mon->sellt]->arrange)
		return;
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen ||
			(p && p->isfullscreen) ? LyrFS
			: c->isfloating ? LyrFloat : LyrTile]);
	arrange(c->mon);
	printstatus();
}

void
setfullscreen(Client *c, int fullscreen)
{
	c->isfullscreen = fullscreen;
	if (!c->mon || !client_surface(c)->mapped)
		return;
	c->bw = fullscreen ? 0 : borderpx;
	client_set_fullscreen(c, fullscreen);
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen
			? LyrFS : c->isfloating ? LyrFloat : LyrTile]);

	if (fullscreen) {
		c->prev = c->geom;
		resize(c, c->mon->m, 0);
		set_adaptive_sync(c->mon, 1);
	} else {
		/* restore previous size instead of arrange for floating windows since
		 * client positions are set by the user and cannot be recalculated */
		resize(c, c->prev, 0);
		set_adaptive_sync(c->mon, 0);
	}
	arrange(c->mon);
	printstatus();
}

void
setlayout(const Arg *arg)
{
	if (!selmon)
		return;
	if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
		selmon->sellt ^= 1;
	if (arg && arg->v)
		selmon->lt[selmon->sellt] = (Layout *)arg->v;
	strncpy(selmon->ltsymbol, selmon->lt[selmon->sellt]->symbol, LENGTH(selmon->ltsymbol));
	arrange(selmon);
	printstatus();
}

/* arg > 1.0 will set mfact absolutely */
void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0f ? arg->f + selmon->mfact : arg->f - 1.0f;
	if (f < 0.1 || f > 0.9)
		return;
	selmon->mfact = f;
	arrange(selmon);
}

void
setmon(Client *c, Monitor *m, uint32_t newtags)
{
	Monitor *oldmon = c->mon;

	if (oldmon == m)
		return;
	c->mon = m;
	c->prev = c->geom;

	/* Scene graph sends surface leave/enter events on move and resize */
	if (oldmon)
		arrange(oldmon);
	if (m) {
		/* Make sure window actually overlaps with the monitor */
		resize(c, c->geom, 0);
		c->tags = newtags ? newtags : m->tagset[m->seltags]; /* assign tags of target monitor */
		setfullscreen(c, c->isfullscreen); /* This will call arrange(c->mon) */
		setfloating(c, c->isfloating);
	}
	focusclient(focustop(selmon), 1);
}

void
setpsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor them
	 */
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

void
setsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor them
	 */
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(seat, event->source, event->serial);
}

static void
ensure_shell_env(void)
{
	const char *shell = getenv("SHELL");
	struct passwd *pw;
	int looks_like_minimal_nix_bash = 0;

	if (shell && *shell) {
		looks_like_minimal_nix_bash = strstr(shell, "/nix/store/") &&
				strstr(shell, "bash-") &&
				!strstr(shell, "bash-interactive");
		if (!looks_like_minimal_nix_bash)
			return;
	}

	pw = getpwuid(getuid());
	if (pw && pw->pw_shell && *pw->pw_shell)
		setenv("SHELL", pw->pw_shell, 1);
}

void
setup(void)
{
	int drm_fd, i, sig[] = {SIGCHLD, SIGINT, SIGTERM, SIGPIPE};
	struct sigaction sa = {.sa_flags = SA_RESTART, .sa_handler = handlesig};
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < (int)LENGTH(sig); i++)
		sigaction(sig[i], &sa, NULL);

	wlr_log_init(log_level, NULL);

	/* Make sure spawned terminals get the real login shell, not the minimal wrapper shell */
	ensure_shell_env();

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	dpy = wl_display_create();
	event_loop = wl_display_get_event_loop(dpy);
	wl_list_init(&mons);
	wl_list_init(&tray_items);
	status_timer = wl_event_loop_add_timer(event_loop, updatestatusclock, NULL);
	status_cpu_timer = wl_event_loop_add_timer(event_loop, updatestatuscpu, NULL);
	status_hover_timer = wl_event_loop_add_timer(event_loop, updatehoverfade, NULL);
	tray_init();
	fcft_initialized = fcft_init(FCFT_LOG_COLORIZE_NEVER, 0, FCFT_LOG_CLASS_ERROR);
	if (!fcft_initialized)
		die("couldn't initialize fcft");
	init_net_icon_paths();
	if (!loadstatusfont())
		die("couldn't load statusbar font");
	tray_update_icons_text();
	ensure_desktop_entries_loaded();
	backlight_available = findbacklightdevice(backlight_brightness_path,
			sizeof(backlight_brightness_path),
			backlight_max_path, sizeof(backlight_max_path));

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	if (!(backend = wlr_backend_autocreate(event_loop, &session)))
		die("couldn't create backend");

	/* Initialize the scene graph used to lay out windows */
	scene = wlr_scene_create();
	root_bg = wlr_scene_rect_create(&scene->tree, 0, 0, rootcolor);
	for (i = 0; i < NUM_LAYERS; i++)
		layers[i] = wlr_scene_tree_create(&scene->tree);
	drag_icon = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_place_below(&drag_icon->node, &layers[LyrBlock]->node);

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	if (!(drw = wlr_renderer_autocreate(backend)))
		die("couldn't create renderer");
	wl_signal_add(&drw->events.lost, &gpu_reset);

	/* Create shm, drm and linux_dmabuf interfaces by ourselves.
	 * The simplest way is to call:
	 *      wlr_renderer_init_wl_display(drw);
	 * but we need to create the linux_dmabuf interface manually to integrate it
	 * with wlr_scene. */
	wlr_renderer_init_wl_shm(drw, dpy);

	if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
		wlr_drm_create(dpy, drw);
		wlr_scene_set_linux_dmabuf_v1(scene,
				wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw));
	}

	if ((drm_fd = wlr_renderer_get_drm_fd(drw)) >= 0 && drw->features.timeline
			&& backend->features.timeline)
		wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, drm_fd);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't create allocator");

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the setsel() function. */
	compositor = wlr_compositor_create(dpy, 6, drw);
	wlr_subcompositor_create(dpy);
	wlr_data_device_manager_create(dpy);
	wlr_export_dmabuf_manager_v1_create(dpy);
	wlr_screencopy_manager_v1_create(dpy);
	wlr_data_control_manager_v1_create(dpy);
	wlr_primary_selection_v1_device_manager_create(dpy);
	wlr_viewporter_create(dpy);
	wlr_single_pixel_buffer_manager_v1_create(dpy);
	wlr_fractional_scale_manager_v1_create(dpy, 1);
	wlr_presentation_create(dpy, backend, 2);
	wlr_alpha_modifier_v1_create(dpy);

	/* Initializes the interface used to implement urgency hints */
	activation = wlr_xdg_activation_v1_create(dpy);
	wl_signal_add(&activation->events.request_activate, &request_activate);

	wlr_scene_set_gamma_control_manager_v1(scene, wlr_gamma_control_manager_v1_create(dpy));

	power_mgr = wlr_output_power_manager_v1_create(dpy);
	wl_signal_add(&power_mgr->events.set_mode, &output_power_mgr_set_mode);

	/* Creates an output layout, which is a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	output_layout = wlr_output_layout_create(dpy);
	wl_signal_add(&output_layout->events.change, &layout_change);

    wlr_xdg_output_manager_v1_create(dpy, output_layout);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_signal_add(&backend->events.new_output, &new_output);

	/* Set up our client lists, the xdg-shell and the layer-shell. The xdg-shell is a
	 * Wayland protocol which is used for application windows. For more
	 * detail on shells, refer to the article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&clients);
	wl_list_init(&fstack);

	xdg_shell = wlr_xdg_shell_create(dpy, 6);
	wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
	wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

	layer_shell = wlr_layer_shell_v1_create(dpy, 3);
	wl_signal_add(&layer_shell->events.new_surface, &new_layer_surface);

	idle_notifier = wlr_idle_notifier_v1_create(dpy);

	idle_inhibit_mgr = wlr_idle_inhibit_v1_create(dpy);
	wl_signal_add(&idle_inhibit_mgr->events.new_inhibitor, &new_idle_inhibitor);

	session_lock_mgr = wlr_session_lock_manager_v1_create(dpy);
	wl_signal_add(&session_lock_mgr->events.new_lock, &new_session_lock);
	locked_bg = wlr_scene_rect_create(layers[LyrBlock], sgeom.width, sgeom.height,
			(float [4]){0.1f, 0.1f, 0.1f, 1.0f});
	wlr_scene_node_set_enabled(&locked_bg->node, 0);

	/* Use decoration protocols to negotiate server-side decorations */
	wlr_server_decoration_manager_set_default_mode(
			wlr_server_decoration_manager_create(dpy),
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(dpy);
	wl_signal_add(&xdg_decoration_mgr->events.new_toplevel_decoration, &new_xdg_decoration);

	pointer_constraints = wlr_pointer_constraints_v1_create(dpy);
	wl_signal_add(&pointer_constraints->events.new_constraint, &new_pointer_constraint);

	relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(dpy);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). Scaled cursors will be loaded with each output. */
	cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	setenv("XCURSOR_SIZE", "24", 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	wl_signal_add(&cursor->events.motion, &cursor_motion);
	wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
	wl_signal_add(&cursor->events.button, &cursor_button);
	wl_signal_add(&cursor->events.axis, &cursor_axis);
	wl_signal_add(&cursor->events.frame, &cursor_frame);

	cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(dpy, 1);
	wl_signal_add(&cursor_shape_mgr->events.request_set_shape, &request_set_cursor_shape);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_signal_add(&backend->events.new_input, &new_input_device);
	virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(dpy);
	wl_signal_add(&virtual_keyboard_mgr->events.new_virtual_keyboard,
			&new_virtual_keyboard);
	virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(dpy);
    wl_signal_add(&virtual_pointer_mgr->events.new_virtual_pointer,
            &new_virtual_pointer);

	seat = wlr_seat_create(dpy, "seat0");
	wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
	wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
	wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);
	wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
	wl_signal_add(&seat->events.start_drag, &start_drag);

	kb_group = createkeyboardgroup();
	wl_list_init(&kb_group->destroy.link);

	output_mgr = wlr_output_manager_v1_create(dpy);
	wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);
	wl_signal_add(&output_mgr->events.test, &output_mgr_test);

	/* Make sure XWayland clients don't connect to the parent X server,
	 * e.g when running in the x11 backend or the wayland backend and the
	 * compositor has Xwayland support */
	unsetenv("DISPLAY");
#ifdef XWAYLAND
	/*
	 * Initialise the XWayland X server.
	 * It will be started when the first X client is started.
	 */
	if ((xwayland = wlr_xwayland_create(dpy, compositor, 1))) {
		wl_signal_add(&xwayland->events.ready, &xwayland_ready);
		wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

		setenv("DISPLAY", xwayland->display_name, 1);
	} else {
		wlr_log(WLR_ERROR, "failed to setup XWayland X server, continuing without it");
	}
#endif

	initial_status_refresh();
	if (status_timer)
		schedule_status_timer();
	if (status_cpu_timer) {
		init_status_refresh_tasks();
		schedule_next_status_refresh();
	}
	if (!wifi_scan_timer)
		wifi_scan_timer = wl_event_loop_add_timer(event_loop, wifi_scan_timer_cb, NULL);
	if (status_hover_timer)
		wl_event_source_timer_update(status_hover_timer, 0);
}

void
spawn(const Arg *arg)
{
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		die("dwl: execvp %s failed:", ((char **)arg->v)[0]);
	}
}

void
startdrag(struct wl_listener *listener, void *data)
{
	struct wlr_drag *drag = data;
	if (!drag->icon)
		return;

	drag->icon->data = &wlr_scene_drag_icon_create(drag_icon, drag->icon)->node;
	LISTEN_STATIC(&drag->icon->events.destroy, destroydragicon);
}

void
tag(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel || (arg->ui & TAGMASK) == 0)
		return;

	sel->tags = arg->ui & TAGMASK;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
tagmon(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel)
		return;
	setmon(sel, dirtomon(arg->i), 0);
	free(sel->output);
	if (!(sel->output = strdup(sel->mon->wlr_output->name)))
		die("oom");
}

void
tile(Monitor *m)
{
	unsigned int h, r, e = m->gaps, mw, my, ty;
	int i, n = 0;
	Client *c;

	wl_list_for_each(c, &clients, link)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;
	if (n == 0)
		return;
	if (smartgaps == n)
		e = 0;

	if (n > m->nmaster)
		mw = m->nmaster ? (int)roundf((m->w.width + gappx * e) * m->mfact) : 0;
	else
		mw = m->w.width;
	i = 0;
	my = ty = gappx * e;
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		if (i < m->nmaster) {
			r = MIN(n, m->nmaster) - i;
			h = (m->w.height - my - gappx * e - gappx * e * (r - 1)) / r;
			resize(c, (struct wlr_box){.x = m->w.x + gappx * e, .y = m->w.y + my,
				.width = mw - 2 * gappx * e, .height = h}, 0);
			my += c->geom.height + gappx * e;
		} else {
			r = n - i;
			h = (m->w.height - ty - gappx * e - gappx * e * (r - 1)) / r;
			resize(c, (struct wlr_box){.x = m->w.x + mw, .y = m->w.y + ty,
				.width = m->w.width - mw - gappx * e, .height = h}, 0);
			ty += c->geom.height + gappx * e;
		}
		i++;
	}
}

void
togglefloating(const Arg *arg)
{
	Client *sel = focustop(selmon);
	/* return if fullscreen */
	if (sel && !sel->isfullscreen)
		setfloating(sel, !sel->isfloating);
}

void
togglefullscreen(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		setfullscreen(sel, !sel->isfullscreen);
}

void
togglefullscreenadaptivesync(const Arg *arg)
{
	fullscreen_adaptive_sync_enabled = !fullscreen_adaptive_sync_enabled;
}

void
__attribute__((unused)) togglesticky(const Arg *arg)
{
	Client *c = focustop(selmon);

	if (!c)
		return;

	setsticky(c, !c->issticky);
}

void
togglegaps(const Arg *arg)
{
	if (!selmon)
		return;
	selmon->gaps = !selmon->gaps;
	arrangelayers(selmon);
	arrange(selmon);
}

void
togglestatusbar(const Arg *arg)
{
	(void)arg;
	if (!selmon)
		return;
	selmon->showbar = !selmon->showbar;
	arrangelayers(selmon);
}

void
toggletag(const Arg *arg)
{
	uint32_t newtags;
	Client *sel = focustop(selmon);
	if (!sel || !(newtags = sel->tags ^ (arg->ui & TAGMASK)))
		return;

	sel->tags = newtags;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
toggleview(const Arg *arg)
{
	uint32_t newtagset;
	if (!(newtagset = selmon ? selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK) : 0))
		return;

	selmon->tagset[selmon->seltags] = newtagset;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
unlocksession(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, unlock);
	destroylock(lock, 1);
}

void
unmaplayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, unmap);

	l->mapped = 0;
	wlr_scene_node_set_enabled(&l->scene->node, 0);
	if (l == exclusive_focus)
		exclusive_focus = NULL;
	if (l->layer_surface->output && (l->mon = l->layer_surface->output->data))
		arrangelayers(l->mon);
	if (l->layer_surface->surface == seat->keyboard_state.focused_surface)
		focusclient(focustop(selmon), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
unmapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is unmapped, and should no longer be shown. */
	Client *c = wl_container_of(listener, c, unmap);
	if (c == grabc) {
		cursor_mode = CurNormal;
		grabc = NULL;
	}

	if (client_is_unmanaged(c)) {
		if (c == exclusive_focus) {
			exclusive_focus = NULL;
			focusclient(focustop(selmon), 1);
		}
	} else {
		wl_list_remove(&c->link);
		setmon(c, NULL, 0);
		wl_list_remove(&c->flink);
	}
	/* Toggle adaptive sync off when fullscreen client is unmapped */
	if (c->isfullscreen)
		set_adaptive_sync(c->mon ? c->mon : selmon, 0);

	wlr_scene_node_destroy(&c->scene->node);
	printstatus();
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
updatemons(struct wl_listener *listener, void *data)
{
	/*
	 * Called whenever the output layout changes: adding or removing a
	 * monitor, changing an output's mode or position, etc. This is where
	 * the change officially happens and we update geometry, window
	 * positions, focus, and the stored configuration in wlroots'
	 * output-manager implementation.
	 */
	struct wlr_output_configuration_v1 *config
			= wlr_output_configuration_v1_create();
	Client *c;
	struct wlr_output_configuration_head_v1 *config_head;
	Monitor *m;

	/* First remove from the layout the disabled monitors */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled || m->asleep)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
		config_head->state.enabled = 0;
		/* Remove this output from the layout to avoid cursor enter inside it */
		wlr_output_layout_remove(output_layout, m->wlr_output);
		closemon(m);
		m->m = m->w = (struct wlr_box){0};
	}
	/* Insert outputs that need to */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled
				&& !wlr_output_layout_get(output_layout, m->wlr_output))
			wlr_output_layout_add_auto(output_layout, m->wlr_output);
	}

	/* Now that we update the output layout we can get its box */
	wlr_output_layout_get_box(output_layout, NULL, &sgeom);

	wlr_scene_node_set_position(&root_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(root_bg, sgeom.width, sgeom.height);

	/* Make sure the clients are hidden when dwl is locked */
	wlr_scene_node_set_position(&locked_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(locked_bg, sgeom.width, sgeom.height);

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

		/* Get the effective monitor geometry to use for surfaces */
		wlr_output_layout_get_box(output_layout, m->wlr_output, &m->m);
		m->w = m->m;
		wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

		wlr_scene_node_set_position(&m->fullscreen_bg->node, m->m.x, m->m.y);
		wlr_scene_rect_set_size(m->fullscreen_bg, m->m.width, m->m.height);

		if (m->lock_surface) {
			struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
			wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
			wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width, m->m.height);
		}

		/* Calculate the effective monitor geometry to use for clients */
		arrangelayers(m);
		/* Don't move clients to the left output when plugging monitors */
		arrange(m);
		/* make sure fullscreen clients have the right size */
		if ((c = focustop(m)) && c->isfullscreen)
			resize(c, m->m, 0);

		/* Try to re-set the gamma LUT when updating monitors,
		 * it's only really needed when enabling a disabled output, but meh. */
		m->gamma_lut_changed = 1;

		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;

		if (!selmon) {
			selmon = m;
		}
	}

	if (selmon && selmon->wlr_output->enabled) {
		wl_list_for_each(c, &clients, link) {
			if (!c->mon && client_surface(c)->mapped)
				setmon(c, selmon, c->tags);
		}
		focusclient(focustop(selmon), 1);
		if (selmon->lock_surface) {
			client_notify_enter(selmon->lock_surface->surface,
					wlr_seat_get_keyboard(seat));
			client_activate_surface(selmon->lock_surface->surface, 1);
		}
	}

	/* FIXME: figure out why the cursor image is at 0,0 after turning all
	 * the monitors on.
	 * Move the cursor image where it used to be. It does not generate a
	 * wl_pointer.motion event for the clients, it's only the image what it's
	 * at the wrong position after all. */
	wlr_cursor_move(cursor, NULL, 0, 0);

	wlr_output_manager_v1_set_configuration(output_mgr, config);
}

void
updatetitle(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_title);
	if (c == focustop(c->mon))
		printstatus();
}

void
urgent(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	Client *c = NULL;
	toplevel_from_wlr_surface(event->surface, &c, NULL);
	if (!c || c == focustop(selmon))
		return;

	c->isurgent = 1;
	printstatus();

	if (client_surface(c)->mapped)
		client_set_border_color(c, urgentcolor);
}

void
view(const Arg *arg)
{
	if (!selmon || (arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
		return;
	selmon->seltags ^= 1; /* toggle sel tagset */
	if (arg->ui & TAGMASK)
		selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
virtualkeyboard(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_keyboard_v1 *kb = data;
	/* virtual keyboards shouldn't share keyboard group */
	KeyboardGroup *group = createkeyboardgroup();
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
	LISTEN(&kb->keyboard.base.events.destroy, &group->destroy, destroykeyboardgroup);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);
}

void
virtualpointer(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_input_device *device = &event->new_pointer->pointer.base;

	wlr_cursor_attach_input_device(cursor, device);
	if (event->suggested_output)
		wlr_cursor_map_input_to_output(cursor, device, event->suggested_output);
}

void
warpcursor(const Client *c)
{
	(void)c;
	/* Intentionally do not warp the cursor; keep it exactly where the user left it. */
}

Monitor *
xytomon(double x, double y)
{
	struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
	return o ? o->data : NULL;
}

void
xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny)
{
	struct wlr_scene_node *node, *pnode;
	struct wlr_surface *surface = NULL;
	Client *c = NULL;
	LayerSurface *l = NULL;
	int layer;

	for (layer = NUM_LAYERS - 1; !surface && layer >= 0; layer--) {
		if (!(node = wlr_scene_node_at(&layers[layer]->node, x, y, nx, ny)))
			continue;

		if (node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_surface *scene_surface =
				wlr_scene_surface_try_from_buffer(
						wlr_scene_buffer_from_node(node));
			if (scene_surface)
				surface = scene_surface->surface;
		}
		/* Walk the tree to find a node that knows the client */
		for (pnode = node; pnode && !c; pnode = &pnode->parent->node)
			c = pnode->data;
		if (c && c->type == LayerShell) {
			c = NULL;
			l = pnode->data;
		}
	}

	if (psurface) *psurface = surface;
	if (pc) *pc = c;
	if (pl) *pl = l;
}

void
zoom(const Arg *arg)
{
	Client *c, *sel = focustop(selmon);

	if (!sel || !selmon || !selmon->lt[selmon->sellt]->arrange || sel->isfloating)
		return;

	/* Search for the first tiled window that is not sel, marking sel as
	 * NULL if we pass it along the way */
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, selmon) && !c->isfloating) {
			if (c != sel)
				break;
			sel = NULL;
		}
	}

	/* Return if no other tiled window was found */
	if (&c->link == &clients)
		return;

	/* If we passed sel, move c to the front; otherwise, move sel to the
	 * front */
	if (!sel)
		sel = c;
	wl_list_remove(&sel->link);
	wl_list_insert(&clients, &sel->link);

	focusclient(sel, 1);
	arrange(selmon);
}

static void rotate_clients(const Arg *arg) {
	Monitor* m = selmon;
	Client *c;
	Client *first = NULL;
	Client *last = NULL;

	if (arg->i == 0)
		return;

	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen) {
			if (first == NULL) first = c;
			last = c;
		}
	}
	if (first != last) {
		struct wl_list *append_to = (arg->i > 0) ? &last->link : first->link.prev;
		struct wl_list *elem = (arg->i > 0) ? &first->link : &last->link;
		wl_list_remove(elem);
		wl_list_insert(append_to, elem);
		arrange(selmon);
	} 
}

#ifdef XWAYLAND
void
activatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, activate);

	/* Only "managed" windows can be activated */
	if (!client_is_unmanaged(c))
		wlr_xwayland_surface_activate(c->surface.xwayland, 1);
}

void
associatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, associate);

	LISTEN(&client_surface(c)->events.map, &c->map, mapnotify);
	LISTEN(&client_surface(c)->events.unmap, &c->unmap, unmapnotify);
}

void
configurex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	if (!client_surface(c) || !client_surface(c)->mapped) {
		wlr_xwayland_surface_configure(c->surface.xwayland,
				event->x, event->y, event->width, event->height);
		return;
	}
	if (client_is_unmanaged(c)) {
		wlr_scene_node_set_position(&c->scene->node, event->x, event->y);
		wlr_xwayland_surface_configure(c->surface.xwayland,
				event->x, event->y, event->width, event->height);
		return;
	}
	if ((c->isfloating && c != grabc) || !c->mon->lt[c->mon->sellt]->arrange) {
		resize(c, (struct wlr_box){.x = event->x - c->bw,
				.y = event->y - c->bw, .width = event->width + c->bw * 2,
				.height = event->height + c->bw * 2}, 0);
	} else {
		arrange(c->mon);
	}
}

void
minimizenotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, minimize);
	struct wlr_xwayland_surface *xsurface = c->surface.xwayland;
	struct wlr_xwayland_minimize_event *e = data;
	int focused;

	if (xsurface->surface == NULL || !xsurface->surface->mapped)
		return;

	focused = seat->keyboard_state.focused_surface == xsurface->surface;
	wlr_xwayland_surface_set_minimized(xsurface, !focused && e->minimize);
}

void
createnotifyx11(struct wl_listener *listener, void *data)
{
	struct wlr_xwayland_surface *xsurface = data;
	Client *c;

	/* Allocate a Client for this surface */
	c = xsurface->data = ecalloc(1, sizeof(*c));
	c->surface.xwayland = xsurface;
	c->type = X11;
	c->bw = client_is_unmanaged(c) ? 0 : borderpx;

	/* Listen to the various events it can emit */
	LISTEN(&xsurface->events.associate, &c->associate, associatex11);
	LISTEN(&xsurface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xsurface->events.dissociate, &c->dissociate, dissociatex11);
	LISTEN(&xsurface->events.request_activate, &c->activate, activatex11);
	LISTEN(&xsurface->events.request_minimize, &c->minimize, minimizenotify);
	LISTEN(&xsurface->events.request_configure, &c->configure, configurex11);
	LISTEN(&xsurface->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&xsurface->events.set_hints, &c->set_hints, sethints);
	LISTEN(&xsurface->events.set_title, &c->set_title, updatetitle);
}

void
dissociatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, dissociate);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
}

void
sethints(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_hints);
	struct wlr_surface *surface = client_surface(c);
	if (c == focustop(selmon) || !c->surface.xwayland->hints)
		return;

	c->isurgent = xcb_icccm_wm_hints_get_urgency(c->surface.xwayland->hints);
	printstatus();

	if (c->isurgent && surface && surface->mapped)
		client_set_border_color(c, urgentcolor);
}

void
xwaylandready(struct wl_listener *listener, void *data)
{
	struct wlr_xcursor *xcursor;

	/* assign the one and only seat */
	wlr_xwayland_set_seat(xwayland, seat);

	/* Set the default XWayland cursor to match the rest of dwl. */
	if ((xcursor = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1)))
		wlr_xwayland_set_cursor(xwayland,
				xcursor->images[0]->buffer, xcursor->images[0]->width * 4,
				xcursor->images[0]->width, xcursor->images[0]->height,
				xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);
}
#endif

int
main(int argc, char *argv[])
{
	const char *startup_cmd = NULL;
	int c;

	while ((c = getopt(argc, argv, "s:hdv")) != -1) {
		if (c == 's')
			startup_cmd = optarg;
		else if (c == 'd')
			log_level = WLR_DEBUG;
		else if (c == 'v')
			die("dwl " VERSION);
		else
			goto usage;
	}
	if (optind < argc)
		goto usage;

	if (!startup_cmd)
		startup_cmd = autostart_cmd;

	/* Wayland requires XDG_RUNTIME_DIR for creating its communications socket */
	if (!getenv("XDG_RUNTIME_DIR"))
		die("XDG_RUNTIME_DIR must be set");
	setup();
	run(startup_cmd);
	cleanup();
	return EXIT_SUCCESS;

usage:
	die("Usage: %s [-v] [-d] [-s startup command]", argv[0]);
}

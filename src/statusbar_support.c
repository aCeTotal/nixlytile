/*
 * statusbar_support.c — globals + icon/font/net helpers backing the
 * embedded status bar (statusbar.c) and system tray (tray.c).
 * Restored from the pre-refactor modular tree (commit dd6fa93).
 */
#include "nixlytile.h"
#include "client.h"

/* ── statusbar / tray / net global state ─────────────────────────── */
struct wl_event_source *status_timer;
struct wl_event_source *status_cpu_timer;
struct wl_event_source *status_hover_timer;
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
const double status_icon_scale = 2.0;
pid_t ssid_pid = -1;
struct wl_event_source *ssid_event = NULL;
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
char last_cpu_render[32];
char last_ram_render[32];
char last_light_render[32];
char last_volume_render[32];
char last_mic_render[32];
char last_battery_render[32];
char last_net_render[64];
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
const float *volume_text_color = NULL; /* set at runtime; defaults to statusbar_fg */
const float *mic_text_color = NULL;
const float *statusbar_fg_override = NULL;
struct wl_event_source *cpu_popup_refresh_timer = NULL;
struct wl_event_source *ram_popup_refresh_timer = NULL;
struct wl_event_source *popup_delay_timer = NULL;
struct wl_list wifi_networks; /* WifiNetwork */
int wifi_networks_initialized;
struct wl_list vpn_connections; /* VpnConnection */
int vpn_list_initialized;
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
const uint32_t cpu_popup_refresh_interval_ms = 2000;
const uint32_t ram_popup_refresh_interval_ms = 2000;
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
unsigned long long net_prev_rx;
unsigned long long net_prev_tx;
struct timespec net_prev_ts;
int net_prev_valid;
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
double speaker_active = -1.0;
double speaker_stored = 70.0;
double microphone_active = -1.0;
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
const double volume_max_percent = 150.0;
const double mic_max_percent = 150.0;
double cpu_last_core_percent[MAX_CPU_CORES];
int cpu_core_count;
char sysicons_text[64] = "Tray";

/* ── draw helpers (drawhoverrect / drawroundedrect) ──────────────── */
void
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

void
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

/* ── icon/font buffer + asset-path helpers ───────────────────────── */
struct wlr_buffer *
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

int
pathisdir(const char *path)
{
	struct stat st;

	return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int
pathisfile(const char *path)
{
	struct stat st;

	return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int
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

	envdir = getenv("NIXLYTILE_ICON_DIR");
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
		if (snprintf(cand, sizeof(cand), "%s/../share/nixlytile/%s", exe_dir, path) < (int)sizeof(cand)
				&& pathisfile(cand)) {
			snprintf(out, len, "%s", cand);
			return 0;
		}
	}

	return -1;
}

int
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

int
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

void
add_icon_root_paths(const char *base, const char *themes[], size_t theme_count,
		char pathbufs[][PATH_MAX], size_t *pathcount, size_t max_paths)
{
	if (!base || !*base || !themes || !pathbufs || !pathcount)
		return;

	/* Most XDG_DATA_DIRS entries have no icons/ subdir.  Skip those so
	 * they don't burn the (bounded) pathbufs slots before we reach the
	 * dirs that actually hold icon themes. */
	if (!pathisdir(base))
		return;

	for (size_t i = 0; i < theme_count && *pathcount < max_paths; i++) {
		char themed[PATH_MAX];
		snprintf(themed, sizeof(themed), "%s/%s", base, themes[i]);
		if (!pathisdir(themed))
			continue;
		snprintf(pathbufs[*pathcount], PATH_MAX, "%s", themed);
		(*pathcount)++;
	}

	if (*pathcount < max_paths) {
		snprintf(pathbufs[*pathcount], PATH_MAX, "%s", base);
		(*pathcount)++;
	}
}

GdkPixbuf *
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

void
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

struct wlr_buffer *
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

void
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

struct wlr_buffer *
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

int
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

void
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

void
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

struct wlr_buffer *
statusbar_scaled_buffer_from_argb32_ex(const uint32_t *data, int width, int height,
		int target_h, int fix_format)
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

	if (fix_format) {
		fix_tray_argb32((uint32_t *)src_copy, (size_t)width * (size_t)height, 0);

		/* If the first pass produced fully transparent data, retry assuming RGBA order */
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

struct wlr_buffer *
statusbar_scaled_buffer_from_argb32(const uint32_t *data, int width, int height, int target_h)
{
	return statusbar_scaled_buffer_from_argb32_ex(data, width, height, target_h, 1);
}

struct wlr_buffer *
statusbar_scaled_buffer_from_argb32_raw(const uint32_t *data, int width, int height, int target_h)
{
	return statusbar_scaled_buffer_from_argb32_ex(data, width, height, target_h, 0);
}

struct wlr_buffer *
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

struct wlr_buffer *
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

int
loadstatusfont(void)
{
	size_t count;
	const char **fonts;

	if (statusfont.font)
		return 1;

	/* Use runtime fonts if configured, otherwise use default */
	if (runtime_fonts_set && runtime_fonts[0]) {
		fonts = (const char **)runtime_fonts;
		for (count = 0; count < 8 && fonts[count]; count++)
			;
	} else {
		fonts = statusbar_fonts;
		/* Count actual non-NULL fonts, not array size */
		for (count = 0; count < LENGTH(statusbar_fonts) && fonts[count]; count++)
			;
	}

	if (count == 0)
		return 0;

	statusfont.font = fcft_from_name(count, fonts, statusbar_font_attributes);
	if (!statusfont.font)
		return 0;

	statusfont.ascent = statusfont.font->ascent;
	statusfont.descent = statusfont.font->descent;
	statusfont.height = statusfont.font->height;
	return 1;
}

void
freestatusfont(void)
{
	if (statusfont.font)
		fcft_destroy(statusfont.font);
	statusfont.font = NULL;
}

/* ── async network fetch (public IP / SSID) ──────────────────────── */
#include "nixlytile.h"
#include "client.h"

static inline void
cleanup_async_task(struct wl_event_source **ev, int *fd, pid_t *pid)
{
	if (*ev) {
		wl_event_source_remove(*ev);
		*ev = NULL;
	}
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
	if (*pid > 0) {
		waitpid(*pid, NULL, WNOHANG);
		*pid = -1;
	}
}

void
request_public_ip_async_ex(int force)
{
	const char *cmd;
	time_t now = time(NULL);

	if (now == (time_t)-1)
		return;

	/* Skip rate limit if force is set (real-time update when hovering) */
	if (!force && net_public_ip_last != 0 && (now - net_public_ip_last) < 300)
		return;

	if (public_ip_pid > 0 || public_ip_event)
		return;

	cmd = getenv("NIXLYTILE_PUBLIC_IP_CMD");
	if (!cmd || !cmd[0])
		cmd = "curl -4 -s https://ifconfig.me";

	if (spawn_async_read(cmd, &public_ip_pid, &public_ip_fd) != 0)
		return;

	public_ip_len = 0;
	public_ip_buf[0] = '\0';
	net_public_ip_last = now; /* rate-limit even if fetch fails */
	public_ip_event = wl_event_loop_add_fd(event_loop, public_ip_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP,
			public_ip_event_cb, NULL);
	if (!public_ip_event)
		stop_public_ip_fetch();
}

void
request_public_ip_async(void)
{
	request_public_ip_async_ex(0);
}

void
stop_public_ip_fetch(void)
{
	cleanup_async_task(&public_ip_event, &public_ip_fd, &public_ip_pid);
	public_ip_len = 0;
	public_ip_buf[0] = '\0';
}

int
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

void
request_ssid_async(const char *iface)
{
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

	if (spawn_async_read(cmd, &ssid_pid, &ssid_fd) != 0)
		return;

	ssid_len = 0;
	ssid_buf[0] = '\0';
	ssid_event = wl_event_loop_add_fd(event_loop, ssid_fd,
			WL_EVENT_READABLE | WL_EVENT_HANGUP,
			ssid_event_cb, NULL);
	if (!ssid_event)
		stop_ssid_fetch();
}

void
stop_ssid_fetch(void)
{
	cleanup_async_task(&ssid_event, &ssid_fd, &ssid_pid);
	ssid_len = 0;
	ssid_buf[0] = '\0';
}

int
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

/* ── startup defaults (backlight / volume / mic) ─────────────────── */
void
apply_startup_defaults(void)
{
	static int applied = 0;
	const double light_default = 40.0;
	const double speaker_default = 70.0;
	const double mic_default = 80.0;

	if (applied)
		return;

	/* Backlight - only on laptops with a backlight device.
	 * Ensure paths are initialized before checking availability,
	 * since this runs before the first refreshstatuslight(). */
	if (!backlight_paths_initialized) {
		backlight_available = findbacklightdevice(backlight_brightness_path,
				sizeof(backlight_brightness_path),
				backlight_max_path, sizeof(backlight_max_path));
		if (!backlight_available)
			backlight_writable = 0;
		backlight_paths_initialized = 1;
	}
	if (backlight_available) {
		if (set_backlight_percent(light_default) == 0) {
			light_last_percent = light_default;
			light_cached_percent = light_default;
		} else if (light_cached_percent < 0.0) {
			light_cached_percent = light_default;
		}
	}

	/*
	 * Speaker/headset volume - read actual state from PipeWire first,
	 * then unmute and set to default. This ensures statusbar shows
	 * correct state from the very beginning.
	 */
	{
		int is_headset = 0;
		double actual_vol;

		/* Invalidate cache to force fresh read from PipeWire */
		volume_invalidate_cache(0); /* speaker */
		volume_invalidate_cache(1); /* headset */

		/* Read actual current state from PipeWire */
		actual_vol = pipewire_volume_percent(&is_headset);
		(void)actual_vol; /* We just want the mute state synced */

		/* Now unmute and set volume */
		set_pipewire_mute(0);

		/* Update all mute state variables to match */
		volume_muted = 0;
		volume_cached_speaker_muted = 0;
		volume_cached_headset_muted = 0;

		/* Invalidate cache again so next read gets fresh data */
		volume_invalidate_cache(0);
		volume_invalidate_cache(1);
	}

	if (set_pipewire_volume(speaker_default) == 0) {
		speaker_active = speaker_default;
		speaker_stored = speaker_default;
		volume_last_speaker_percent = speaker_default;
		volume_last_headset_percent = speaker_default;
	}

	/*
	 * Microphone volume - read actual state from PipeWire first,
	 * then unmute and set to default.  Skip if no mic source exists
	 * (e.g. desktop without a microphone connected).
	 */
	{
		double actual_mic;

		/* Invalidate cache to force fresh read */
		mic_last_read_ms = 0;

		/* Read actual current state from PipeWire */
		actual_mic = pipewire_mic_volume_percent();

		if (actual_mic >= 0.0) {
			/* Mic source exists - unmute and set volume */
			set_pipewire_mic_mute(0);

			mic_muted = 0;
			mic_cached_muted = 0;

			/* Invalidate cache again */
			mic_last_read_ms = 0;

			if (set_pipewire_mic_volume(mic_default) == 0) {
				microphone_active = mic_default;
				microphone_stored = mic_default;
				mic_last_percent = mic_default;
			}
		}
		/* else: no mic source, leave microphone_active = -1.0 */
	}

	applied = 1;
}

/* ── extra net-async globals (public IP / SSID fetch state) ──────── */
pid_t public_ip_pid = -1;
int public_ip_fd = -1;
struct wl_event_source *public_ip_event = NULL;
char public_ip_buf[128];
size_t public_ip_len = 0;
time_t net_public_ip_last;
int ssid_fd = -1;
char ssid_buf[256];
size_t ssid_len = 0;

/* fancontrol.c was removed in the modular split; the fan status task is
 * a no-op so STATUS_TASKS_COUNT and status_tasks[] stay consistent. */
void
refreshstatusfan(void)
{
}

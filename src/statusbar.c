/* statusbar.c - Auto-extracted from nixlytile.c */
#include "nixlytile.h"
#include "client.h"
#include "diag.h"
#include "fetch_async.h"
#include "netsys.h"

/* battery popup: hit ids 0-2 are the power-profile buttons */
#define CHARGE_LIMIT_HIT_BASE 10

void
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

void
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

void
renderclock(StatusModule *module, int bar_height, const char *text)
{
	render_icon_label(module, bar_height, text,
			ensure_clock_icon_buffer, &clock_icon_buf, &clock_icon_w, &clock_icon_h,
			0, statusbar_icon_text_gap_clock, statusbar_fg);
}

void
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

int
status_text_width(const char *text)
{
	int pen_x = 0;
	uint32_t prev_cp = 0;

	if (!text || !*text)
		return 0;

	if (!statusfont.font)
		return (int)strlen(text) * 8;

	for (int i = 0; text[i]; ) {
		long kern_x = 0, kern_y = 0;
		uint32_t cp;
		unsigned char c = (unsigned char)text[i];
		const struct fcft_glyph *glyph;

		/* Decode UTF-8 to Unicode codepoint */
		if ((c & 0x80) == 0) {
			cp = c;
			i += 1;
		/* Trunkert multibyte-sekvens (labels kuttes byte-vis av
		 * snprintf/sanitering): rykk kun forbi bytes som faktisk
		 * finnes, aldri forbi NUL — ellers leser løkka videre i
		 * tilstøtende minne (OOB). */
		} else if ((c & 0xE0) == 0xC0) {
			cp = (c & 0x1F) << 6;
			i += 1;
			if (text[i]) {
				cp |= ((unsigned char)text[i] & 0x3F);
				i += 1;
			}
		} else if ((c & 0xF0) == 0xE0) {
			cp = (c & 0x0F) << 12;
			i += 1;
			if (text[i]) {
				cp |= ((unsigned char)text[i] & 0x3F) << 6;
				i += 1;
				if (text[i]) {
					cp |= ((unsigned char)text[i] & 0x3F);
					i += 1;
				}
			}
		} else if ((c & 0xF8) == 0xF0) {
			cp = (c & 0x07) << 18;
			i += 1;
			if (text[i]) {
				cp |= ((unsigned char)text[i] & 0x3F) << 12;
				i += 1;
				if (text[i]) {
					cp |= ((unsigned char)text[i] & 0x3F) << 6;
					i += 1;
					if (text[i]) {
						cp |= ((unsigned char)text[i] & 0x3F);
						i += 1;
					}
				}
			}
		} else {
			i += 1;
			continue;
		}

		if (prev_cp)
			fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
		pen_x += (int)kern_x;

		glyph = fcft_rasterize_char_utf32(statusfont.font, cp,
				statusbar_font_subpixel);
		if (glyph && glyph->pix) {
			pen_x += glyph->advance.x;
			if (text[i])
				pen_x += statusbar_font_spacing;
		}
		prev_cp = cp;
	}

	return pen_x;
}

int
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

	for (i = 0; text[i]; ) {
		long kern_x = 0, kern_y = 0;
		uint32_t cp;
		unsigned char c = (unsigned char)text[i];

		/* Decode UTF-8 to Unicode codepoint */
		if ((c & 0x80) == 0) {
			/* ASCII (0xxxxxxx) */
			cp = c;
			i += 1;
		} else if ((c & 0xE0) == 0xC0) {
			/* 2-byte sequence (110xxxxx 10xxxxxx).
			 * Rykk kun forbi bytes som finnes — trunkert sekvens
			 * må ikke hoppe forbi NUL (OOB-les). */
			cp = (c & 0x1F) << 6;
			i += 1;
			if (text[i]) {
				cp |= ((unsigned char)text[i] & 0x3F);
				i += 1;
			}
		} else if ((c & 0xF0) == 0xE0) {
			/* 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx) */
			cp = (c & 0x0F) << 12;
			i += 1;
			if (text[i]) {
				cp |= ((unsigned char)text[i] & 0x3F) << 6;
				i += 1;
				if (text[i]) {
					cp |= ((unsigned char)text[i] & 0x3F);
					i += 1;
				}
			}
		} else if ((c & 0xF8) == 0xF0) {
			/* 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx) */
			cp = (c & 0x07) << 18;
			i += 1;
			if (text[i]) {
				cp |= ((unsigned char)text[i] & 0x3F) << 12;
				i += 1;
				if (text[i]) {
					cp |= ((unsigned char)text[i] & 0x3F) << 6;
					i += 1;
					if (text[i]) {
						cp |= ((unsigned char)text[i] & 0x3F);
						i += 1;
					}
				}
			}
		} else {
			/* Invalid UTF-8, skip byte */
			i += 1;
			continue;
		}

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
			if (text[i])
				pen_x += statusbar_font_spacing;
		}
		prev_cp = cp;
	}

	if (tll_length(glyphs) == 0) {
		tll_free(glyphs);
		statusbar_fg_override = prev_fg;
		return 0;
	}

	/* pen_x already holds the exact advance from the glyph loop —
	 * status_text_width() would re-walk the string and redo kerning
	 * and glyph-cache lookups for the same value. */
	text_width = pen_x;
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

void
rendercpu(StatusModule *module, int bar_height, const char *text)
{
	render_icon_label(module, bar_height, text,
			ensure_cpu_icon_buffer, &cpu_icon_buf, &cpu_icon_w, &cpu_icon_h,
			0, statusbar_icon_text_gap_cpu, statusbar_fg);
}

void
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

void
rendernet(StatusModule *module, int bar_height, const char *text)
{
	/* Icon-only module; `text` is a change-key, never drawn. */
	(void)text;
	render_icon_label(module, bar_height, "",
			ensure_net_icon_buffer, &net_icon_buf,
			&net_icon_w, &net_icon_h, 0, statusbar_icon_text_gap,
			statusbar_fg);
}

void
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
			0, statusbar_icon_text_gap_battery, statusbar_fg);
}

void
renderram(StatusModule *module, int bar_height, const char *text)
{
	render_icon_label(module, bar_height, text,
			ensure_ram_icon_buffer, &ram_icon_buf, &ram_icon_w, &ram_icon_h,
			0, statusbar_icon_text_gap_ram, statusbar_fg);
}

void
rendervolume(StatusModule *module, int bar_height, const char *text)
{
	render_icon_label(module, bar_height, text,
			ensure_volume_icon_buffer, &volume_icon_buf, &volume_icon_w, &volume_icon_h,
			0, statusbar_icon_text_gap_volume, volume_text_color);
}

void
rendermic(StatusModule *module, int bar_height, const char *text)
{
	if (!mic_available) {
		if (module && module->tree) {
			clearstatusmodule(module);
			module->width = 0;
			wlr_scene_node_set_enabled(&module->tree->node, 0);
		}
		return;
	}

	render_icon_label(module, bar_height, text,
			ensure_mic_icon_buffer, &mic_icon_buf, &mic_icon_w, &mic_icon_h,
			0, statusbar_icon_text_gap_microphone, mic_text_color);
}

void
renderfan(StatusModule *module, int bar_height, const char *text)
{
	if (fan_pub.total_fans <= 0) {
		if (module && module->tree) {
			clearstatusmodule(module);
			module->width = 0;
			wlr_scene_node_set_enabled(&module->tree->node, 0);
		}
		return;
	}

	render_icon_label(module, bar_height, text,
			ensure_fan_icon_buffer, &fan_icon_buf, &fan_icon_w, &fan_icon_h,
			0, statusbar_icon_text_gap, statusbar_fg);
}

void
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

int
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

void
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

int
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

void
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

int
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

void
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

int
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

void
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

int
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

void
drop_fan_icon_buffer(void)
{
	if (fan_icon_buf) {
		wlr_buffer_drop(fan_icon_buf);
		fan_icon_buf = NULL;
	}
	fan_icon_loaded_h = 0;
	fan_icon_w = fan_icon_h = 0;
	fan_icon_loaded_path[0] = '\0';
}

int
ensure_fan_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = fan_icon_path;

	if (target_h <= 0)
		return -1;

	if (resolve_asset_path(fan_icon_path, resolved, sizeof(resolved)) == 0 && resolved[0])
		path = resolved;

	if (fan_icon_buf && fan_icon_loaded_h == target_h &&
			strncmp(fan_icon_loaded_path, path, sizeof(fan_icon_loaded_path)) == 0)
		return 0;

	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "fan icon: failed to load '%s': %s",
						path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

	drop_fan_icon_buffer();
	fan_icon_buf = buf;
	fan_icon_w = w;
	fan_icon_h = h;
	fan_icon_loaded_h = target_h;
	snprintf(fan_icon_loaded_path, sizeof(fan_icon_loaded_path), "%s", path);
	return 0;
}

void
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

int
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

void
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

int
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

void
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

int
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

int
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

/* Render the SNI system-tray items (real icons) into the traylabel module.
 * Each TrayItem's loaded icon_buf is drawn left-to-right; it->x / it->w are
 * recorded (relative to the module) for click hit-testing. */
void
rendertray(Monitor *m, int bar_height)
{
	StatusModule *module;
	TrayItem *it;
	int padding, gap, x, count = 0, right_edge = 0;

	if (!m || !m->statusbar.traylabel.tree)
		return;

	module = &m->statusbar.traylabel;
	clearstatusmodule(module);
	module->width = 0;
	module->x = 0;

	padding = statusbar_module_padding / 2;
	if (padding < 1)
		padding = 1;
	gap = 6;

	/* First pass: load icons.  Advance is per-icon width + gap, so the
	 * whitespace between two neighbours is always exactly `gap`.  A fixed
	 * slot (widest icon) instead gives even CENTER distance, which reads
	 * as uneven spacing the moment the icons differ in width. */
	wl_list_for_each(it, &tray_items, link) {
		if (it->passive)
			continue;
		if (!it->icon_buf) {
			/* Retry: the item may have registered before its icon
			 * properties were ready (SNI startup race).  NewIcon
			 * signals clear the failed-latch when the app publishes
			 * an icon; this timed retry is only the safety net.
			 * Unconditional retry here would re-run the full icon
			 * theme scan (potentially 1e5 stat() calls) plus D-Bus
			 * round-trips on EVERY layout pass for a permanently
			 * icon-less item. */
			uint64_t now_ms = monotonic_msec();
			if (!it->icon_tried ||
			    (it->icon_failed &&
			     now_ms >= it->icon_retry_not_before_ms)) {
				it->icon_tried = 0;
				it->icon_failed = 0;
				tray_item_load_icon(it);
				if (it->icon_failed)
					it->icon_retry_not_before_ms =
						now_ms + 10000;
			}
		}
	}

	x = padding;
	wl_list_for_each(it, &tray_items, link) {
		struct wlr_scene_buffer *scene_buf;
		int icon_y, content_w;

		if (it->passive || !it->icon_buf || it->icon_w <= 0 || it->icon_h <= 0) {
			it->x = x;
			it->w = 0;
			continue;
		}

		/* Space by visible content, not buffer width: the icons carry
		 * different amounts of transparent margin (tray_measure_icon_insets),
		 * so buffer-width spacing reads as uneven gaps. */
		content_w = it->icon_w - it->icon_pad_l - it->icon_pad_r;
		if (content_w <= 0)
			content_w = it->icon_w;
		/* x tracks the content edge, the buffer is drawn pad_l to its left:
		 * keep the first buffer inside the module. */
		if (count == 0 && x < it->icon_pad_l)
			x = it->icon_pad_l;

		it->x = x;
		it->w = content_w + gap;
		icon_y = MAX(0, (bar_height - it->icon_h) / 2);

		scene_buf = wlr_scene_buffer_create(module->tree, NULL);
		if (scene_buf) {
			wlr_scene_buffer_set_buffer(scene_buf, it->icon_buf);
			wlr_scene_node_set_position(&scene_buf->node,
					x - it->icon_pad_l, icon_y);
		}

		right_edge = MAX(right_edge, x - it->icon_pad_l + it->icon_w);
		x += it->w;
		count++;
	}

	/* Drop the gap trailing the last icon so the module has the same
	 * padding on both sides.  right_edge keeps the last buffer's transparent
	 * margin inside the module background. */
	module->width = count > 0 ? MAX(x - gap + padding, right_edge) : 0;
	if (module->width > 0)
		updatemodulebg(module, module->width, bar_height, statusbar_bg);
	wlr_scene_node_set_enabled(&module->tree->node, module->width > 0);
}

int
cpu_proc_is_critical(pid_t pid, const char *name)
{
	/* Only show real user applications - hide all system/background processes */
	const char *user_apps[] = {
		/* Browsers */
		"firefox", "chromium", "chrome", "brave", "vivaldi", "opera",
		"epiphany", "midori", "qutebrowser", "nyxt", "librewolf", "waterfox",
		"zen", "floorp", "thorium",
		/* File managers */
		"thunar", "nautilus", "dolphin", "nemo", "pcmanfm", "caja",
		"spacefm", "ranger", "lf", "nnn", "vifm",
		/* Terminals */
		"alacritty", "kitty", "foot", "wezterm", "konsole", "gnome-terminal",
		"xfce4-terminal", "terminator", "tilix", "st", "urxvt", "xterm",
		/* Editors/IDEs */
		"code", "codium", "vscodium", "nvim", "vim", "emacs", "gedit", "kate",
		"sublime", "atom", "jetbrains", "idea", "pycharm", "webstorm",
		"clion", "goland", "rider", "android-studio", "zed", "helix",
		/* Creative */
		"blender", "gimp", "inkscape", "krita", "darktable", "rawtherapee",
		"kdenlive", "shotcut", "openshot", "obs", "audacity", "ardour",
		"lmms", "bitwig", "reaper", "godot", "unity",
		/* Office */
		"libreoffice", "soffice", "writer", "calc", "impress", "draw",
		"onlyoffice", "wps", "evince", "okular", "zathura", "mupdf",
		/* Communication */
		"discord", "slack", "teams", "zoom", "skype", "telegram", "signal",
		"element", "fractal", "nheko", "thunderbird", "geary", "evolution",
		/* Media players */
		"vlc", "celluloid", "totem", "parole", "smplayer",
		"spotify", "rhythmbox", "clementine", "strawberry", "elisa",
		/* Games */
		"steam", "lutris", "heroic", "bottles", "wine", "proton",
		"minecraft", "retroarch",
		/* Utilities */
		"keepassxc", "bitwarden", "1password", "syncthing", "transmission",
		"qbittorrent", "deluge", "fragments", "virt-manager", "virtualbox",
		"vmware", "docker", "podman",
		/* Misc apps */
		"calibre", "anki", "logseq", "obsidian", "notion", "joplin",
		"drawio", "figma", "postman", "insomnia", "dbeaver", "pgadmin",
		"ghidra", "wireshark", "burp"
	};

	if (pid <= 1 || pid == getpid())
		return 1;
	if (!name || !*name)
		return 1;

	/* Block kernel threads */
	if (name[0] == '[')
		return 1;

	/* Block common kernel/system process prefixes */
	if (!strncmp(name, "kworker", 7) || !strncmp(name, "ksoftirq", 8) ||
	    !strncmp(name, "kthread", 7) || !strncmp(name, "kswapd", 6) ||
	    !strncmp(name, "rcu_", 4) || !strncmp(name, "migration", 9) ||
	    !strncmp(name, "irq/", 4) || !strncmp(name, "watchdog", 8) ||
	    !strncmp(name, "khugepaged", 10) || !strncmp(name, "kcompact", 8) ||
	    !strncmp(name, "writeback", 9) || !strncmp(name, "kblockd", 7) ||
	    !strncmp(name, "oom_", 4) || !strncmp(name, "kaudit", 6) ||
	    !strncmp(name, "ksmd", 4) || !strncmp(name, "khungtask", 9) ||
	    !strncmp(name, "kdevtmpfs", 9) || !strncmp(name, "netns", 5) ||
	    !strncmp(name, "kintegrity", 10) || !strncmp(name, "bioset", 6) ||
	    !strncmp(name, "crypto", 6) || !strncmp(name, "kstrp", 5) ||
	    !strncmp(name, "charger", 7) || !strncmp(name, "scsi_", 5) ||
	    !strncmp(name, "nvme", 4) || !strncmp(name, "usb-storage", 11) ||
	    !strncmp(name, "jbd2", 4) || !strncmp(name, "ext4", 4) ||
	    !strncmp(name, "btrfs", 5) || !strncmp(name, "xfs", 3) ||
	    !strncmp(name, "dm-", 3) || !strncmp(name, "md", 2) ||
	    !strncmp(name, "loop", 4) || !strncmp(name, "zram", 4) ||
	    !strncmp(name, "cfg80211", 8) || !strncmp(name, "card", 4) ||
	    !strncmp(name, "i915", 4) || !strncmp(name, "amdgpu", 6) ||
	    !strncmp(name, "nvidia", 6) ||
	    /* systemd and related */
	    !strncmp(name, "systemd", 7) || !strncmp(name, "(sd-", 4) ||
	    !strncmp(name, "sd-", 3))
		return 1;

	/* Block by suffix patterns */
	{
		size_t len = strlen(name);
		if (len > 2 && name[len-1] == 'd' && name[len-2] == '-')
			return 1; /* ends with -d (daemon) */
		if (strcasestr(name, "daemon") || strcasestr(name, "helper") ||
		    strcasestr(name, "agent") || strcasestr(name, "server") ||
		    strcasestr(name, "service") || strcasestr(name, "worker") ||
		    strcasestr(name, "watcher") || strcasestr(name, "monitor"))
			return 1;
	}

	/* Whitelist approach: only show known user applications */
	for (size_t i = 0; i < LENGTH(user_apps); i++) {
		if (strcasestr(name, user_apps[i]))
			return 0; /* NOT critical - show it */
	}

	/* Everything else is considered system/background - hide it */
	return 1;
}

int
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

int
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

int
cpu_popup_hover_index(Monitor *m, CpuPopup *p)
{
	int rel_x, rel_y;
	int popup_x;

	if (!m || !p || p->proc_count <= 0 || p->width <= 0 || p->height <= 0)
		return -1;

	popup_x = cpu_popup_clamped_x(m, p);
	rel_x = (int)floor(cursor->x) - m->statusbar.area.x - popup_x;
	rel_y = (int)floor(cursor->y) - m->statusbar.area.y - statusbar_popup_y(m);
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

int
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
			/* found means "a process with this comm exists" —
			 * setting it for every numeric /proc entry made the
			 * pkill fallback fork for names that never existed. */
			found = 1;
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
		/* Fallback: use pkill (non-blocking) */
		if (fork() == 0) {
			setsid();
			execlp("pkill", "pkill", "-9", "-x", name, (char *)NULL);
			_exit(127);
		}
	}
	return killed;
}

/* ".Discord-wrapped" → "Discord": strip NixOS wrapper decorations
 * (leading dots, -wrapped/-wrapper/-bin suffixes — possibly cut short
 * by the 15-char comm limit) and capitalize, for the CPU/RAM popup
 * lists. Display only — kill and dedup keep the raw comm name. */
static const char *
proc_display_name(const char *raw, char *buf, size_t sz)
{
	const char *p = raw;
	char *cut;
	size_t len;

	while (*p == '.')
		p++;
	snprintf(buf, sz, "%s", *p ? p : raw);
	cut = strstr(buf, "-wrap");
	if (cut && cut != buf)
		*cut = '\0';
	len = strlen(buf);
	if (len > 4 && !strcmp(buf + len - 4, "-bin"))
		buf[len - 4] = '\0';
	if (buf[0] >= 'a' && buf[0] <= 'z')
		buf[0] -= 'a' - 'A';
	return buf;
}

/* Parse `top -bn1` output (already fetched) into the popup's proc list. */
static int
parse_top_cpu_processes(CpuPopup *p, FILE *fp)
{
	char line[256];
	int count = 0;
	int lines = 0;

	if (!p || !fp)
		return 0;

	while (fgets(line, sizeof(line), fp) && lines < 128) {
		CpuProcEntry *e;
		pid_t pid = 0;
		char name[64] = {0};
		double cpu = 0.0;
		int existing = -1;
		char user[32], pr[8], ni[8], virt[16], res[16], shr[16], s[4];

		lines++;
		/* top format: PID USER PR NI VIRT RES SHR S %CPU %MEM TIME+ COMMAND */
		if (sscanf(line, "%d %31s %7s %7s %15s %15s %15s %3s %lf %*f %*s %63s",
		           &pid, user, pr, ni, virt, res, shr, s, &cpu, name) < 10)
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

	if (count > 1)
		qsort(p->procs, (size_t)count, sizeof(p->procs[0]), cpu_proc_cmp);
	p->proc_count = count;
	return count;
}

/* Async replacement for the old popen(top) call: the fetch runs in the
 * background and the visible popup re-renders when the data lands, so
 * the cursor never stalls on a popup refresh tick. */
static int cpu_fetch_inflight;

static void
cpu_procs_fetch_done(const char *out, size_t len, void *data)
{
	Monitor *m;
	FILE *fp;

	(void)data;
	cpu_fetch_inflight = 0;
	if (!len)
		return;
	wl_list_for_each(m, &mons, link) {
		CpuPopup *p = &m->statusbar.cpu_popup;

		if (!p->tree || !p->visible)
			continue;
		fp = fmemopen((void *)out, len, "r");
		if (!fp)
			return;
		parse_top_cpu_processes(p, fp);
		fclose(fp);
		rendercpupopup(m);
		return;
	}
}

int
read_top_cpu_processes(CpuPopup *p)
{
	(void)p;
	if (cpu_fetch_inflight)
		return 0;
	if (fetch_async("top -bn1 -o %CPU 2>/dev/null | tail -n +8 | head -50",
			cpu_procs_fetch_done, NULL) == 0)
		cpu_fetch_inflight = 1;
	return 0;
}

void
rendercpupopup(Monitor *m)
{
	CpuPopup *p;
	Card *card;
	CardResult res;
	char value[16], sub[24], k1[16], v1[16], k2[16], v2[16];
	char dispname[64];
	int avg_disp;
	int hover_idx;
	int need_fetch_now = 0;
	uint64_t now;

	if (!m || !m->statusbar.cpu_popup.tree)
		return;

	p = &m->statusbar.cpu_popup;
	hover_idx = p->hover_idx;
	if (hover_idx < -1 || hover_idx >= (int)LENGTH(p->procs))
		hover_idx = -1;
	now = monotonic_msec();

	if (!statusfont.font || cpu_core_count <= 0) {
		p->width = p->height = 0;
		if (p->tree)
			wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->visible = 0;
		return;
	}

	/* Only fetch data after suppress delay has passed */
	if (p->suppress_refresh_until_ms == 0 || now >= p->suppress_refresh_until_ms) {
		if (p->last_fetch_ms == 0 ||
				now < p->last_fetch_ms ||
				now - p->last_fetch_ms >= cpu_popup_refresh_interval_ms)
			p->refresh_data = 1;

		if (p->refresh_data) {
			if (p->last_fetch_ms == 0 || now - p->last_fetch_ms >= 200)
				need_fetch_now = 1;
			if (need_fetch_now) {
				read_top_cpu_processes(p);
				p->last_fetch_ms = now;
			}
			if (p->suppress_refresh_until_ms > 0)
				p->suppress_refresh_until_ms = 0;
			p->refresh_data = 0;
		}
	}

	card = card_begin();
	if (!card)
		return;

	avg_disp = (cpu_last_percent < 1.0) ? 0 :
		(int)lround(cpu_last_percent < 0.0 ? 0.0 : cpu_last_percent);
	snprintf(value, sizeof(value), "%d%%", avg_disp);
	snprintf(sub, sizeof(sub), "%d CORES", cpu_core_count);
	card_header(card, cpu_icon_path, "CPU", sub, value);
	card_gap(card, 6);
	card_gauge(card, avg_disp / 100.0,
			avg_disp >= 90 ? card_col_red : card_col_blue);
	card_gap(card, 2);

	card_section(card, "PER CORE");
	for (int i = 0; i < cpu_core_count; i += 2) {
		double p1 = cpu_last_core_percent[i];

		snprintf(k1, sizeof(k1), "C%d", i);
		if (p1 < 0.0)
			snprintf(v1, sizeof(v1), "--");
		else
			snprintf(v1, sizeof(v1), "%d%%", (int)lround(p1));
		if (i + 1 < cpu_core_count) {
			double p2 = cpu_last_core_percent[i + 1];

			snprintf(k2, sizeof(k2), "C%d", i + 1);
			if (p2 < 0.0)
				snprintf(v2, sizeof(v2), "--");
			else
				snprintf(v2, sizeof(v2), "%d%%", (int)lround(p2));
			card_kv2(card, k1, v1, NULL, k2, v2, NULL);
		} else {
			card_kv2(card, k1, v1, NULL, NULL, NULL, NULL);
		}
	}

	if (p->proc_count > 0) {
		card_section(card, "TOP PROCESSES");
		for (int i = 0; i < p->proc_count; i++) {
			CpuProcEntry *e = &p->procs[i];
			int cpu_disp = (int)lround(e->cpu < 0.0 ? 0.0 : e->cpu);
			char pct[16];

			snprintf(pct, sizeof(pct), "%d%%", cpu_disp);
			if (e->has_kill)
				card_text_btn(card,
						proc_display_name(e->name, dispname,
							sizeof(dispname)),
						pct, card_col_dim,
						"Kill", i, hover_idx == i);
			else
				card_text(card,
						proc_display_name(e->name, dispname,
							sizeof(dispname)),
						pct, card_col_dim);
		}
	}

	if (card_finish(card, &res) != 0)
		return;

	/* Map kill-button hit rects back onto the proc entries the click
	 * and hover paths read. */
	for (int i = 0; i < p->proc_count; i++)
		p->procs[i].kill_x = p->procs[i].kill_y =
			p->procs[i].kill_w = p->procs[i].kill_h = 0;
	for (int i = 0; i < res.nhits; i++) {
		CardHit *hit = &res.hits[i];

		if (hit->id >= 0 && hit->id < p->proc_count) {
			CpuProcEntry *e = &p->procs[hit->id];

			e->kill_x = hit->x;
			e->kill_y = hit->y;
			e->kill_w = hit->w;
			e->kill_h = hit->h;
			e->y = hit->y;
			e->height = hit->h;
		}
	}

	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
	p->last_render_ms = now;
}

int
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
	rel_y = ly - statusbar_popup_y(m);
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

int
ram_proc_cmp(const void *a, const void *b)
{
	const RamProcEntry *pa = a;
	const RamProcEntry *pb = b;

	if (pa->mem_kb < pb->mem_kb)
		return 1;
	if (pa->mem_kb > pb->mem_kb)
		return -1;
	if (pa->pid < pb->pid)
		return -1;
	if (pa->pid > pb->pid)
		return 1;
	return 0;
}

/* Parse `ps -eo pid,rss,comm` output (already fetched) into the popup. */
static int
parse_top_ram_processes(RamPopup *p, FILE *fp)
{
	char line[256];
	int count = 0;
	int lines = 0;
	const unsigned long min_kb = 50 * 1024; /* 50 MB minimum */

	if (!p || !fp)
		return 0;

	while (fgets(line, sizeof(line), fp) && lines < 200 && count < 15) {
		RamProcEntry *e;
		pid_t pid = 0;
		unsigned long rss = 0;
		char name[64] = {0};
		int existing = -1;

		lines++;
		if (sscanf(line, "%d %lu %63s", &pid, &rss, name) != 3)
			continue;
		if (name[0] == '[')
			continue;
		if (rss < min_kb)
			continue;
		/* Hide system processes from RAM popup */
		if (strcmp(name, "nixlytile") == 0 ||
				strcmp(name, "Xwayland") == 0 ||
				strncmp(name, "blueman", 7) == 0)
			continue;

		/* Check for existing entry with same name */
		for (int i = 0; i < count; i++) {
			if (strcmp(p->procs[i].name, name) == 0) {
				existing = i;
				break;
			}
		}

		if (existing >= 0) {
			e = &p->procs[existing];
			e->mem_kb += rss;
			if (rss > e->mem_kb / 2)
				e->pid = pid;
		} else {
			e = &p->procs[count];
			e->pid = pid;
			snprintf(e->name, sizeof(e->name), "%s", name);
			e->mem_kb = rss;
			e->y = e->height = 0;
			e->kill_x = e->kill_y = e->kill_w = e->kill_h = 0;
			e->has_kill = 1;
			count++;
		}
	}

	if (count > 1)
		qsort(p->procs, (size_t)count, sizeof(p->procs[0]), ram_proc_cmp);
	p->proc_count = count;
	return count;
}

/* Async replacement for the old popen(ps) call — same shape as the CPU
 * popup fetch. */
static int ram_fetch_inflight;

static void
ram_procs_fetch_done(const char *out, size_t len, void *data)
{
	Monitor *m;
	FILE *fp;

	(void)data;
	ram_fetch_inflight = 0;
	if (!len)
		return;
	wl_list_for_each(m, &mons, link) {
		RamPopup *p = &m->statusbar.ram_popup;

		if (!p->tree || !p->visible)
			continue;
		fp = fmemopen((void *)out, len, "r");
		if (!fp)
			return;
		parse_top_ram_processes(p, fp);
		fclose(fp);
		renderrampopup(m);
		return;
	}
}

int
read_top_ram_processes(RamPopup *p)
{
	(void)p;
	if (ram_fetch_inflight)
		return 0;
	if (fetch_async("ps -eo pid,rss,comm --no-headers --sort=-rss",
			ram_procs_fetch_done, NULL) == 0)
		ram_fetch_inflight = 1;
	return 0;
}

int
ram_popup_clamped_x(Monitor *m, RamPopup *p)
{
	int popup_x;

	if (!m || !p)
		return 0;

	popup_x = m->statusbar.ram.x;
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

int
ram_popup_hover_index(Monitor *m, RamPopup *p)
{
	double cx, cy;
	int lx, ly;
	int popup_x, rel_x, rel_y;

	if (!m || !p || !p->visible || !cursor)
		return -1;

	/* Statusbar-area coordinates, like cpu_popup_hover_index — the
	 * kill-button rects are relative to the bar area, and monitor-
	 * origin math is off by gappx + statusbar_top_gap. */
	popup_x = ram_popup_clamped_x(m, p);
	cx = cursor->x;
	cy = cursor->y;
	lx = (int)round(cx) - m->statusbar.area.x;
	ly = (int)round(cy) - m->statusbar.area.y;
	rel_x = lx - popup_x;
	rel_y = ly - statusbar_popup_y(m);

	for (int i = 0; i < p->proc_count; i++) {
		RamProcEntry *e = &p->procs[i];
		if (!e->has_kill || e->kill_w <= 0 || e->kill_h <= 0)
			continue;
		if (rel_x >= e->kill_x && rel_x < e->kill_x + e->kill_w &&
				rel_y >= e->kill_y && rel_y < e->kill_y + e->kill_h)
			return i;
	}
	return -1;
}

void
format_mem_size(unsigned long kb, char *buf, size_t bufsz)
{
	if (kb >= 1024 * 1024)
		snprintf(buf, bufsz, "%.1fG", kb / (1024.0 * 1024.0));
	else if (kb >= 1024)
		snprintf(buf, bufsz, "%.0fM", kb / 1024.0);
	else
		snprintf(buf, bufsz, "%luK", kb);
}

void
renderrampopup(Monitor *m)
{
	RamPopup *p;
	Card *card;
	CardResult res;
	char value[16], v1[32], v2[32];
	int hover_idx;
	int need_fetch_now = 0;
	double total_mb = -1.0, avail_mb = -1.0, used_mb;
	double used_frac = -1.0;
	uint64_t now;

	if (!m || !m->statusbar.ram_popup.tree)
		return;

	p = &m->statusbar.ram_popup;
	hover_idx = p->hover_idx;
	if (hover_idx < -1 || hover_idx >= (int)LENGTH(p->procs))
		hover_idx = -1;
	now = monotonic_msec();

	if (!statusfont.font) {
		p->width = p->height = 0;
		if (p->tree)
			wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->visible = 0;
		return;
	}

	/* Only fetch data after suppress delay has passed */
	if (p->suppress_refresh_until_ms == 0 || now >= p->suppress_refresh_until_ms) {
		if (p->last_fetch_ms == 0 ||
				now < p->last_fetch_ms ||
				now - p->last_fetch_ms >= ram_popup_refresh_interval_ms)
			p->refresh_data = 1;

		if (p->refresh_data) {
			if (p->last_fetch_ms == 0 || now - p->last_fetch_ms >= 200)
				need_fetch_now = 1;
			if (need_fetch_now) {
				read_top_ram_processes(p);
				p->last_fetch_ms = now;
			}
			if (p->suppress_refresh_until_ms > 0)
				p->suppress_refresh_until_ms = 0;
			p->refresh_data = 0;
		}
	}

	/* Total from /proc/meminfo; used is the bar module's last value so
	 * bar and popup always show the exact same number. */
	{
		FILE *fp = fopen("/proc/meminfo", "r");
		char line[128];
		unsigned long long kb;

		if (fp) {
			while (fgets(line, sizeof(line), fp)) {
				if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) {
					total_mb = kb / 1024.0;
					break;
				}
			}
			fclose(fp);
		}
	}
	used_mb = ram_last_mb >= 0.0 ? ram_last_mb : ramused_mb();
	if (total_mb >= 0.0 && used_mb >= 0.0 && used_mb <= total_mb)
		avail_mb = total_mb - used_mb;
	if (total_mb > 0.0 && used_mb >= 0.0)
		used_frac = used_mb / total_mb;

	card = card_begin();
	if (!card)
		return;

	if (used_frac >= 0.0)
		snprintf(value, sizeof(value), "%.0f%%", used_frac * 100.0);
	else
		snprintf(value, sizeof(value), "--");
	card_header(card, ram_icon_path, "Memory", "SYSTEM RAM", value);
	card_gap(card, 6);
	card_gauge(card, used_frac >= 0.0 ? used_frac : 0.0,
			used_frac >= 0.9 ? card_col_red : card_col_blue);
	card_gap(card, 6);

	if (used_mb >= 0.0)
		snprintf(v1, sizeof(v1), "%.1fGB", used_mb / 1024.0);
	else
		snprintf(v1, sizeof(v1), "--");
	if (total_mb >= 0.0)
		snprintf(v2, sizeof(v2), "%.1fGB", total_mb / 1024.0);
	else
		snprintf(v2, sizeof(v2), "--");
	card_kv2(card, "In use", v1, NULL, "Total", v2, NULL);
	if (avail_mb >= 0.0) {
		snprintf(v1, sizeof(v1), "%.1fGB", avail_mb / 1024.0);
		card_kv2(card, "Available", v1, card_col_green, NULL, NULL, NULL);
	}

	if (p->proc_count > 0) {
		card_section(card, "TOP PROCESSES");
		for (int i = 0; i < p->proc_count; i++) {
			RamProcEntry *e = &p->procs[i];
			char amt[16], dispname[64];

			if (e->mem_kb >= 1024 * 1024)
				snprintf(amt, sizeof(amt), "%.1fGB",
						e->mem_kb / (1024.0 * 1024.0));
			else
				snprintf(amt, sizeof(amt), "%luMB",
						e->mem_kb / 1024);
			if (e->has_kill)
				card_text_btn(card,
						proc_display_name(e->name, dispname,
							sizeof(dispname)),
						amt, card_col_dim,
						"Kill", i, hover_idx == i);
			else
				card_text(card,
						proc_display_name(e->name, dispname,
							sizeof(dispname)),
						amt, card_col_dim);
		}
	} else if (p->suppress_refresh_until_ms > 0 &&
			now < p->suppress_refresh_until_ms) {
		card_section(card, "TOP PROCESSES");
		card_text(card, "Loading...", NULL, NULL);
	}

	if (card_finish(card, &res) != 0)
		return;

	for (int i = 0; i < p->proc_count; i++)
		p->procs[i].kill_x = p->procs[i].kill_y =
			p->procs[i].kill_w = p->procs[i].kill_h = 0;
	for (int i = 0; i < res.nhits; i++) {
		CardHit *hit = &res.hits[i];

		if (hit->id >= 0 && hit->id < p->proc_count) {
			RamProcEntry *e = &p->procs[hit->id];

			e->kill_x = hit->x;
			e->kill_y = hit->y;
			e->kill_w = hit->w;
			e->kill_h = hit->h;
			e->y = hit->y;
			e->height = hit->h;
		}
	}

	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
	p->last_render_ms = now;
}

int
ram_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	RamPopup *p;
	int popup_x, rel_x, rel_y;

	if (!m || !m->statusbar.ram_popup.visible || button != BTN_LEFT)
		return 0;

	p = &m->statusbar.ram_popup;
	popup_x = ram_popup_clamped_x(m, p);

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
	rel_y = ly - statusbar_popup_y(m);
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;

	for (int i = 0; i < p->proc_count; i++) {
		RamProcEntry *e = &p->procs[i];
		if (!e->has_kill || e->kill_w <= 0 || e->kill_h <= 0)
			continue;
		if (rel_x >= e->kill_x && rel_x < e->kill_x + e->kill_w &&
				rel_y >= e->kill_y && rel_y < e->kill_y + e->kill_h) {
			if (cpu_proc_is_critical(e->pid, e->name))
				return 1;
			if (kill_processes_with_name(e->name) == 0) {
				if (kill(e->pid, SIGKILL) != 0)
					wlr_log(WLR_ERROR, "ram popup: kill %d failed: %s",
							e->pid, strerror(errno));
			}
			p->last_fetch_ms = monotonic_msec();
			p->suppress_refresh_until_ms = p->last_fetch_ms + 2000;
			p->refresh_data = 0;
			schedule_ram_popup_refresh(2000);
			refreshstatusram();
			return 1;
		}
	}

	return 0;
}

int
ram_popup_refresh_timeout(void *data)
{
	Monitor *m;
	int any_visible = 0;

	(void)data;

	wl_list_for_each(m, &mons, link) {
		RamPopup *p = &m->statusbar.ram_popup;
		if (!p || !p->tree || !p->visible)
			continue;
		p->suppress_refresh_until_ms = 0;
		p->refresh_data = 1;
		renderrampopup(m);
		any_visible = 1;
	}

	if (any_visible)
		wl_event_source_timer_update(ram_popup_refresh_timer, ram_popup_refresh_interval_ms);

	return 0;
}

void
schedule_ram_popup_refresh(uint32_t ms)
{
	if (!event_loop)
		return;
	if (!ram_popup_refresh_timer)
		ram_popup_refresh_timer = wl_event_loop_add_timer(event_loop,
				ram_popup_refresh_timeout, NULL);
	if (ram_popup_refresh_timer)
		wl_event_source_timer_update(ram_popup_refresh_timer, ms);
}

int readulong(const char *path, unsigned long long *out);

/* Battery Popup Functions */
static void
read_battery_info(BatteryPopup *p)
{
	BattSnapshot s;

	if (!p || !battery_available)
		return;
	/* All sysfs reads happen on battwatch's worker thread — an EC-backed
	 * status read blocks ~100ms and used to freeze the cursor whenever
	 * this popup fetched.  Here we only copy the latest snapshot. */
	if (!battwatch_get(&s) || !s.available)
		return;

	p->charging = (strcmp(s.status, "Charging") == 0 ||
			strcmp(s.status, "Full") == 0);
	p->actively_charging = (strcmp(s.status, "Charging") == 0);
	if (strcmp(s.status, "Charging") == 0)
		snprintf(p->state, sizeof(p->state), "Charging");
	else if (strcmp(s.status, "Full") == 0)
		snprintf(p->state, sizeof(p->state), "Full");
	else if (strcmp(s.status, "Not charging") == 0)
		snprintf(p->state, sizeof(p->state), "Holding");
	else
		snprintf(p->state, sizeof(p->state), "Draining");

	p->percent = s.percent;
	p->voltage_v = s.voltage_v;
	p->power_w = s.power_w;
	p->time_remaining_h = s.time_remaining_h;
	p->design_wh = s.design_wh;
	p->full_wh = s.full_wh;
	p->cycles = s.cycles;
	p->thr_start = s.thr_start;
	p->thr_end = s.thr_end;
	snprintf(p->profile, sizeof(p->profile), "%s", s.profile);
	snprintf(p->choices, sizeof(p->choices), "%s", s.choices);
	p->has_profile = s.has_profile;
	p->profile_backend = s.profile_backend;
	/* Ask the worker for a fresh sample so an open popup shows live
	 * draw/state on its next refresh. */
	battwatch_refresh();
}

int
battery_popup_clamped_x(Monitor *m, BatteryPopup *p)
{
	int popup_x;

	if (!m || !p)
		return 0;

	popup_x = m->statusbar.battery.x;
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

/* Map the current profile string (either backend) to a button index
 * (0 = power-saver, 1 = balanced, 2 = performance), -1 if unknown. */
int
battery_profile_index(const char *profile)
{
	if (!profile || !*profile)
		return -1;
	if (!strcmp(profile, "low-power") || !strcmp(profile, "power-saver")
			|| !strcmp(profile, "quiet") || !strcmp(profile, "eco"))
		return 0;
	if (!strcmp(profile, "balanced") || !strcmp(profile, "balanced-performance")
			|| !strcmp(profile, "comfort"))
		return 1;
	if (!strcmp(profile, "performance") || !strcmp(profile, "sport")
			|| !strcmp(profile, "turbo"))
		return 2;
	return -1;
}

void
renderbatterypopup(Monitor *m)
{
	BatteryPopup *p;
	Card *card;
	CardResult res;
	char value[16], v1[32], v2[32];
	const float *accent;
	const float *statecol;
	uint64_t now;

	if (!m || !m->statusbar.battery_popup.tree)
		return;

	p = &m->statusbar.battery_popup;
	now = monotonic_msec();

	if (!statusfont.font || !battery_available) {
		p->width = p->height = 0;
		if (p->tree)
			wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->visible = 0;
		return;
	}

	/* Fetch data only when refresh_data flag is set */
	if (p->refresh_data) {
		read_battery_info(p);
		p->last_fetch_ms = now;
		p->refresh_data = 0;
	}

	card = card_begin();
	if (!card)
		return;

	if (p->percent >= 0)
		snprintf(value, sizeof(value), "%.0f%%", p->percent);
	else
		snprintf(value, sizeof(value), "--");
	{
		const char *dev = strrchr(battery_device_dir, '/');
		char sub[32];
		size_t i;

		snprintf(sub, sizeof(sub), "%s", dev && dev[1] ? dev + 1 : "BATTERY");
		for (i = 0; sub[i]; i++)
			sub[i] = (char)toupper((unsigned char)sub[i]);
		card_header(card, battery_icon_path, "Battery", sub, value);
	}

	accent = card_col_fg;
	if (p->actively_charging)
		accent = card_col_green;
	else if (p->percent >= 0 && p->percent <= 15.0)
		accent = card_col_red;
	card_gap(card, 6);
	card_gauge(card, p->percent >= 0 ? p->percent / 100.0 : 0.0, accent);
	card_gap(card, 6);

	if (p->design_wh > 0)
		snprintf(v1, sizeof(v1), "%.0fWh", p->design_wh);
	else
		snprintf(v1, sizeof(v1), "--");
	if (p->thr_start >= 0 && p->thr_end >= 0)
		snprintf(v2, sizeof(v2), "%d-%d%%", p->thr_start, p->thr_end);
	else if (p->thr_end >= 0)
		snprintf(v2, sizeof(v2), "%d%%", p->thr_end);
	else
		snprintf(v2, sizeof(v2), "100%%");
	card_kv2(card, "Battery size", v1, NULL, "Charge limit", v2, NULL);

	if (p->cycles >= 0)
		snprintf(v1, sizeof(v1), "%d", p->cycles);
	else
		snprintf(v1, sizeof(v1), "--");
	statecol = card_col_fg;
	if (!strcmp(p->state, "Holding"))
		statecol = card_col_yellow;
	else if (!strcmp(p->state, "Charging") || !strcmp(p->state, "Full"))
		statecol = card_col_green;
	card_kv2(card, "Charge cycles", v1, NULL, "Battery state",
			p->state[0] ? p->state : "--", statecol);

	if (p->power_w > 0) {
		snprintf(v1, sizeof(v1), "%.1f W", p->power_w);
		v2[0] = '\0';
		if (!p->charging && p->time_remaining_h > 0)
			snprintf(v2, sizeof(v2), "%dh %dm",
					(int)p->time_remaining_h,
					(int)((p->time_remaining_h -
						(int)p->time_remaining_h) * 60));
		else if (p->full_wh > 0 && p->design_wh > 0)
			snprintf(v2, sizeof(v2), "%.0f%%",
					100.0 * p->full_wh / p->design_wh);
		card_kv2(card, "Power draw", v1, NULL,
				(!p->charging && p->time_remaining_h > 0) ?
				"Time left" : "Health", v2[0] ? v2 : "--", NULL);
	}

	if (p->has_profile) {
		static const char *labels[3] =
			{ "Power-saver", "Balanced", "Performance" };

		card_section(card, "POWER PROFILE");
		card_buttons(card, labels, NULL, 3,
				battery_profile_index(p->profile),
				p->btn_hover, 0);
	}

	/* Charge limit buttons — only when the battery has the sysfs knob */
	if (p->thr_end >= 0) {
		static const char *limits[3] = { "80%", "90%", "100%" };
		int active = p->thr_end >= 100 ? 2 : p->thr_end >= 90 ? 1 : 0;

		card_section(card, "CHARGE LIMIT");
		card_buttons(card, limits, NULL, 3, active, p->btn_hover,
				CHARGE_LIMIT_HIT_BASE);
	}

	if (card_finish(card, &res) != 0)
		return;
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;

	p->last_render_ms = now;
}


void
updatebatteryhover(Monitor *m, double cx, double cy)
{
	int lx, ly;
	int inside = 0;
	int popup_hover = 0;
	int was_visible;
	BatteryPopup *p;
	int popup_x;
	uint64_t now = monotonic_msec();
	int need_refresh = 0;
	int stale_refresh = 0;

	if (!m || !m->showbar || !m->statusbar.battery.tree || !m->statusbar.battery_popup.tree || !battery_available) {
		if (m && m->statusbar.battery_popup.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.battery_popup.tree->node, 0);
			m->statusbar.battery_popup.visible = 0;
		}
		return;
	}

	p = &m->statusbar.battery_popup;
	lx = (int)floor(cx) - m->statusbar.area.x;
	ly = (int)floor(cy) - m->statusbar.area.y;

	popup_x = battery_popup_clamped_x(m, p);

	/* Check if hovering over popup */
	if (p->visible && p->width > 0 && p->height > 0 &&
			lx >= popup_x &&
			lx < popup_x + p->width &&
			ly >= m->statusbar.area.height &&
			ly < statusbar_popup_y(m) + p->height) {
		popup_hover = 1;
	}

	/* Check if hovering over battery module */
	if (lx >= m->statusbar.battery.x &&
			lx < m->statusbar.battery.x + m->statusbar.battery.width &&
			ly >= 0 && ly < m->statusbar.area.height &&
			m->statusbar.battery.width > 0) {
		inside = 1;
	} else if (popup_hover) {
		inside = 1;
	}

	was_visible = p->visible;

	if (inside) {
		/* Track when hover started for delay */
		if (p->hover_start_ms == 0)
			p->hover_start_ms = now;

		/* Wait 300ms before showing popup */
		if (!was_visible && (now - p->hover_start_ms) < 300) {
			/* Schedule timer to check again after remaining delay */
			uint64_t remaining = 300 - (now - p->hover_start_ms);
			schedule_popup_delay(remaining + 1);
			return;
		}

		if (!was_visible) {
			/* Short delay to avoid flicker on quick mouse movements */
			p->suppress_refresh_until_ms = now + 100;
			p->refresh_data = 1; /* Trigger immediate fetch when delay passes */
		}
		p->visible = 1;
		wlr_scene_node_set_enabled(&p->tree->node, 1);
		wlr_scene_node_set_position(&p->tree->node,
				popup_x, statusbar_popup_y(m));

		/* Check if refresh is needed (every 500ms after delay) */
		stale_refresh = (p->last_fetch_ms == 0 ||
				now < p->last_fetch_ms ||
				(now - p->last_fetch_ms) >= 500);
		need_refresh = stale_refresh &&
				(p->suppress_refresh_until_ms == 0 ||
				 now >= p->suppress_refresh_until_ms);

		if (need_refresh)
			p->refresh_data = 1;

		/* Profile-button hover feedback */
		{
			int new_hover = -1;

			if (popup_hover) {
				int rel_x = lx - popup_x;
				int rel_y = ly - statusbar_popup_y(m);

				for (int i = 0; i < p->nhits; i++) {
					CardHit *hit = &p->hits[i];
					if (rel_x >= hit->x && rel_x < hit->x + hit->w &&
							rel_y >= hit->y && rel_y < hit->y + hit->h) {
						new_hover = hit->id;
						break;
					}
				}
			}
			if (new_hover != p->btn_hover) {
				p->btn_hover = new_hover;
				if (was_visible && !need_refresh &&
						p->last_render_ms != 0) {
					renderbatterypopup(m);
					p->last_render_ms = now;
				}
			}
		}

		/* Render only when the data changed or the popup just
		 * appeared — the old 100 ms clause re-rasterized identical
		 * content 10x per second for the whole hover. */
		if (!was_visible || need_refresh || p->last_render_ms == 0) {
			renderbatterypopup(m);
			p->last_render_ms = now;
			/* first render establishes the real width — re-clamp so
			 * the card never hangs past the screen edge */
			wlr_scene_node_set_position(&p->tree->node,
					battery_popup_clamped_x(m, p),
					statusbar_popup_y(m));
		}
		if (!was_visible)
			popup_view_show(&p->view);
	} else if (p->visible || p->hover_start_ms != 0) {
		p->visible = 0;
		p->suppress_refresh_until_ms = 0;
		p->hover_start_ms = 0;
		p->btn_hover = -1;
		popup_view_hide(&p->view);
		wlr_scene_node_set_enabled(&p->tree->node, 0);
	}
}

/* 1 if `name` appears as a whole space-separated token in `choices`. */
static int
profile_choice_present(const char *choices, const char *name)
{
	size_t nlen = strlen(name);
	const char *s = choices;

	while ((s = strstr(s, name))) {
		int at_start = (s == choices || s[-1] == ' ');
		int at_end = (s[nlen] == '\0' || s[nlen] == ' ');

		if (at_start && at_end)
			return 1;
		s += nlen;
	}
	return 0;
}

/* Button index -> the profile name this firmware actually offers. */
static const char *
profile_name_for_button(BatteryPopup *p, int idx)
{
	static const char *acpi_cands[3][2] = {
		{ "low-power",   "quiet" },
		{ "balanced",    "balanced-performance" },
		{ "performance", NULL },
	};
	/* Performance prefers turbo over sport — highest MSI mode wins */
	static const char *msi_cands[3][2] = {
		{ "eco",     NULL },
		{ "comfort", NULL },
		{ "turbo",   "sport" },
	};
	const char *(*cands)[2] =
		p->profile_backend == PROFILE_BACKEND_MSI_EC ?
		msi_cands : acpi_cands;

	if (idx < 0 || idx > 2)
		return NULL;
	if (!p->choices[0])
		return cands[idx][0];
	for (int i = 0; i < 2 && cands[idx][i]; i++)
		if (profile_choice_present(p->choices, cands[idx][i]))
			return cands[idx][i];
	return NULL;
}

/* sysfs file the active backend is controlled through */
static const char *
profile_backend_path(BatteryPopup *p)
{
	switch (p->profile_backend) {
	case PROFILE_BACKEND_ACPI:
		return "/sys/firmware/acpi/platform_profile";
	case PROFILE_BACKEND_MSI_EC:
		return "/sys/devices/platform/msi-ec/shift_mode";
	default:
		return NULL;
	}
}

/* Left click on the battery popup: power-profile buttons. Writes the
 * ACPI platform profile directly (sysfs made group-writable by a
 * nixlyos boot rule). Returns 1 when the click was consumed. */
int
battery_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	BatteryPopup *p;
	int rel_x, rel_y, popup_x;

	if (!m || !m->statusbar.battery_popup.visible || button != BTN_LEFT)
		return 0;

	p = &m->statusbar.battery_popup;
	if (!p->tree || p->width <= 0 || p->height <= 0)
		return 0;

	popup_x = battery_popup_clamped_x(m, p);
	rel_x = lx - popup_x;
	rel_y = ly - statusbar_popup_y(m);
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;

	for (int i = 0; i < p->nhits; i++) {
		CardHit *hit = &p->hits[i];

		if (rel_x < hit->x || rel_x >= hit->x + hit->w ||
				rel_y < hit->y || rel_y >= hit->y + hit->h)
			continue;
		if (hit->id >= CHARGE_LIMIT_HIT_BASE &&
				hit->id <= CHARGE_LIMIT_HIT_BASE + 2) {
			int pct = 80 + (hit->id - CHARGE_LIMIT_HIT_BASE) * 10;

			if (charge_limit_set(pct) == 0)
				p->thr_end = pct;
			renderbatterypopup(m);
			/* refetch shortly so a rejected write corrects itself */
			p->last_fetch_ms = 0;
			schedule_popup_delay(400);
			return 1;
		}
		if (hit->id < 0 || hit->id > 2)
			return 1;
		{
			const char *name = profile_name_for_button(p, hit->id);
			const char *path = profile_backend_path(p);
			int fd;
			int err = 0;
			ssize_t n = -1;

			if (!name || !path)
				return 1;
			fd = open(path, O_WRONLY);
			if (fd >= 0) {
				n = write(fd, name, strlen(name));
				err = n < 0 ? errno : 0;
				close(fd);
			} else {
				err = errno;
			}
			if (n < 0) {
				wlr_log(WLR_ERROR,
					"battery popup: writing '%s' to %s failed: %s",
					name, path, strerror(err));
				return 1;
			}
			/* Show the chosen profile immediately, then refetch
			 * shortly after so a rejected switch corrects itself. */
			snprintf(p->profile, sizeof(p->profile), "%s", name);
			renderbatterypopup(m);
			p->last_fetch_ms = 0;
			schedule_popup_delay(400);
		}
		return 1;
	}

	return 1; /* click landed inside the popup — always consume */
}

void
renderworkspaces(Monitor *m, StatusModule *module, int bar_height)
{
	Workspace *ws;
	Client *c;
	int padding, inner, spacing, outer_pad;
	int box_h, box_y, total_w = 0;
	int x, count = 0, pos = -1;
	struct wlr_scene_buffer *scene_buf;
	struct wlr_buffer *buffer;
	struct { int tag; int active; } shown[TAGCOUNT];
	int shown_count = 0, n;
	uint64_t sig;

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
	if (module->hover_tag < -1 || module->hover_tag >= TAGCOUNT)
		module->hover_tag = -1;

	box_h = MAX(1, MIN(bar_height - 2, statusfont.height + inner * 2 + 2));
	box_y = (bar_height - box_h) / 2;

	/* One box per Niri-style workspace, in stack order.  Shown when the
	 * workspace has content (tiles or a fullscreen client) or is active.
	 * box_tag stores the workspace's position in m->workspaces. */
	wl_list_for_each(ws, &m->workspaces, link) {
		int active, occupied;

		if (++pos >= TAGCOUNT)
			break;

		occupied = workspace_has_clients(ws);
		if (!occupied) {
			wl_list_for_each(c, &clients, link) {
				if (c->fs_ws == ws) {
					occupied = 1;
					break;
				}
			}
		}
		active = (ws == m->active_ws);
		if (!occupied && !active && pos != 0)
			continue;

		shown[shown_count].tag = pos;
		shown[shown_count].active = active;
		shown_count++;
	}

	/* Hash of everything the row will draw.  arrangelayers runs on every
	 * layer-surface commit and on every frame of the bar's slide, and a
	 * re-render tears down the digit textures and uploads new ones — the
	 * boxes are scene rects and come back instantly, the digits do not,
	 * so a redundant re-render shows numberless boxes for a few frames.
	 * Identical hash means the row on screen is already the right one. */
	sig = 1469598103934665603ULL;
#define SIG_MIX(v) do { sig = (sig ^ (uint64_t)(int64_t)(v)) * 1099511628211ULL; } while (0)
	SIG_MIX(bar_height);
	SIG_MIX(box_h);
	SIG_MIX(box_y);
	SIG_MIX(statusfont.height);
	SIG_MIX(inner);
	SIG_MIX(spacing);
	SIG_MIX(outer_pad);
	SIG_MIX(shown_count);
	for (n = 0; n < shown_count; n++) {
		SIG_MIX(shown[n].tag);
		SIG_MIX(shown[n].active);
		SIG_MIX(lround(module->hover_alpha[shown[n].tag] * 255.0f));
	}
	for (n = 0; n < 4; n++) {
		SIG_MIX(lround(statusbar_tag_bg[n] * 255.0f));
		SIG_MIX(lround(statusbar_tag_active_bg[n] * 255.0f));
		SIG_MIX(lround(statusbar_tag_hover_bg[n] * 255.0f));
	}
#undef SIG_MIX

	if (sig == module->render_sig && module->width > 0)
		return;
	module->render_sig = sig;

	module->box_count = 0;
	module->tagmask = 0;
	clearstatusmodule(module);
	x = outer_pad;

	for (n = 0; n < shown_count; n++) {
		const struct fcft_glyph *glyph;
		int min_x, max_x, min_y, max_y;
		int text_w, text_h, box_w;
		int origin_x, origin_y;
		const float *bgcol;
		int active = shown[n].active;
		int i = shown[n].tag;

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

int
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

		/* Aggregatlinjen "cpu  ..." (uten indeks) matcher også cpu%d —
		 * første jiffies-tall blir da tolket som kjerneindeks. Krev
		 * siffer rett etter "cpu". */
		if (line[3] < '0' || line[3] > '9')
			continue;
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

double
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

double
ramused_mb(void)
{
	/* Sum of process RSS — excludes kernel slab, page cache and other
	 * reclaimable memory, so the bar shows what processes actually hold. */
	DIR *dir;
	struct dirent *de;
	unsigned long long pages = 0;
	long page_kb = sysconf(_SC_PAGESIZE) / 1024;

	dir = opendir("/proc");
	if (!dir)
		return -1.0;

	while ((de = readdir(dir))) {
		char path[64];
		FILE *fp;
		unsigned long long resident;

		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/statm", de->d_name);
		fp = fopen(path, "r");
		if (!fp)
			continue;
		if (fscanf(fp, "%*s %llu", &resident) == 1)
			pages += resident;
		fclose(fp);
	}
	closedir(dir);

	if (pages == 0)
		return -1.0;

	return (double)pages * page_kb / 1024.0;
}

int
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

int
findbluetoothdevice(void)
{
	DIR *dir;
	struct dirent *ent;
	int found = 0;

	dir = opendir("/sys/class/bluetooth");
	if (!dir)
		return 0;

	while ((ent = readdir(dir))) {
		if (ent->d_name[0] == '.')
			continue;
		/* Found at least one bluetooth adapter */
		found = 1;
		break;
	}

	closedir(dir);
	return found;
}

int
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

int
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

int
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

int
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

int
readssid(const char *iface, char *out, size_t len)
{
	/* Deprecated: synchronous SSID lookup removed to avoid blocking */
	(void)iface;
	if (!out || len == 0)
		return 0;
	out[0] = '\0';
	return 0;
}

int
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

double
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

		/* %15: bounds — name er IF_NAMESIZE(16); uten bredde smadrer
		 * en lang linje stacken. */
		if (sscanf(line, " %15[^:]: %*[^ ] %lf", name, &link) == 2) {
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

void
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

const char *
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

void
set_net_icon_path(const char *path)
{
	if (!path || !*path)
		path = net_icon_no_conn_resolved[0] ? net_icon_no_conn_resolved : net_icon_no_conn;

	if (strncmp(net_icon_path, path, sizeof(net_icon_path)) != 0) {
		snprintf(net_icon_path, sizeof(net_icon_path), "%s", path);
	}
}

double
net_bytes_to_rate(unsigned long long cur, unsigned long long prev, double elapsed)
{
	if (elapsed <= 0.0 || cur < prev)
		return -1.0;
	return (double)(cur - prev) / elapsed;
}

int
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

int
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

double
backlight_percent(void)
{
	unsigned long long cur, max;
	double percent;

	/* sysfs first — two file reads, no fork.  The brightnessctl /
	 * light fallbacks each cost a fork+exec and this runs on every
	 * brightness scroll notch and 45 s refresh tick. */
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

int
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

	/* Use external tools (non-blocking) */
	{
		char arg[32];
		snprintf(arg, sizeof(arg), "%.2f%%", percent);

		if (fork() == 0) {
			setsid();
			execlp("brightnessctl", "brightnessctl", "set", arg, (char *)NULL);
			/* If brightnessctl fails, try light */
			snprintf(arg, sizeof(arg), "%.2f", percent);
			execlp("light", "light", "-S", arg, (char *)NULL);
			_exit(127);
		}
		light_cached_percent = percent;
		return 0;
	}
}

int
set_backlight_relative(double delta_percent)
{
	char arg[32];
	char light_arg[32];
	double cur;

	if (delta_percent == 0.0)
		return 0;

	/* Update cached value */
	cur = light_cached_percent >= 0.0 ? light_cached_percent : backlight_percent();
	if (cur >= 0.0) {
		double target = cur + delta_percent;
		if (target < 0.0)
			target = 0.0;
		if (target > 100.0)
			target = 100.0;
		light_cached_percent = target;
	}

	/* Use external tools (non-blocking) */
	if (delta_percent > 0) {
		snprintf(arg, sizeof(arg), "+%.2f%%", delta_percent);
		snprintf(light_arg, sizeof(light_arg), "%.2f", delta_percent);
	} else {
		snprintf(arg, sizeof(arg), "%.2f%%-", -delta_percent);
		snprintf(light_arg, sizeof(light_arg), "%.2f", -delta_percent);
	}

	if (fork() == 0) {
		setsid();
		execlp("brightnessctl", "brightnessctl", "set", arg, (char *)NULL);
		/* If brightnessctl fails, try light */
		execlp("light", "light", delta_percent > 0 ? "-A" : "-U", light_arg, (char *)NULL);
		_exit(127);
	}

	return 0;
}

double
battery_percent(void)
{
	BattSnapshot s;

	/* Cached snapshot from battwatch's worker thread — reading the
	 * capacity file here would walk EC-backed ACPI on some laptops. */
	if (!battery_available || !battwatch_get(&s) || !s.available)
		return -1.0;
	return s.percent;
}

double
volume_last_for_type(int is_headset)
{
	return is_headset ? volume_last_headset_percent : volume_last_speaker_percent;
}

void
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

/* Cached result of the sink-type probe — each probe costs 1-2
 * popen(wpctl) fork/execs and it's called from every scroll notch
 * and volume refresh. */
static int headset_probe_cached = -1;
static uint64_t headset_probe_ms;

void
volume_invalidate_cache(int is_headset)
{
	if (is_headset)
		volume_last_read_headset_ms = 0;
	else
		volume_last_read_speaker_ms = 0;
	headset_probe_cached = -1;
}

static const char *headset_kw[] = {
	"headset", "headphone", "headphones", "earbud", "earbuds",
	"earphone", "handsfree", "bluez", "bluetooth", "a2dp",
	"hfp", "hsp", "head-unit"
};

static int
has_headset_kw(const char *s)
{
	for (size_t i = 0; i < LENGTH(headset_kw); i++)
		if (strcasestr(s, headset_kw[i]))
			return 1;
	return 0;
}

/* Exported for audio_devices.c's async headset autoselect. */
int
audio_line_is_headset(const char *line)
{
	return has_headset_kw(line);
}

/* ── non-blocking wpctl state for the popup render path ──────────────
 * The popup re-render runs on cursor motion and refresh timers; a
 * popen(wpctl) there freezes the cursor for the duration of the fork +
 * PipeWire round-trip.  These variants always return the cached state
 * immediately and refresh it through fetch_async(); the popup re-renders
 * via audio_popup_data_arrived() when the answer lands. */
static int vol_fetch_inflight, mic_fetch_inflight, headset_fetch_inflight;
static uint64_t vol_fetch_start_ms, mic_fetch_start_ms;

static void
headset_status_fetch_done(const char *out, size_t len, void *data)
{
	FILE *fp;
	char line[512];
	int headset = 0;

	(void)data;
	headset_fetch_inflight = 0;
	fp = fmemopen((void *)out, len ? len : 1, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		if (!strchr(line, '*'))
			continue;
		if (has_headset_kw(line)) {
			headset = 1;
			break;
		}
	}
	fclose(fp);
	headset_probe_cached = headset;
	headset_probe_ms = monotonic_msec();
	audio_popup_data_arrived();
}

static void
headset_inspect_fetch_done(const char *out, size_t len, void *data)
{
	(void)len;
	(void)data;
	if (has_headset_kw(out)) {
		headset_fetch_inflight = 0;
		headset_probe_cached = 1;
		headset_probe_ms = monotonic_msec();
		audio_popup_data_arrived();
		return;
	}
	/* not conclusive: check the default sink line in wpctl status */
	if (fetch_async("wpctl status", headset_status_fetch_done, NULL) != 0)
		headset_fetch_inflight = 0;
}

int
pipewire_sink_is_headset_nb(void)
{
	uint64_t now = monotonic_msec();

	if (headset_probe_cached >= 0 && now - headset_probe_ms < 8000)
		return headset_probe_cached;
	if (!headset_fetch_inflight &&
			fetch_async("wpctl inspect @DEFAULT_AUDIO_SINK@",
				headset_inspect_fetch_done, NULL) == 0)
		headset_fetch_inflight = 1;
	return headset_probe_cached >= 0 ? headset_probe_cached : 0;
}

/* Parse one "Volume: 0.70 [MUTED]" reply; level < 0 on parse failure. */
static double
parse_wpctl_volume(const char *out, int *muted)
{
	const char *v = strstr(out, "Volume:");
	double raw;

	*muted = strstr(out, "[MUTED]") != NULL;
	if (v && sscanf(v, "Volume: %lf", &raw) == 1)
		return raw * 100.0;
	return -1.0;
}

static void
vol_fetch_done(const char *out, size_t len, void *data)
{
	int muted, is_headset;
	double level;
	uint64_t now = monotonic_msec();

	(void)len;
	(void)data;
	vol_fetch_inflight = 0;
	level = parse_wpctl_volume(out, &muted);
	if (level < 0.0)
		return;
	is_headset = headset_probe_cached == 1;
	/* a scroll/drag stored a newer value while we were in flight */
	if ((is_headset ? volume_last_read_headset_ms :
			volume_last_read_speaker_ms) >= vol_fetch_start_ms)
		return;
	volume_muted = muted;
	volume_cache_store(is_headset, level, muted, now);
	refreshstatusvolume();
	audio_popup_data_arrived();
}

double
pipewire_volume_percent_nb(int *is_headset_out)
{
	uint64_t now = monotonic_msec();
	int is_headset = pipewire_sink_is_headset_nb();
	uint64_t last_read = is_headset ? volume_last_read_headset_ms :
		volume_last_read_speaker_ms;
	double cached = is_headset ? volume_cached_headset :
		volume_cached_speaker;
	int cached_muted = is_headset ? volume_cached_headset_muted :
		volume_cached_speaker_muted;

	if (is_headset_out)
		*is_headset_out = is_headset;
	if (last_read != 0 && now - last_read < 8000 && cached >= 0.0) {
		volume_muted = cached_muted;
		return cached;
	}
	if (!vol_fetch_inflight &&
			fetch_async("wpctl get-volume @DEFAULT_AUDIO_SINK@",
				vol_fetch_done, NULL) == 0) {
		vol_fetch_inflight = 1;
		vol_fetch_start_ms = now;
	}
	return cached >= 0.0 ? cached : volume_last_for_type(is_headset);
}

static void
mic_fetch_done(const char *out, size_t len, void *data)
{
	int muted;
	double level;
	uint64_t now = monotonic_msec();

	(void)len;
	(void)data;
	mic_fetch_inflight = 0;
	level = parse_wpctl_volume(out, &muted);
	if (level < 0.0)
		return;
	if (mic_last_read_ms >= mic_fetch_start_ms)
		return;
	mic_muted = muted;
	mic_cached = level;
	mic_cached_muted = muted;
	mic_last_read_ms = now;
	microphone_active = level;
	refreshstatusmic();
	audio_popup_data_arrived();
}

double
pipewire_mic_volume_percent_nb(void)
{
	uint64_t now = monotonic_msec();

	if (mic_last_read_ms != 0 && now - mic_last_read_ms < 8000 &&
			mic_cached >= 0.0) {
		mic_muted = mic_cached_muted;
		return mic_cached;
	}
	if (!mic_fetch_inflight &&
			fetch_async("wpctl get-volume @DEFAULT_AUDIO_SOURCE@",
				mic_fetch_done, NULL) == 0) {
		mic_fetch_inflight = 1;
		mic_fetch_start_ms = now;
	}
	return mic_cached;
}

/* Startup defaults (apply_startup_defaults): unmute + set level + read
 * back, all in one background shell per device.  The old run_wpctl_sync
 * sequence blocked the compositor thread for every wpctl round-trip. */
void
audio_defaults_apply_async(double speaker_pct, double mic_pct)
{
	char cmd[192];
	uint64_t now = monotonic_msec();

	volume_invalidate_cache(0);
	volume_invalidate_cache(1);
	snprintf(cmd, sizeof(cmd),
		"wpctl set-mute @DEFAULT_AUDIO_SINK@ 0; "
		"wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f; "
		"wpctl get-volume @DEFAULT_AUDIO_SINK@", speaker_pct / 100.0);
	if (fetch_async(cmd, vol_fetch_done, NULL) == 0) {
		vol_fetch_inflight = 1;
		vol_fetch_start_ms = now;
	}
	mic_last_read_ms = 0;
	snprintf(cmd, sizeof(cmd),
		"wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 0; "
		"wpctl set-volume @DEFAULT_AUDIO_SOURCE@ %.2f; "
		"wpctl get-volume @DEFAULT_AUDIO_SOURCE@", mic_pct / 100.0);
	if (fetch_async(cmd, mic_fetch_done, NULL) == 0) {
		mic_fetch_inflight = 1;
		mic_fetch_start_ms = now;
	}
}

int
set_pipewire_mute(int mute)
{
	char arg[8];

	snprintf(arg, sizeof(arg), "%d", mute ? 1 : 0);

	if (fork() == 0) {
		setsid();
		execlp("wpctl", "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", arg, (char *)NULL);
		_exit(127);
	}

	/* Update cached state optimistically */
	volume_muted = mute;
	return 0;
}

int
set_pipewire_mic_mute(int mute)
{
	char arg[8];

	snprintf(arg, sizeof(arg), "%d", mute ? 1 : 0);

	if (fork() == 0) {
		setsid();
		execlp("wpctl", "wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", arg, (char *)NULL);
		_exit(127);
	}

	/* Update cached state optimistically */
	mic_muted = mute;
	mic_cached_muted = mute;
	return 0;
}

int
set_pipewire_volume(double percent)
{
	char arg[32];

	if (percent < 0.0)
		percent = 0.0;
	if (percent > volume_max_percent)
		percent = volume_max_percent;

	snprintf(arg, sizeof(arg), "%.2f%%", percent);

	if (fork() == 0) {
		setsid();
		execlp("wpctl", "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", arg, (char *)NULL);
		_exit(127);
	}

	return 0;
}

int
set_pipewire_mic_volume(double percent)
{
	char arg[32];

	if (percent < 0.0)
		percent = 0.0;
	if (percent > mic_max_percent)
		percent = mic_max_percent;

	snprintf(arg, sizeof(arg), "%.2f%%", percent);

	if (fork() == 0) {
		setsid();
		execlp("wpctl", "wpctl", "set-volume", "@DEFAULT_AUDIO_SOURCE@", arg, (char *)NULL);
		_exit(127);
	}

	mic_last_percent = percent;
	return 0;
}

int
toggle_pipewire_mute(void)
{
	int is_headset = pipewire_sink_is_headset_nb();
	uint64_t now = monotonic_msec();

	/* Native toggle: PipeWire flips mute atomically and preserves the
	 * volume level.  Toggle and read-back run in one background shell —
	 * a popen here blocked the compositor for the wpctl fork + PipeWire
	 * round-trip; vol_fetch_done() re-renders when the answer lands. */
	volume_invalidate_cache(is_headset);
	if (fetch_async("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle; "
			"wpctl get-volume @DEFAULT_AUDIO_SINK@",
			vol_fetch_done, NULL) == 0) {
		vol_fetch_inflight = 1;
		vol_fetch_start_ms = now;
	}
	return 0;
}

int
toggle_pipewire_mic_mute(void)
{
	uint64_t now = monotonic_msec();

	mic_last_read_ms = 0;
	if (fetch_async("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle; "
			"wpctl get-volume @DEFAULT_AUDIO_SOURCE@",
			mic_fetch_done, NULL) == 0) {
		mic_fetch_inflight = 1;
		mic_fetch_start_ms = now;
	}
	return 0;
}

void
positionstatusmodules(Monitor *m)
{
	int x, spacing, left_end;

	if (!m || !m->statusbar.tree)
		return;

	if (!m->showbar || !m->statusbar.area.width || !m->statusbar.area.height) {
		/* Bar toggled off but already laid out once: it is sliding out
		 * with the tile edge. Tear down popups and menus, but leave the
		 * bar's own nodes alone — blanking the modules mid-slide would
		 * show an empty bar gliding off. statusbar_anim_sync owns the
		 * tree's visibility and disables it once it is off-screen. */
		if (m->statusbar.slot.height > 0) {
			if (m->statusbar.tray_menu.tree) {
				wlr_scene_node_set_enabled(
					&m->statusbar.tray_menu.tree->node, 0);
				m->statusbar.tray_menu.visible = 0;
			}
			if (m->statusbar.cpu_popup.tree) {
				wlr_scene_node_set_enabled(
					&m->statusbar.cpu_popup.tree->node, 0);
				m->statusbar.cpu_popup.visible = 0;
			}
			if (m->statusbar.net_popup.tree) {
				wlr_scene_node_set_enabled(
					&m->statusbar.net_popup.tree->node, 0);
				m->statusbar.net_popup.visible = 0;
			}
			statusbar_anim_sync(m);
			return;
		}
		wlr_scene_node_set_enabled(&m->statusbar.tree->node, 0);
		if (m->statusbar.tags.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.tags.tree->node, 0);
			m->statusbar.tags.x = 0;
		}
		if (m->statusbar.traylabel.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.traylabel.tree->node, 0);
			m->statusbar.traylabel.x = 0;
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
		if (m->statusbar.bluetooth.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.bluetooth.tree->node, 0);
			m->statusbar.bluetooth.x = 0;
		}
		if (m->statusbar.display.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.display.tree->node, 0);
			m->statusbar.display.x = 0;
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
	if (m->statusbar.terminfo.tree)
		wlr_scene_node_set_enabled(&m->statusbar.terminfo.tree->node,
				m->statusbar.terminfo.width > 0);
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
	if (m->statusbar.fan.tree)
		wlr_scene_node_set_enabled(&m->statusbar.fan.tree->node,
				m->statusbar.fan.width > 0);
	if (m->statusbar.bluetooth.tree)
		wlr_scene_node_set_enabled(&m->statusbar.bluetooth.tree->node,
				m->statusbar.bluetooth.width > 0);
	if (m->statusbar.display.tree)
		wlr_scene_node_set_enabled(&m->statusbar.display.tree->node,
				m->statusbar.display.width > 0);
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
	if (m->statusbar.traylabel.width > 0) {
		wlr_scene_node_set_position(&m->statusbar.traylabel.tree->node, x, 0);
		m->statusbar.traylabel.x = x;
		x += m->statusbar.traylabel.width + spacing;
	}
	left_end = x;

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
	if (m->statusbar.fan.width > 0) {
		x -= m->statusbar.fan.width;
		wlr_scene_node_set_position(&m->statusbar.fan.tree->node, x, 0);
		m->statusbar.fan.x = x;
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
	if (m->statusbar.net.width > 0) {
		x -= m->statusbar.net.width;
		wlr_scene_node_set_position(&m->statusbar.net.tree->node, x, 0);
		m->statusbar.net.x = x;
		x -= spacing;
	}
	if (m->statusbar.bluetooth.width > 0) {
		x -= m->statusbar.bluetooth.width;
		wlr_scene_node_set_position(&m->statusbar.bluetooth.tree->node, x, 0);
		m->statusbar.bluetooth.x = x;
		x -= spacing;
	}
	if (m->statusbar.display.width > 0) {
		x -= m->statusbar.display.width;
		wlr_scene_node_set_position(&m->statusbar.display.tree->node, x, 0);
		m->statusbar.display.x = x;
		x -= spacing;
	}

	/* Terminal-info label: centered in the free span between the left
	 * group (tags/steam/net/tray) and the right group.  x is now the
	 * left edge of the leftmost right-side module minus spacing. */
	if (m->statusbar.terminfo.tree) {
		int w = m->statusbar.terminfo.width;
		int right_start = x + spacing;

		if (right_start > m->statusbar.area.width)
			right_start = m->statusbar.area.width;
		if (w > 0 && right_start - left_end >= w) {
			int tx = left_end + (right_start - left_end - w) / 2;
			wlr_scene_node_set_position(
					&m->statusbar.terminfo.tree->node, tx, 0);
			m->statusbar.terminfo.x = tx;
			wlr_scene_node_set_enabled(
					&m->statusbar.terminfo.tree->node, 1);
		} else {
			wlr_scene_node_set_enabled(
					&m->statusbar.terminfo.tree->node, 0);
		}
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
					popup_x, statusbar_popup_y(m));
			m->statusbar.cpu_popup.refresh_data = 1;
		} else {
			wlr_scene_node_set_enabled(&m->statusbar.cpu_popup.tree->node, 0);
			m->statusbar.cpu_popup.visible = 0;
			m->statusbar.cpu_popup.refresh_data = 0;
		}
	}
	if (m->statusbar.net_popup.tree) {
		int pos_x = m->statusbar.net_popup.anchor_x;

		if (m->statusbar.area.height > 0) {
			wlr_scene_node_set_position(&m->statusbar.net_popup.tree->node,
					pos_x, statusbar_popup_y(m));
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
			m->statusbar.tray_menu.y = statusbar_popup_y(m);
			wlr_scene_node_set_position(&m->statusbar.tray_menu.tree->node,
					m->statusbar.tray_menu.x, m->statusbar.tray_menu.y);
			wlr_scene_node_set_enabled(&m->statusbar.tray_menu.tree->node, 1);
		} else {
			wlr_scene_node_set_enabled(&m->statusbar.tray_menu.tree->node, 0);
			m->statusbar.tray_menu.visible = 0;
		}
	}
}

int
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

void
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

int
popup_delay_timeout(void *data)
{
	Monitor *m;
	(void)data;

	/* Full hover re-poll for the bar under the cursor: motionnotify's
	 * 8ms throttle can drop the burst's last event, leaving the module
	 * under the resting cursor without hover state (popup never shows).
	 * The throttled path schedules this timeout to catch up. */
	m = xytomon(cursor->x, cursor->y);
	if (m && m->showbar) {
		Client *fs = focustop(m);

		if (!(fs && fs->isfullscreen)) {
			updatecpuhover(m, cursor->x, cursor->y);
			updateramhover(m, cursor->x, cursor->y);
			updatebatteryhover(m, cursor->x, cursor->y);
			updatenethover(m, cursor->x, cursor->y);
			updateinfopopups(m, cursor->x, cursor->y);
			tray_menu_update_hover(m, cursor->x, cursor->y);
		}
	}

	wl_list_for_each(m, &mons, link) {
		if (!m->showbar)
			continue;
		/* Re-run hover checks to show popup after delay */
		if (m->statusbar.cpu_popup.hover_start_ms != 0 && !m->statusbar.cpu_popup.visible)
			updatecpuhover(m, cursor->x, cursor->y);
		if (m->statusbar.ram_popup.hover_start_ms != 0 && !m->statusbar.ram_popup.visible)
			updateramhover(m, cursor->x, cursor->y);
		if (m->statusbar.battery_popup.hover_start_ms != 0 && !m->statusbar.battery_popup.visible)
			updatebatteryhover(m, cursor->x, cursor->y);
		if (m->statusbar.net_popup.hover_start_ms != 0 && !m->statusbar.net_popup.visible)
			updatenethover(m, cursor->x, cursor->y);
		if (info_popup_pending(m))
			updateinfopopups(m, cursor->x, cursor->y);
		/* battery profile-switch refetch lands here too */
		if (m->statusbar.battery_popup.visible &&
				m->statusbar.battery_popup.last_fetch_ms == 0) {
			m->statusbar.battery_popup.refresh_data = 1;
			renderbatterypopup(m);
		}
	}
	return 0;
}

void
schedule_popup_delay(uint32_t ms)
{
	if (!event_loop)
		return;
	if (!popup_delay_timer)
		popup_delay_timer = wl_event_loop_add_timer(event_loop,
				popup_delay_timeout, NULL);
	if (popup_delay_timer)
		wl_event_source_timer_update(popup_delay_timer, ms);
}

/* Slide the bar in lock-step with the tile area.
 *
 * togglestatusbar only flips m->showbar; the tile area then springs to its
 * new top edge in monitor_anim_tick. The bar's own offset is derived from
 * that same spring value rather than from a second animation, so the gap
 * between the bar's bottom edge and the tiles is pixel-identical in every
 * frame of the transition — two independent timelines would drift.
 *
 * Fully off-screen (offset == slide distance) parks the node far above every
 * output instead of disabling it: the scene graph releases the buffer AND the
 * texture of every buffer node under a disabled tree
 * (scene_node_cleanup_when_disabled), so a disabled bar loses every glyph and
 * icon it had rasterized and comes back as bare module backgrounds. Parked
 * off-screen it is just as invisible and just as uncomposited, but its content
 * survives, so re-showing is a pure node move. Clicks cannot reach it either —
 * every bar input path tests m->showbar first. */
#define STATUSBAR_PARK_Y 100000
void
statusbar_anim_sync(Monitor *m)
{
	int gap, bar_h, slide, offset, shown_x, shown_y, tile_top_with_bar;
	double tile_top_now;

	if (!m || !m->statusbar.tree || !m->wlr_output || !m->wlr_output->enabled)
		return;

	gap = m->gaps ? (int)gappx : 0;

	/* The slot layoutstatusbar last computed — it accounts for exclusive
	 * layer surfaces, which m->m does not. Before the first layout (or on
	 * a monitor that has never shown the bar) fall back to the monitor
	 * box. */
	if (m->statusbar.slot.height > 0) {
		shown_x = m->statusbar.slot.x;
		shown_y = m->statusbar.slot.y;
		bar_h = m->statusbar.slot.height;
	} else {
		shown_x = m->m.x + gap;
		shown_y = m->m.y + statusbar_top_gap;
		bar_h = MIN((int)statusbar_height, m->m.height);
	}
	if (bar_h <= 0)
		return;

	/* Where arrangelayers puts the tile top when the bar is visible. */
	tile_top_with_bar = shown_y + bar_h + gap;
	slide = bar_h + statusbar_top_gap;

	tile_top_now = m->w_initialized ? m->w_y_f : (double)tile_top_with_bar;
	offset = (int)lround((double)tile_top_with_bar - tile_top_now);
	if (offset < 0)
		offset = 0;
	if (offset > slide)
		offset = slide;

	wlr_scene_node_set_enabled(&m->statusbar.tree->node, 1);
	wlr_scene_node_set_position(&m->statusbar.tree->node, shown_x,
			offset < slide ? shown_y - offset
					: shown_y - STATUSBAR_PARK_Y);
}

void
layoutstatusbar(Monitor *m, const struct wlr_box *area, struct wlr_box *client_area)
{
	int gap, bar_height;
	struct wlr_box bar_area = {0};

	if (!m || !m->statusbar.tree || !area || !client_area)
		return;

	if (!m->showbar) {
		hidetagthumbnail(m);
		*client_area = *area;
		/* Arm a full module re-render for the re-show layout: buffer
		 * nodes can lose their contents while the bar is away (scene
		 * cleanup on any disabled subtree), and the change-gated
		 * refresh paths never repaint unchanged text — so repaint
		 * everything the moment the bar comes back instead of trusting
		 * the parked content to have survived. */
		m->statusbar.last_layout_h = 0;
		m->statusbar.tags.render_sig = 0;
		/* Node stays in the scene and slides out with the tile edge —
		 * statusbar_anim_sync parks it once it is fully off-screen. */
		statusbar_anim_sync(m);
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

	m->statusbar.area = bar_area;
	m->statusbar.slot = bar_area;
	/* Position comes from the slide state, not straight from bar_area:
	 * on re-show the bar starts off-screen and rides the tile-area
	 * spring back down. */
	statusbar_anim_sync(m);

	/* Tags reflect occupancy/focus which changes with every arrange —
	 * always re-render.  The other modules only depend on bar height
	 * and their own text; their refresh paths (2 s cpu tick, 45 s icon
	 * tick, volume/net events) re-render on content change, so a full
	 * re-rasterization here on EVERY arrangelayers (each layer-surface
	 * commit!) is wasted work unless the height
	 * changed or the bar was just re-shown. */
	renderworkspaces(m, &m->statusbar.tags, bar_area.height);
	diag_logf("BAR", "layout %s barh=%d tags_w=%d boxes=%d font=%d reshow=%d",
		m->wlr_output->name, bar_area.height, m->statusbar.tags.width,
		m->statusbar.tags.box_count, statusfont.font ? 1 : 0,
		m->statusbar.last_layout_h != bar_area.height);
	if (m->statusbar.last_layout_h != bar_area.height) {
		m->statusbar.last_layout_h = bar_area.height;
		if (m->statusbar.traylabel.tree)
			rendertray(m, bar_area.height);
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
		if (m->statusbar.fan.tree)
			renderfan(&m->statusbar.fan, bar_area.height, fan_text);
		if (m->statusbar.bluetooth.tree)
			renderbluetooth(&m->statusbar.bluetooth, bar_area.height, "");
		if (m->statusbar.display.tree)
			renderdisplays(&m->statusbar.display, bar_area.height, "");
		if (m->statusbar.mic.tree)
			rendermic(&m->statusbar.mic, bar_area.height, mic_text);
		if (m->statusbar.ram.tree)
			renderram(&m->statusbar.ram, bar_area.height, ram_text);
		if (m->statusbar.clock.tree) {
			time_t now = time(NULL);
			struct tm tm;
			char timestr[6] = {0};
			if (now != (time_t)-1 && localtime_r(&now, &tm)
					&& strftime(timestr, sizeof(timestr), "%H:%M", &tm))
				renderclock(&m->statusbar.clock, bar_area.height, timestr);
		}
		if (m->statusbar.terminfo.tree) {
			m->statusbar.terminfo.last_render_text[0] = '\0';
			refreshstatusterminfo();
		}
	}
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

void
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
		if (!m->statusbar.clock.tree)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		renderclock(&m->statusbar.clock, barh, timestr);
		positionstatusmodules(m);
	}
}

void
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
		if (!m->statusbar.light.tree)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.light, barh, light_text)) {
			renderlight(&m->statusbar.light, barh, light_text);
			positionstatusmodules(m);
		}
	}
}

void
refreshstatusbattery(void)
{
	Monitor *m;
	int barh;
	double percent, display;
	const char *icon = battery_icon_100;

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
		if (!m->statusbar.battery.tree)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.battery, barh, battery_text)) {
			renderbattery(&m->statusbar.battery, barh, battery_text);
			positionstatusmodules(m);
		}
	}
}

/* battwatch's event-loop callback: a new battery snapshot landed. */
void
statusbar_battery_event(void)
{
	BattSnapshot s;
	Monitor *m;

	if (!battwatch_get(&s))
		return;
	battery_available = s.available;
	if (s.available && !battery_device_dir[0]) {
		snprintf(battery_device_dir, sizeof(battery_device_dir),
				"%s", s.device_dir);
		charge_limit_apply_saved();
	}
	refreshstatusbattery();
	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.battery_popup.visible) {
			m->statusbar.battery_popup.refresh_data = 1;
			renderbatterypopup(m);
		}
	}
}

void
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
		if (!m->statusbar.cpu.tree)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.cpu, barh, cpu_text)
				|| m->statusbar.cpu_popup.visible) {
			rendercpu(&m->statusbar.cpu, barh, cpu_text);
			if (m->statusbar.cpu_popup.visible)
				rendercpupopup(m);
			positionstatusmodules(m);
		}
	}
}

void
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
		if (!m->statusbar.ram.tree)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.ram, barh, ram_text)) {
			renderram(&m->statusbar.ram, barh, ram_text);
			positionstatusmodules(m);
		}
	}
}

void
refreshstatusvolume(void)
{
	int is_headset = pipewire_sink_is_headset_nb();
	double vol = speaker_active;
	Monitor *m;
	int barh;
	double display = vol;
	int force_render = 0;
	int use_muted_color = 0;
	const char *icon = volume_icon_speaker_100;

	/* Startup defaults retry here until PipeWire answers — no-op once
	 * they have been applied. */
	apply_startup_defaults();

	/* Always re-read (throttled to one wpctl call per 8s inside
	 * pipewire_volume_percent). Reading only when the cached level was
	 * negative meant mute/volume changed anywhere else — pavucontrol, a
	 * headset button, or a startup default that never landed — stayed
	 * invisible for the rest of the session. */
	{
		double read = pipewire_volume_percent_nb(&is_headset);
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
		if (!m->statusbar.volume.tree)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.volume, barh, volume_text)
				|| force_render) {
			rendervolume(&m->statusbar.volume, barh, volume_text);
			positionstatusmodules(m);
		}
	}
}

void
refreshstatusmic(void)
{
	double vol = microphone_active;
	Monitor *m;
	int barh;
	double display = vol;
	int force_render = 0;
	int use_muted_color = 0;
	const char *icon = mic_icon_unmuted;

	/* The bar is hidden in game mode, and the fallthrough below can
	 * popen wpctl on the main loop (PTT edges land here) — skip; the
	 * game-mode exit refresh re-renders the module. */
	if (game_mode_active)
		return;

	/* Re-read every refresh (8s throttle lives in the reader) so an
	 * externally muted mic — or a startup default that never reached
	 * PipeWire — cannot leave the icon lying. */
	{
		double read = pipewire_mic_volume_percent_nb();
		int available = read >= 0.0;

		if (available) {
			vol = read;
			microphone_active = read;
			mic_last_percent = read;
		}
		if (available != mic_available) {
			mic_available = available;
			force_render = 1;
		}
	}
	if (!mic_available) {
		if (force_render) {
			/* Mic just went away: drop the module and reflow. */
			wl_list_for_each(m, &mons, link) {
				if (!m->statusbar.mic.tree)
					continue;
				rendermic(&m->statusbar.mic, 0, mic_text);
				positionstatusmodules(m);
			}
		}
		return;
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
		if (!m->statusbar.mic.tree)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (status_should_render(&m->statusbar.mic, barh, mic_text)
				|| force_render) {
			rendermic(&m->statusbar.mic, barh, mic_text);
			positionstatusmodules(m);
		}
	}
}

void
refreshstatusicons(void)
{
	Monitor *m;
	int barh;

	wl_list_for_each(m, &mons, link) {
		/* rendertray can hit sync D-Bus pixmap fetches and icon-theme
		 * directory scans; never for a hidden bar (game mode). The
		 * game-mode exit path re-renders via tray_refresh_stale(). */
		if (!m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (m->statusbar.traylabel.tree)
			rendertray(m, barh);
		positionstatusmodules(m);
	}
}

/* Re-render the workspace boxes for one monitor (workspace switched or
 * occupancy changed). */
void
refreshworkspacemodule(Monitor *m)
{
	int barh;

	if (!m || !m->statusbar.tags.tree)
		return;
	barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
	renderworkspaces(m, &m->statusbar.tags, barh);
	positionstatusmodules(m);
}

void
refreshstatustags(void)
{
	Monitor *m;
	int barh;

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.tags.tree)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		renderworkspaces(m, &m->statusbar.tags, barh);
		/* Re-render rather than blank: blanking left the tray gone until
		 * the 45 s icon tick, and with the bar hidden that gap outlives
		 * the re-show — the bar would come back without its tray. */
		if (m->statusbar.traylabel.tree)
			rendertray(m, barh);
		m->statusbar.tags.hover_tag = -1;
		for (int i = 0; i < TAGCOUNT; i++)
			m->statusbar.tags.hover_alpha[i] = 0.0f;
		positionstatusmodules(m);
	}
}

void
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

int
status_should_render(StatusModule *module, int barh, const char *text)
{
	if (!module || !text)
		return 1;

	if (module->last_render_h != barh || module->last_render_text[0] == '\0'
			|| strncmp(module->last_render_text, text,
				sizeof(module->last_render_text)) != 0) {
		snprintf(module->last_render_text,
				sizeof(module->last_render_text), "%s", text);
		module->last_render_h = barh;
		return 1;
	}
	return 0;
}

void
initial_status_refresh(void)
{
	apply_startup_defaults();
	refreshstatusclock();
	refreshstatuscpu();
	refreshstatusram();
	refreshstatuslight();
	/* volume/mic intentionally NOT refreshed here: they popen wpctl,
	 * which blocks for seconds while PipeWire cold-starts alongside the
	 * compositor — the startup freeze.  init_status_refresh_tasks
	 * schedules their first run after the audio grace period. */
	refreshstatusbattery();
	refreshstatusnet();
	request_public_ip_async(); /* prefetch public IP in background */
	refreshstatusicons();
	refreshstatustags();
}

/* Render-only: fanwatch.c's thread does the sysfs sampling and calls
 * this from its event-loop callback once a new snapshot is published. */
void
refreshstatusfan(void)
{
	Monitor *m;
	int barh;

	if (fan_pub.total_fans <= 0)
		return;
	if (fan_primary_value(fan_text, sizeof(fan_text)) != 0)
		return;

	wl_list_for_each(m, &mons, link) {
		if (!m->statusbar.fan.tree)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height :
			(int)statusbar_height;
		if (status_should_render(&m->statusbar.fan, barh, fan_text)) {
			renderfan(&m->statusbar.fan, barh, fan_text);
			positionstatusmodules(m);
		}
	}
}

void
init_status_refresh_tasks(void)
{
	uint64_t now = monotonic_msec();
	uint32_t offset = 100;

	for (size_t i = 0; i < LENGTH(status_tasks); i++) {
		/* wpctl-based tasks wait out the PipeWire cold-start grace
		 * period (see apply_startup_defaults) — running them earlier
		 * blocks the main thread in wpctl's connect. */
		if (status_tasks[i].fn == refreshstatusvolume ||
				status_tasks[i].fn == refreshstatusmic) {
			status_tasks[i].next_due_ms = now + 3200;
			continue;
		}
		status_tasks[i].next_due_ms = now + offset;
		offset += 200; /* stagger initial fills to avoid clumping */
	}
}

void
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

void
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

int
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
	if (fn == refreshstatusbluetooth) {
		wl_list_for_each(m, &mons, link) {
			if (m->showbar && m->statusbar.bt_popup.visible)
				return 1;
		}
	}
	return 0;
}

void
schedule_next_status_refresh(void)
{
	uint64_t now = monotonic_msec();
	uint64_t next = UINT64_MAX;

	if (!status_cpu_timer || game_mode_active || htpc_mode_active)
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

void
schedule_status_timer(void)
{
	struct timespec ts;
	double now, next;
	int ms;

	if (!status_timer || game_mode_active || htpc_mode_active)
		return;

	clock_gettime(CLOCK_REALTIME, &ts);
	now = ts.tv_sec + ts.tv_nsec / 1e9;
	next = ceil(now / 60.0) * 60.0;
	ms = (int)((next - now) * 1000.0);
	if (ms < 1)
		ms = 1;

	wl_event_source_timer_update(status_timer, ms);
}

int
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
	} else if (status_tasks[chosen].fn == refreshstatusterminfo
			&& terminfo_wants_fast_poll()) {
		/* Focused terminal: repoll /proc quickly so cwd/ssh changes
		 * show up without a focus round-trip.  Render only happens
		 * when the text actually changes (status_should_render). */
		status_tasks[chosen].next_due_ms = now + 500;
	} else if (status_task_hover_active(status_tasks[chosen].fn)) {
		status_tasks[chosen].next_due_ms = now + STATUS_FAST_MS;
	} else {
		status_tasks[chosen].next_due_ms = now + random_status_delay_ms();
	}
	schedule_next_status_refresh();
	return 0;
}

int
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

void
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
			ly < statusbar_popup_y(m) + p->height) {
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
		/* Track when hover started for delay */
		if (p->hover_start_ms == 0)
			p->hover_start_ms = now;

		/* Wait 300ms before showing popup */
		if (!was_visible && (now - p->hover_start_ms) < 300) {
			/* Schedule timer to check again after remaining delay */
			uint64_t remaining = 300 - (now - p->hover_start_ms);
			schedule_popup_delay(remaining + 1);
			return;
		}

		if (!was_visible) {
			/* Delay heavy popup refresh until pointer lingers for 1s */
			p->suppress_refresh_until_ms = now + 1000;
		}
		p->visible = 1;
		wlr_scene_node_set_enabled(&p->tree->node, 1);
		wlr_scene_node_set_position(&p->tree->node,
				popup_x, statusbar_popup_y(m));
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
				wlr_scene_node_set_position(&p->tree->node,
						cpu_popup_clamped_x(m, p),
						statusbar_popup_y(m));
			}
		}
		if (!was_visible) {
			schedule_cpu_popup_refresh(1000);
			popup_view_show(&p->view);
		}
	} else if (p->visible || p->hover_start_ms != 0) {
		p->visible = 0;
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->hover_idx = -1;
		p->refresh_data = 0;
		p->last_render_ms = 0;
		p->suppress_refresh_until_ms = 0;
		p->hover_start_ms = 0;
		popup_view_hide(&p->view);
	}
}

void
updateramhover(Monitor *m, double cx, double cy)
{
	int lx, ly;
	int inside = 0;
	int popup_hover = 0;
	int was_visible;
	RamPopup *p;
	int popup_x;
	int new_hover = -1;
	uint64_t now = monotonic_msec();
	int need_refresh = 0;
	int stale_refresh = 0;

	if (!m || !m->showbar || !m->statusbar.ram.tree || !m->statusbar.ram_popup.tree) {
		if (m && m->statusbar.ram_popup.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.ram_popup.tree->node, 0);
			m->statusbar.ram_popup.visible = 0;
			m->statusbar.ram_popup.hover_idx = -1;
		}
		return;
	}

	p = &m->statusbar.ram_popup;
	lx = (int)floor(cx) - m->statusbar.area.x;
	ly = (int)floor(cy) - m->statusbar.area.y;

	popup_x = m->statusbar.ram.x;
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
			ly < statusbar_popup_y(m) + p->height) {
		popup_hover = 1;
	}

	if (lx >= m->statusbar.ram.x &&
			lx < m->statusbar.ram.x + m->statusbar.ram.width &&
			ly >= 0 && ly < m->statusbar.area.height &&
			m->statusbar.ram.width > 0) {
		inside = 1;
	} else if (popup_hover) {
		inside = 1;
	}

	was_visible = p->visible;

	if (inside) {
		/* Track when hover started for delay */
		if (p->hover_start_ms == 0)
			p->hover_start_ms = now;

		/* Wait 300ms before showing popup */
		if (!was_visible && (now - p->hover_start_ms) < 300) {
			/* Schedule timer to check again after remaining delay */
			uint64_t remaining = 300 - (now - p->hover_start_ms);
			schedule_popup_delay(remaining + 1);
			return;
		}

		p->visible = 1;
		wlr_scene_node_set_enabled(&p->tree->node, 1);
		wlr_scene_node_set_position(&p->tree->node,
				popup_x, statusbar_popup_y(m));
		new_hover = ram_popup_hover_index(m, p);
		stale_refresh = (p->last_fetch_ms == 0 ||
				now < p->last_fetch_ms ||
				(now - p->last_fetch_ms) >= ram_popup_refresh_interval_ms);
		need_refresh = (!was_visible || stale_refresh) &&
				(p->suppress_refresh_until_ms == 0 ||
				 now >= p->suppress_refresh_until_ms);
		if (need_refresh)
			p->refresh_data = 1;
		if (!was_visible || need_refresh) {
			p->hover_idx = new_hover;
			renderrampopup(m);
			wlr_scene_node_set_position(&p->tree->node,
					ram_popup_clamped_x(m, p),
					statusbar_popup_y(m));
		} else if (new_hover != p->hover_idx) {
			/* Kill-button hover highlight; throttled like the CPU
			 * popup so fast pointer sweeps don't re-raster every
			 * motion event. */
			if (p->last_render_ms == 0 || now < p->last_render_ms ||
					now - p->last_render_ms >= 16) {
				p->hover_idx = new_hover;
				renderrampopup(m);
			}
		}
		if (!was_visible) {
			schedule_ram_popup_refresh(100);
			popup_view_show(&p->view);
		}
	} else if (p->visible || p->hover_start_ms != 0) {
		p->visible = 0;
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->hover_idx = -1;
		p->refresh_data = 0;
		p->last_render_ms = 0;
		p->suppress_refresh_until_ms = 0;
		p->hover_start_ms = 0;
		popup_view_hide(&p->view);
	}
}

void
updatenethover(Monitor *m, double cx, double cy)
{
	int inside = 0;
	int was_visible;
	NetPopup *p;
	uint64_t now = monotonic_msec();

	if (!m || !m->showbar || !m->statusbar.net_popup.tree) {
		if (m && m->statusbar.net_popup.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.net_popup.tree->node, 0);
			m->statusbar.net_popup.visible = 0;
		}
		return;
	}

	p = &m->statusbar.net_popup;

	/* Anchor on the net module itself */
	p->anchor_w = m->statusbar.net.width;
	if (p->anchor_w > 0) {
		p->anchor_x = m->statusbar.net.x;
		if (p->width > 0 && m->statusbar.area.width > 0) {
			int max_x = m->statusbar.area.width - p->width;

			if (max_x < 0)
				max_x = 0;
			if (p->anchor_x > max_x)
				p->anchor_x = max_x;
			if (p->anchor_x < 0)
				p->anchor_x = 0;
		}
	}

	if (!inside && p->anchor_w > 0) {
		int lx_abs = (int)floor(cx);
		int ly_abs = (int)floor(cy);
		int ax0 = m->statusbar.area.x + m->statusbar.net.x;
		int ay0 = m->statusbar.area.y;
		if (lx_abs >= ax0 && lx_abs < ax0 + p->anchor_w &&
				ly_abs >= ay0 && ly_abs < ay0 + m->statusbar.area.height)
			inside = 1;
	}

	/* Keep visible while hovering popup itself */
	if (!inside && p->visible && p->width > 0 && p->height > 0) {
		int px0 = m->statusbar.area.x + p->anchor_x;
		int py0 = m->statusbar.area.y + m->statusbar.area.height;
		int px1 = px0 + p->width;
		int py1 = m->statusbar.area.y + statusbar_popup_y(m) + p->height;
		int cx_i = (int)floor(cx);
		int cy_i = (int)floor(cy);
		if (cx_i >= px0 && cx_i < px1 && cy_i >= py0 && cy_i < py1)
			inside = 1;
	}

	/* Hold the popup open while an SSID/password entry is active */
	if (p->visible && text_entry_active())
		inside = 1;

	was_visible = p->visible;

	if (inside) {
		/* Track when hover started for delay */
		if (p->hover_start_ms == 0)
			p->hover_start_ms = now;

		/* Wait 300ms before showing popup */
		if (!was_visible && (now - p->hover_start_ms) < 300) {
			/* Schedule timer to check again after remaining delay */
			uint64_t remaining = 300 - (now - p->hover_start_ms);
			schedule_popup_delay(remaining + 1);
			return;
		}

		if (!was_visible) {
			p->suppress_refresh_until_ms = now + 2000;
			set_status_task_due(refreshstatusnet, p->suppress_refresh_until_ms);
		}
		p->visible = 1;
		wlr_scene_node_set_enabled(&p->tree->node, 1);
		wlr_scene_node_set_position(&p->tree->node,
				p->anchor_x, statusbar_popup_y(m));
		if (!was_visible) {
			p->anchor_y = statusbar_popup_y(m);
			rendernetpopup(m);
			/* re-clamp with the freshly rendered width */
			p->anchor_x = m->statusbar.net.x;
			if (p->width > 0 && m->statusbar.area.width > 0) {
				int max_x = m->statusbar.area.width - p->width;

				if (max_x < 0)
					max_x = 0;
				if (p->anchor_x > max_x)
					p->anchor_x = max_x;
				if (p->anchor_x < 0)
					p->anchor_x = 0;
			}
			wlr_scene_node_set_position(&p->tree->node,
					p->anchor_x, statusbar_popup_y(m));
			popup_view_show(&p->view);
		}
		net_popup_track_hover(m, cx, cy);
	} else if (p->visible || p->hover_start_ms != 0) {
		p->visible = 0;
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->suppress_refresh_until_ms = 0;
		p->hover_start_ms = 0;
		p->btn_hover = -1;
		popup_view_hide(&p->view);
		set_status_task_due(refreshstatusnet, now + 60000);
	}
}

static int
popup_contains(Monitor *m, struct wlr_scene_tree *tree, int visible,
		int w, int h, double cx, double cy)
{
	int x0, y0;

	if (!tree || !visible || w <= 0 || h <= 0)
		return 0;
	x0 = m->statusbar.area.x + tree->node.x;
	y0 = m->statusbar.area.y + tree->node.y;
	return cx >= x0 && cx < x0 + w && cy >= y0 && cy < y0 + h;
}

/* 1 if (cx,cy) is inside a visible bar popup/dropdown.  Used to stop
 * pointer focus and motion from leaking to the client underneath. */
int
statusbar_popup_at(Monitor *m, double cx, double cy)
{
	struct StatusBar *sb;

	if (!m || !m->showbar)
		return 0;
	sb = &m->statusbar;
	return popup_contains(m, sb->tray_menu.tree, sb->tray_menu.visible,
				sb->tray_menu.width, sb->tray_menu.height, cx, cy)
		|| popup_contains(m, sb->cpu_popup.tree, sb->cpu_popup.visible,
				sb->cpu_popup.width, sb->cpu_popup.height, cx, cy)
		|| popup_contains(m, sb->ram_popup.tree, sb->ram_popup.visible,
				sb->ram_popup.width, sb->ram_popup.height, cx, cy)
		|| popup_contains(m, sb->battery_popup.tree, sb->battery_popup.visible,
				sb->battery_popup.width, sb->battery_popup.height, cx, cy)
		|| popup_contains(m, sb->net_popup.tree, sb->net_popup.visible,
				sb->net_popup.width, sb->net_popup.height, cx, cy)
		|| popup_contains(m, sb->fan_popup.tree, sb->fan_popup.visible,
				sb->fan_popup.width, sb->fan_popup.height, cx, cy)
		|| popup_contains(m, sb->clock_popup.tree, sb->clock_popup.visible,
				sb->clock_popup.width, sb->clock_popup.height, cx, cy)
		|| popup_contains(m, sb->volume_popup.tree, sb->volume_popup.visible,
				sb->volume_popup.width, sb->volume_popup.height, cx, cy)
		|| popup_contains(m, sb->mic_popup.tree, sb->mic_popup.visible,
				sb->mic_popup.width, sb->mic_popup.height, cx, cy)
		|| popup_contains(m, sb->light_popup.tree, sb->light_popup.visible,
				sb->light_popup.width, sb->light_popup.height, cx, cy)
		|| popup_contains(m, sb->bt_popup.tree, sb->bt_popup.visible,
				sb->bt_popup.width, sb->bt_popup.height, cx, cy)
		|| popup_contains(m, sb->display_popup.tree,
				sb->display_popup.visible,
				sb->display_popup.width,
				sb->display_popup.height, cx, cy);
}

int
updatestatusclock(void *data)
{
	(void)data;
	refreshstatusclock();
	schedule_status_timer();
	return 0;
}

void
initstatusbar(Monitor *m)
{
	if (!m)
		return;

	wl_list_init(&m->statusbar.tray_menu.entries);
	if (!vpn_list_initialized) {
		wl_list_init(&vpn_connections);
		vpn_list_initialized = 1;
	}
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
		m->statusbar.terminfo.tree = wlr_scene_tree_create(m->statusbar.tree);
		m->statusbar.tray_menu.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.tray_menu.tree) {
			m->statusbar.tray_menu.bg = wlr_scene_tree_create(m->statusbar.tray_menu.tree);
			m->statusbar.tray_menu.visible = 0;
			wlr_scene_node_set_enabled(&m->statusbar.tray_menu.tree->node, 0);
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
	m->statusbar.fan.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.fan.tree)
		m->statusbar.fan.bg = wlr_scene_tree_create(m->statusbar.fan.tree);
	m->statusbar.bluetooth.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.bluetooth.tree)
		m->statusbar.bluetooth.bg = wlr_scene_tree_create(m->statusbar.bluetooth.tree);
	m->statusbar.display.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.display.tree)
		m->statusbar.display.bg = wlr_scene_tree_create(m->statusbar.display.tree);
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
		m->statusbar.cpu_popup.refresh_data = 0;
		m->statusbar.cpu_popup.last_fetch_ms = 0;
		m->statusbar.cpu_popup.suppress_refresh_until_ms = 0;
		wlr_scene_node_set_enabled(&m->statusbar.cpu_popup.tree->node, 0);
	}
	m->statusbar.ram_popup.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.ram_popup.tree) {
		m->statusbar.ram_popup.bg = wlr_scene_tree_create(m->statusbar.ram_popup.tree);
		m->statusbar.ram_popup.visible = 0;
		m->statusbar.ram_popup.hover_idx = -1;
		m->statusbar.ram_popup.refresh_data = 0;
		m->statusbar.ram_popup.last_fetch_ms = 0;
		m->statusbar.ram_popup.suppress_refresh_until_ms = 0;
		wlr_scene_node_set_enabled(&m->statusbar.ram_popup.tree->node, 0);
	}
	m->statusbar.battery_popup.tree = wlr_scene_tree_create(m->statusbar.tree);
	if (m->statusbar.battery_popup.tree) {
		m->statusbar.battery_popup.bg = wlr_scene_tree_create(m->statusbar.battery_popup.tree);
		m->statusbar.battery_popup.visible = 0;
		m->statusbar.battery_popup.refresh_data = 0;
		m->statusbar.battery_popup.last_fetch_ms = 0;
		m->statusbar.battery_popup.last_render_ms = 0;
		m->statusbar.battery_popup.suppress_refresh_until_ms = 0;
		wlr_scene_node_set_enabled(&m->statusbar.battery_popup.tree->node, 0);
	}
		m->statusbar.net_popup.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.net_popup.tree) {
			m->statusbar.net_popup.bg = wlr_scene_tree_create(m->statusbar.net_popup.tree);
			m->statusbar.net_popup.visible = 0;
			m->statusbar.net_popup.btn_hover = -1;
			m->statusbar.net_popup.nhits = 0;
			wlr_scene_node_set_enabled(&m->statusbar.net_popup.tree->node, 0);
		}
		{
			InfoPopup *info[7] = { &m->statusbar.clock_popup,
				&m->statusbar.volume_popup, &m->statusbar.mic_popup,
				&m->statusbar.light_popup, &m->statusbar.fan_popup,
				&m->statusbar.bt_popup, &m->statusbar.display_popup };
			for (int i = 0; i < 7; i++) {
				info[i]->tree = wlr_scene_tree_create(m->statusbar.tree);
				info[i]->visible = 0;
				info[i]->hover_start_ms = 0;
				info[i]->last_render_ms = 0;
				info[i]->btn_hover = -1;
				info[i]->nhits = 0;
				if (info[i]->tree)
					wlr_scene_node_set_enabled(&info[i]->tree->node, 0);
			}
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
				m->modal.search_rendered[i][0] = '\0';
				m->modal.result_count[i] = 0;
				for (int j = 0; j < (int)LENGTH(m->modal.results[i]); j++)
					m->modal.results[i][j][0] = '\0';
				m->modal.selected[i] = -1;
				m->modal.scroll[i] = 0;
			}
			m->modal.search_field_tree = NULL;
			m->modal.render_timer = NULL;
			m->modal.render_pending = 0;
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
	m->modal.git_search_pid = -1;
	m->modal.git_search_fd = -1;
	m->modal.git_search_event = NULL;
	m->modal.git_search_len = 0;
	m->modal.git_search_buf[0] = '\0';
	m->modal.git_search_done = 0;
	m->modal.git_result_count = 0;
	m->modal.results_tree = NULL;
	m->modal.last_scroll = 0;
	m->modal.last_selected = -1;
	m->modal.row_highlight_count = 0;
	for (int i = 0; i < MODAL_MAX_RESULTS; i++)
		m->modal.row_highlights[i] = NULL;
	for (int i = 0; i < (int)LENGTH(m->modal.git_results_path); i++) {
		m->modal.git_results_name[i][0] = '\0';
		m->modal.git_results_path[i][0] = '\0';
		m->modal.git_results_mtime[i] = 0;
	}
			wlr_scene_node_set_enabled(&m->modal.tree->node, 0);
		}
	}
	if (!m->nixpkgs.tree) {
		m->nixpkgs.tree = wlr_scene_tree_create(layers[LyrTop]);
		if (m->nixpkgs.tree) {
			m->nixpkgs.bg = wlr_scene_tree_create(m->nixpkgs.tree);
			m->nixpkgs.visible = 0;
			m->nixpkgs.search[0] = '\0';
			m->nixpkgs.search_len = 0;
			m->nixpkgs.search_rendered[0] = '\0';
			m->nixpkgs.search_field_tree = NULL;
			m->nixpkgs.result_count = 0;
			m->nixpkgs.selected = -1;
			m->nixpkgs.scroll = 0;
			m->nixpkgs.results_tree = NULL;
			m->nixpkgs.last_scroll = 0;
			m->nixpkgs.last_selected = -1;
			m->nixpkgs.row_highlight_count = 0;
			for (int i = 0; i < MODAL_MAX_RESULTS; i++)
				m->nixpkgs.row_highlights[i] = NULL;
			wlr_scene_node_set_enabled(&m->nixpkgs.tree->node, 0);
		}
	}
	if (!m->sudo_popup.tree) {
		m->sudo_popup.tree = wlr_scene_tree_create(layers[LyrTop]);
		if (m->sudo_popup.tree) {
			m->sudo_popup.bg = wlr_scene_tree_create(m->sudo_popup.tree);
			m->sudo_popup.visible = 0;
			m->sudo_popup.title[0] = '\0';
			m->sudo_popup.password[0] = '\0';
			m->sudo_popup.password_len = 0;
			m->sudo_popup.cursor_pos = 0;
			m->sudo_popup.button_hover = 0;
			m->sudo_popup.running = 0;
			m->sudo_popup.error = 0;
			m->sudo_popup.pending_cmd[0] = '\0';
			m->sudo_popup.pending_pkg[0] = '\0';
			m->sudo_popup.sudo_pid = -1;
			m->sudo_popup.sudo_fd = -1;
			m->sudo_popup.sudo_event = NULL;
			m->sudo_popup.wait_timer = NULL;
			wlr_scene_node_set_enabled(&m->sudo_popup.tree->node, 0);
		}
	}
	if (!m->gamepad_menu.tree) {
		/* Use LyrBlock to ensure guide popup is always on top of fullscreen */
		m->gamepad_menu.tree = wlr_scene_tree_create(layers[LyrBlock]);
		if (m->gamepad_menu.tree) {
			m->gamepad_menu.bg = NULL;
			m->gamepad_menu.visible = 0;
			m->gamepad_menu.selected = 0;
			/* htpc.c removed — gamepad guide menu left empty */
		m->gamepad_menu.item_count = 0;
			wlr_scene_node_set_enabled(&m->gamepad_menu.tree->node, 0);
		}
	}
}

/* Tear down everything initstatusbar hung off the *global* layer trees.
 * layers[LyrTop]/[LyrBlock] outlive the monitor, so without this a
 * destroyed output (Nvidia hotplug/suspend re-enumerates its outputs)
 * leaves its bar behind as a frozen copy in layout space — the new
 * monitor's bar just gets drawn on top of the corpse. */
void
cleanupstatusbar(Monitor *m)
{
	if (!m)
		return;

	tray_menu_clear(&m->statusbar.tray_menu);
	m->statusbar.tray_menu.hover_rect = NULL;

	/* Popup card views hold scene pointers into the statusbar tree and
	 * may sit on the show-animation timer list — detach before the tree
	 * (and every node in it) is destroyed. */
	{
		PopupView *views[8] = { &m->statusbar.cpu_popup.view,
			&m->statusbar.ram_popup.view,
			&m->statusbar.battery_popup.view,
			&m->statusbar.net_popup.view,
			&m->statusbar.clock_popup.view,
			&m->statusbar.volume_popup.view,
			&m->statusbar.mic_popup.view,
			&m->statusbar.light_popup.view };
		for (int i = 0; i < 8; i++) {
			popup_view_hide(views[i]);
			memset(views[i], 0, sizeof(*views[i]));
		}
		m->statusbar.clock_popup.tree = NULL;
		m->statusbar.volume_popup.tree = NULL;
		m->statusbar.mic_popup.tree = NULL;
		m->statusbar.light_popup.tree = NULL;
	}

	if (m->statusbar.tree)
		wlr_scene_node_destroy(&m->statusbar.tree->node);
	if (m->modal.tree)
		wlr_scene_node_destroy(&m->modal.tree->node);
	if (m->nixpkgs.tree)
		wlr_scene_node_destroy(&m->nixpkgs.tree->node);
	if (m->sudo_popup.tree)
		wlr_scene_node_destroy(&m->sudo_popup.tree->node);
	if (m->gamepad_menu.tree)
		wlr_scene_node_destroy(&m->gamepad_menu.tree->node);

	m->statusbar.tree = NULL;
	m->modal.tree = NULL;
	m->nixpkgs.tree = NULL;
	m->sudo_popup.tree = NULL;
	m->gamepad_menu.tree = NULL;
}

void
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

void
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

/* Legacy stdout status printer (for external bars). The embedded bar
 * reads state directly; workspace.c owns the live printstatus(). Kept
 * static+unused to avoid a duplicate-symbol clash. */
__attribute__((unused)) static void
statusbar_printstatus_legacy(void)
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

/* Restart the status polling after a stretch with the timers disarmed
 * (game mode).  The rendered modules are left untouched: they still hold the
 * last state that was rasterized, so the bar is complete the instant it comes
 * back and each module repaints itself only once its own task reports new
 * text. */
void
statusbar_force_refresh(Monitor *m)
{
	size_t i;
	uint64_t now;

	if (!m)
		return;

	/* Drop the 8 s read throttles: a mute toggled from pavucontrol or a
	 * headset button while the bar was away must show its real state, not
	 * the level cached before it was hidden. */
	volume_invalidate_cache(0);
	volume_invalidate_cache(1);
	mic_last_read_ms = 0;

	/* Arm a full module re-render on the next shown layout — the parked
	 * modules may have lost their buffer contents while the bar was away. */
	m->statusbar.last_layout_h = 0;
	m->statusbar.tags.render_sig = 0;

	/* Re-run every status task — but not on the frames the bar is sliding
	 * in on.  volume/mic/light each fork+exec a helper (wpctl, light)
	 * synchronously on the main loop, tens of ms apiece, and the cache
	 * invalidation above guarantees they all take the slow path.  Firing
	 * all ten one event-loop iteration apart drops that stall straight
	 * into the slide-in and the bar visibly hitches into place.  Nothing
	 * on screen waits for them, so the reads wait for the tile-area spring
	 * to settle and then run one per frame or so.
	 * This also re-arms both timers, which game mode leaves disarmed:
	 * schedule_next_status_refresh bails while game_mode_active, so the
	 * tick that fired during game mode never rescheduled itself. */
	now = monotonic_msec();
	for (i = 0; i < LENGTH(status_tasks); i++)
		status_tasks[i].next_due_ms = now + STATUS_SETTLE_MS
				+ i * STATUS_STAGGER_MS;
	schedule_next_status_refresh();
	schedule_status_timer();
}

void
togglestatusbar(const Arg *arg)
{
	(void)arg;
	if (!selmon)
		return;
	selmon->showbar = !selmon->showbar;
	diag_logf("BAR", "toggle showbar=%d", selmon->showbar);
	/* No refresh on re-show: the modules kept rendering while hidden, so
	 * the bar that slides back in is the same one that slid out. */
	arrangelayers(selmon);
}


/* Anchor for every bar popup/dropdown: not flush against the bar, but at
 * the top edge of the tiles below it (bar bottom + the outer tile gap).
 * Hover keep-alive rects still start at the bar bottom so the cursor can
 * cross the gap strip without the popup closing. */
int
statusbar_popup_y(Monitor *m)
{
	int gap = (m->gaps && gappx > 0) ? (int)gappx : 0;

	return m->statusbar.area.height + gap;
}

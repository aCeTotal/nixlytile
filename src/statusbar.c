#include "nixlytile.h"
#include "client.h"

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
		} else if ((c & 0xE0) == 0xC0) {
			cp = (c & 0x1F) << 6;
			if (text[i + 1])
				cp |= ((unsigned char)text[i + 1] & 0x3F);
			i += 2;
		} else if ((c & 0xF0) == 0xE0) {
			cp = (c & 0x0F) << 12;
			if (text[i + 1]) {
				cp |= ((unsigned char)text[i + 1] & 0x3F) << 6;
				if (text[i + 2])
					cp |= ((unsigned char)text[i + 2] & 0x3F);
			}
			i += 3;
		} else if ((c & 0xF8) == 0xF0) {
			cp = (c & 0x07) << 18;
			if (text[i + 1]) {
				cp |= ((unsigned char)text[i + 1] & 0x3F) << 12;
				if (text[i + 2]) {
					cp |= ((unsigned char)text[i + 2] & 0x3F) << 6;
					if (text[i + 3])
						cp |= ((unsigned char)text[i + 3] & 0x3F);
				}
			}
			i += 4;
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
			/* 2-byte sequence (110xxxxx 10xxxxxx) */
			cp = (c & 0x1F) << 6;
			if (text[i + 1])
				cp |= ((unsigned char)text[i + 1] & 0x3F);
			i += 2;
		} else if ((c & 0xF0) == 0xE0) {
			/* 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx) */
			cp = (c & 0x0F) << 12;
			if (text[i + 1]) {
				cp |= ((unsigned char)text[i + 1] & 0x3F) << 6;
				if (text[i + 2])
					cp |= ((unsigned char)text[i + 2] & 0x3F);
			}
			i += 3;
		} else if ((c & 0xF8) == 0xF0) {
			/* 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx) */
			cp = (c & 0x07) << 18;
			if (text[i + 1]) {
				cp |= ((unsigned char)text[i + 1] & 0x3F) << 12;
				if (text[i + 2]) {
					cp |= ((unsigned char)text[i + 2] & 0x3F) << 6;
					if (text[i + 3])
						cp |= ((unsigned char)text[i + 3] & 0x3F);
				}
			}
			i += 4;
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
	/* Net text module is disabled; keep node hidden */
	if (module && module->tree) {
		clearstatusmodule(module);
		module->width = 0;
		wlr_scene_node_set_enabled(&module->tree->node, 0);
	}
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
			status_text_width("100%"), statusbar_icon_text_gap_battery, statusbar_fg);
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
			status_text_width("100%"), statusbar_icon_text_gap_volume, volume_text_color);
}

void
rendermic(StatusModule *module, int bar_height, const char *text)
{
	render_icon_label(module, bar_height, text,
			ensure_mic_icon_buffer, &mic_icon_buf, &mic_icon_w, &mic_icon_h,
			status_text_width("100%"), statusbar_icon_text_gap_microphone, mic_text_color);
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

void
drop_bluetooth_icon_buffer(void)
{
	if (bluetooth_icon_buf) {
		wlr_buffer_drop(bluetooth_icon_buf);
		bluetooth_icon_buf = NULL;
	}
	bluetooth_icon_loaded_h = 0;
	bluetooth_icon_loaded_path[0] = '\0';
	bluetooth_icon_w = 0;
	bluetooth_icon_h = 0;
}

int
load_bluetooth_icon_buffer(const char *path, int target_h)
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

	if (tray_load_svg_pixbuf(load_path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(load_path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "bluetooth icon: failed to load '%s': %s", load_path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

	drop_bluetooth_icon_buffer();
	bluetooth_icon_buf = buf;
	bluetooth_icon_w = w;
	bluetooth_icon_h = h;
	bluetooth_icon_loaded_h = target_h;
	snprintf(bluetooth_icon_loaded_path, sizeof(bluetooth_icon_loaded_path), "%s", load_path);
	return 0;
}

int
ensure_bluetooth_icon_buffer(int target_h)
{
	if (target_h <= 0)
		return -1;

	if (bluetooth_icon_buf && bluetooth_icon_loaded_h == target_h &&
			strncmp(bluetooth_icon_loaded_path, bluetooth_icon_path, sizeof(bluetooth_icon_loaded_path)) == 0)
		return 0;

	return load_bluetooth_icon_buffer(bluetooth_icon_path, target_h);
}

void
drop_steam_icon_buffer(void)
{
	if (steam_icon_buf) {
		wlr_buffer_drop(steam_icon_buf);
		steam_icon_buf = NULL;
	}
	steam_icon_loaded_h = 0;
	steam_icon_loaded_path[0] = '\0';
	steam_icon_w = 0;
	steam_icon_h = 0;
}

int
load_steam_icon_buffer(const char *path, int target_h)
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

	if (tray_load_svg_pixbuf(load_path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(load_path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "steam icon: failed to load '%s': %s", load_path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

	drop_steam_icon_buffer();
	steam_icon_buf = buf;
	steam_icon_w = w;
	steam_icon_h = h;
	steam_icon_loaded_h = target_h;
	snprintf(steam_icon_loaded_path, sizeof(steam_icon_loaded_path), "%s", load_path);
	return 0;
}

int
ensure_steam_icon_buffer(int target_h)
{
	if (target_h <= 0)
		return -1;

	if (steam_icon_buf && steam_icon_loaded_h == target_h &&
			strncmp(steam_icon_loaded_path, steam_icon_path, sizeof(steam_icon_loaded_path)) == 0)
		return 0;

	return load_steam_icon_buffer(steam_icon_path, target_h);
}

void
drop_discord_icon_buffer(void)
{
	if (discord_icon_buf) {
		wlr_buffer_drop(discord_icon_buf);
		discord_icon_buf = NULL;
	}
	discord_icon_loaded_h = 0;
	discord_icon_loaded_path[0] = '\0';
	discord_icon_w = 0;
	discord_icon_h = 0;
}

int
load_discord_icon_buffer(const char *path, int target_h)
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

	if (tray_load_svg_pixbuf(load_path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(load_path, &gerr);
		if (!pixbuf) {
			if (gerr) {
				wlr_log(WLR_ERROR, "discord icon: failed to load '%s': %s", load_path, gerr->message);
				g_error_free(gerr);
			}
			return -1;
		}
	}

	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;

	drop_discord_icon_buffer();
	discord_icon_buf = buf;
	discord_icon_w = w;
	discord_icon_h = h;
	discord_icon_loaded_h = target_h;
	snprintf(discord_icon_loaded_path, sizeof(discord_icon_loaded_path), "%s", load_path);
	return 0;
}

int
ensure_discord_icon_buffer(int target_h)
{
	if (target_h <= 0)
		return -1;

	if (discord_icon_buf && discord_icon_loaded_h == target_h &&
			strncmp(discord_icon_loaded_path, discord_icon_path, sizeof(discord_icon_loaded_path)) == 0)
		return 0;

	return load_discord_icon_buffer(discord_icon_path, target_h);
}

void
renderbluetooth(Monitor *m, int bar_height)
{
	StatusModule *module;
	int padding, target_h, icon_x, icon_y, base_h;
	struct wlr_scene_buffer *scene_buf;

	if (!m || !m->statusbar.bluetooth.tree)
		return;

	module = &m->statusbar.bluetooth;
	clearstatusmodule(module);
	module->width = 0;
	module->x = 0;

	/* Only show if bluetooth is available */
	if (!bluetooth_available) {
		wlr_scene_node_set_enabled(&module->tree->node, 0);
		return;
	}

	padding = statusbar_module_padding / 2;
	if (padding < 1)
		padding = 1;
	base_h = bar_height - 2 * padding;
	target_h = (int)lround(base_h * 1.5);
	if (target_h > bar_height)
		target_h = bar_height;
	if (target_h <= 0)
		target_h = bar_height - padding;
	if (target_h <= 0)
		target_h = 1;

	if (ensure_bluetooth_icon_buffer(target_h) != 0 || !bluetooth_icon_buf ||
			bluetooth_icon_w <= 0 || bluetooth_icon_h <= 0) {
		wlr_scene_node_set_enabled(&module->tree->node, 0);
		return;
	}

	module->width = bluetooth_icon_w + 2 * padding;
	if (module->width < bluetooth_icon_w)
		module->width = bluetooth_icon_w;

	updatemodulebg(module, module->width, bar_height, statusbar_bg);

	scene_buf = wlr_scene_buffer_create(module->tree, NULL);
	if (scene_buf) {
		int usable_w = module->width - 2 * padding;
		icon_x = padding + MAX(0, (usable_w - bluetooth_icon_w) / 2);
		icon_y = MAX(0, (bar_height - bluetooth_icon_h) / 2);
		wlr_scene_buffer_set_buffer(scene_buf, bluetooth_icon_buf);
		wlr_scene_node_set_position(&scene_buf->node, icon_x, icon_y);
	}

	wlr_scene_node_set_enabled(&module->tree->node, module->width > 0);
}

void
rendersteam(Monitor *m, int bar_height)
{
	StatusModule *module;
	int padding, target_h, icon_x, icon_y, base_h;
	struct wlr_scene_buffer *scene_buf;

	if (!m || !m->statusbar.steam.tree)
		return;

	module = &m->statusbar.steam;
	clearstatusmodule(module);
	module->width = 0;
	module->x = 0;

	/* Only show if Steam process is running */
	steam_running = is_process_running("steam");
	if (!steam_running) {
		wlr_scene_node_set_enabled(&module->tree->node, 0);
		return;
	}

	padding = statusbar_module_padding / 2;
	if (padding < 1)
		padding = 1;
	base_h = bar_height - 2 * padding;
	target_h = (int)lround(base_h * 1.5);
	if (target_h > bar_height)
		target_h = bar_height;
	if (target_h <= 0)
		target_h = bar_height - padding;
	if (target_h <= 0)
		target_h = 1;

	if (ensure_steam_icon_buffer(target_h) != 0 || !steam_icon_buf ||
			steam_icon_w <= 0 || steam_icon_h <= 0) {
		wlr_scene_node_set_enabled(&module->tree->node, 0);
		return;
	}

	module->width = steam_icon_w + 2 * padding;
	if (module->width < steam_icon_w)
		module->width = steam_icon_w;

	updatemodulebg(module, module->width, bar_height, statusbar_bg);

	scene_buf = wlr_scene_buffer_create(module->tree, NULL);
	if (scene_buf) {
		int usable_w = module->width - 2 * padding;
		icon_x = padding + MAX(0, (usable_w - steam_icon_w) / 2);
		icon_y = MAX(0, (bar_height - steam_icon_h) / 2);
		wlr_scene_buffer_set_buffer(scene_buf, steam_icon_buf);
		wlr_scene_node_set_position(&scene_buf->node, icon_x, icon_y);
	}

	wlr_scene_node_set_enabled(&module->tree->node, module->width > 0);
}

void
renderdiscord(Monitor *m, int bar_height)
{
	StatusModule *module;
	int padding, target_h, icon_x, icon_y, base_h;
	struct wlr_scene_buffer *scene_buf;

	if (!m || !m->statusbar.discord.tree)
		return;

	module = &m->statusbar.discord;
	clearstatusmodule(module);
	module->width = 0;
	module->x = 0;

	/* Only show if Discord process is running.
	 * Check both "Discord" and ".Discord" for NixOS wrapped version */
	discord_running = is_process_running("Discord") || is_process_running(".Discord");
	if (!discord_running) {
		wlr_scene_node_set_enabled(&module->tree->node, 0);
		return;
	}

	padding = statusbar_module_padding / 2;
	if (padding < 1)
		padding = 1;
	base_h = bar_height - 2 * padding;
	target_h = (int)lround(base_h * 1.5);
	if (target_h > bar_height)
		target_h = bar_height;
	if (target_h <= 0)
		target_h = bar_height - padding;
	if (target_h <= 0)
		target_h = 1;

	if (ensure_discord_icon_buffer(target_h) != 0 || !discord_icon_buf ||
			discord_icon_w <= 0 || discord_icon_h <= 0) {
		wlr_scene_node_set_enabled(&module->tree->node, 0);
		return;
	}

	module->width = discord_icon_w + 2 * padding;
	if (module->width < discord_icon_w)
		module->width = discord_icon_w;

	updatemodulebg(module, module->width, bar_height, statusbar_bg);

	scene_buf = wlr_scene_buffer_create(module->tree, NULL);
	if (scene_buf) {
		int usable_w = module->width - 2 * padding;
		icon_x = padding + MAX(0, (usable_w - discord_icon_w) / 2);
		icon_y = MAX(0, (bar_height - discord_icon_h) / 2);
		wlr_scene_buffer_set_buffer(scene_buf, discord_icon_buf);
		wlr_scene_node_set_position(&scene_buf->node, icon_x, icon_y);
	}

	wlr_scene_node_set_enabled(&module->tree->node, module->width > 0);
}

void
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

		if (!*ent->d_name)
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
		/* Fallback: use pkill (non-blocking) */
		if (fork() == 0) {
			setsid();
			execlp("pkill", "pkill", "-9", "-x", name, (char *)NULL);
			_exit(127);
		}
	}
	return killed;
}

int
read_top_cpu_processes(CpuPopup *p)
{
	FILE *fp;
	char line[256];
	int count = 0;
	int lines = 0;

	if (!p)
		return 0;

	/* Use top for real-time CPU usage (not cumulative like ps pcpu) */
	fp = popen("top -bn1 -o %CPU 2>/dev/null | tail -n +8 | head -50", "r");
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

	pclose(fp);
	if (count > 1)
		qsort(p->procs, (size_t)count, sizeof(p->procs[0]), cpu_proc_cmp);
	p->proc_count = count;
	return count;
}

void
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
	int max_proc_text_w = 0;
	int any_has_kill = 0;
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

	/* First pass: find max text width for vertical kill button alignment */
	for (int i = 0; i < p->proc_count; i++) {
		CpuProcEntry *e = &p->procs[i];
		int cpu_disp = (int)lround(e->cpu < 0.0 ? 0.0 : e->cpu);
		int text_w;
		char proc_line[128];

		snprintf(proc_line, sizeof(proc_line), "%s %d%%", e->name, cpu_disp);
		text_w = status_text_width(proc_line);
		max_proc_text_w = MAX(max_proc_text_w, text_w);
		if (e->has_kill)
			any_has_kill = 1;
	}
	/* Calculate right_w with aligned kill buttons */
	if (p->proc_count > 0) {
		right_w = max_proc_text_w;
		if (any_has_kill)
			right_w += button_gap + kill_w;
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
			int btn_x = right_x + max_proc_text_w + button_gap;
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

int
read_top_ram_processes(RamPopup *p)
{
	FILE *fp;
	char line[256];
	int count = 0;
	int lines = 0;
	const unsigned long min_kb = 50 * 1024; /* 50 MB minimum */

	if (!p)
		return 0;

	/* ps with RSS (resident set size) in KB, sorted by memory */
	fp = popen("ps -eo pid,rss,comm --no-headers --sort=-rss", "r");
	if (!fp) {
		p->proc_count = 0;
		return 0;
	}

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

	pclose(fp);
	if (count > 1)
		qsort(p->procs, (size_t)count, sizeof(p->procs[0]), ram_proc_cmp);
	p->proc_count = count;
	return count;
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

	popup_x = ram_popup_clamped_x(m, p);
	cx = cursor->x;
	cy = cursor->y;
	lx = (int)round(cx) - m->m.x;
	ly = (int)round(cy) - m->m.y;
	rel_x = lx - popup_x;
	rel_y = ly - m->statusbar.area.height;

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
	int padding, line_spacing;
	int row_height;
	int button_gap;
	int kill_text_w;
	int kill_w;
	int kill_h;
	uint64_t now;
	int hover_idx;
	int popup_x;
	int need_fetch_now;
	int max_proc_text_w = 0;
	int any_has_kill = 0;
	int content_w = 0, content_h = 0;
	struct wlr_scene_node *node, *tmp;

	if (!m || !m->statusbar.ram_popup.tree)
		return;

	p = &m->statusbar.ram_popup;
	hover_idx = p->hover_idx;
	if (hover_idx < -1 || hover_idx >= (int)LENGTH(p->procs))
		hover_idx = -1;
	padding = statusbar_module_padding;
	line_spacing = 2;
	row_height = statusfont.height > 0 ? statusfont.height : 16;
	button_gap = statusbar_module_spacing > 0 ? statusbar_module_spacing / 2 : 6;
	kill_text_w = status_text_width("Kill");
	kill_w = kill_text_w + 12;
	kill_h = row_height;
	now = monotonic_msec();
	need_fetch_now = 0;
	popup_x = m->statusbar.ram.x;

	/* Clear previous buffers but keep bg */
	wl_list_for_each_safe(node, tmp, &p->tree->children, link) {
		if (p->bg && node == &p->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

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

	if (p->proc_count == 0) {
		/* Don't hide if we're waiting for data to load (suppress period) */
		if (p->suppress_refresh_until_ms > 0 && now < p->suppress_refresh_until_ms) {
			/* Show loading placeholder */
			const char *loading = "Loading...";
			int text_w = status_text_width(loading);
			struct wlr_scene_tree *row;
			StatusModule mod = {0};

			p->width = 2 * padding + text_w;
			p->height = 2 * padding + row_height;

			if (!p->bg && !(p->bg = wlr_scene_tree_create(p->tree)))
				return;
			wlr_scene_node_set_enabled(&p->bg->node, 1);
			wlr_scene_node_set_position(&p->bg->node, 0, 0);
			wl_list_for_each_safe(node, tmp, &p->bg->children, link)
				wlr_scene_node_destroy(node);
			drawrect(p->bg, 0, 0, p->width, p->height, statusbar_popup_bg);

			row = wlr_scene_tree_create(p->tree);
			if (row) {
				wlr_scene_node_set_position(&row->node, padding, padding);
				mod.tree = row;
				tray_render_label(&mod, loading, 0, row_height, statusbar_fg);
			}
			p->last_render_ms = now;
			return;
		}
		p->width = p->height = 0;
		if (p->tree)
			wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->visible = 0;
		return;
	}

	/* First pass: find max text width for vertical kill button alignment */
	for (int i = 0; i < p->proc_count; i++) {
		RamProcEntry *e = &p->procs[i];
		int text_w;
		char proc_line[128];
		char mem_str[16];

		format_mem_size(e->mem_kb, mem_str, sizeof(mem_str));
		snprintf(proc_line, sizeof(proc_line), "%s %s", e->name, mem_str);
		text_w = status_text_width(proc_line);
		max_proc_text_w = MAX(max_proc_text_w, text_w);
		if (e->has_kill)
			any_has_kill = 1;
	}

	/* Calculate dimensions */
	content_w = max_proc_text_w;
	if (any_has_kill)
		content_w += button_gap + kill_w;
	content_h = p->proc_count * row_height + (p->proc_count - 1) * line_spacing;

	p->width = 2 * padding + content_w;
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

	/* Render process rows */
	for (int i = 0; i < p->proc_count; i++) {
		RamProcEntry *e = &p->procs[i];
		int row_y = padding + i * (row_height + line_spacing);
		char proc_line[128];
		char mem_str[16];
		struct wlr_scene_tree *row;
		StatusModule mod = {0};

		format_mem_size(e->mem_kb, mem_str, sizeof(mem_str));
		snprintf(proc_line, sizeof(proc_line), "%s %s", e->name, mem_str);
		row = wlr_scene_tree_create(p->tree);
		if (row) {
			wlr_scene_node_set_position(&row->node, padding, row_y);
			mod.tree = row;
			tray_render_label(&mod, proc_line, 0, row_height, statusbar_fg);
		}

		e->y = row_y;
		e->height = row_height;

		if (e->has_kill && kill_w > 0 && kill_h > 0) {
			int btn_x = padding + max_proc_text_w + button_gap;
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

	if (p->width <= 0 || p->height <= 0)
		wlr_scene_node_set_enabled(&p->tree->node, 0);
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
	rel_y = ly - m->statusbar.area.height;
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
void
read_battery_info(BatteryPopup *p)
{
	char path[PATH_MAX];
	char buf[64];
	FILE *fp;
	unsigned long long val;

	if (!p || !battery_available || !battery_device_dir[0])
		return;

	/* Read charging status */
	snprintf(path, sizeof(path), "%s/status", battery_device_dir);
	fp = fopen(path, "r");
	if (fp) {
		if (fgets(buf, sizeof(buf), fp)) {
			char *nl = strchr(buf, '\n');
			if (nl) *nl = '\0';
			p->charging = (strcmp(buf, "Charging") == 0 || strcmp(buf, "Full") == 0);
		}
		fclose(fp);
	}

	/* Read capacity percentage */
	p->percent = battery_percent();

	/* Read voltage (microvolts -> volts) */
	snprintf(path, sizeof(path), "%s/voltage_now", battery_device_dir);
	if (readulong(path, &val) == 0) {
		p->voltage_v = val / 1000000.0;
	} else {
		p->voltage_v = -1.0;
	}

	/* Read power draw (microwatts -> watts) */
	snprintf(path, sizeof(path), "%s/power_now", battery_device_dir);
	if (readulong(path, &val) == 0) {
		p->power_w = val / 1000000.0;
	} else {
		/* Try current_now * voltage_now if power_now not available */
		snprintf(path, sizeof(path), "%s/current_now", battery_device_dir);
		if (readulong(path, &val) == 0 && p->voltage_v > 0) {
			double current_a = val / 1000000.0;
			p->power_w = current_a * p->voltage_v;
		} else {
			p->power_w = -1.0;
		}
	}

	/* Calculate time remaining */
	p->time_remaining_h = -1.0;
	if (!p->charging && p->power_w > 0.1 && p->percent >= 0) {
		/* Read energy_now (microwatt-hours) */
		snprintf(path, sizeof(path), "%s/energy_now", battery_device_dir);
		if (readulong(path, &val) == 0) {
			double energy_wh = val / 1000000.0;
			p->time_remaining_h = energy_wh / p->power_w;
		} else {
			/* Try charge_now * voltage */
			snprintf(path, sizeof(path), "%s/charge_now", battery_device_dir);
			if (readulong(path, &val) == 0 && p->voltage_v > 0) {
				double charge_ah = val / 1000000.0;
				double energy_wh = charge_ah * p->voltage_v;
				p->time_remaining_h = energy_wh / p->power_w;
			}
		}
	}
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

void
renderbatterypopup(Monitor *m)
{
	BatteryPopup *p;
	int padding, line_spacing;
	int row_height;
	int max_width = 0;
	int line_count = 0;
	char lines[5][128];
	int popup_x;
	uint64_t now;
	struct wlr_scene_node *node, *tmp;

	if (!m || !m->statusbar.battery_popup.tree)
		return;

	p = &m->statusbar.battery_popup;
	padding = statusbar_module_padding;
	line_spacing = 2;
	row_height = statusfont.height > 0 ? statusfont.height : 16;
	now = monotonic_msec();
	popup_x = m->statusbar.battery.x;

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &p->tree->children, link) {
		if (p->bg && node == &p->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

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

	/* Build display lines */
	if (p->charging) {
		snprintf(lines[line_count++], sizeof(lines[0]), "Charging:");
	} else {
		snprintf(lines[line_count++], sizeof(lines[0]), "On Battery:");
	}

	if (p->percent >= 0) {
		snprintf(lines[line_count++], sizeof(lines[0]), "Level: %.0f%%", p->percent);
	}

	if (p->voltage_v > 0) {
		snprintf(lines[line_count++], sizeof(lines[0]), "Voltage: %.2f V", p->voltage_v);
	}

	if (p->power_w > 0) {
		snprintf(lines[line_count++], sizeof(lines[0]), "Power: %.1f W", p->power_w);
	}

	if (!p->charging && p->time_remaining_h > 0) {
		int hours = (int)p->time_remaining_h;
		int mins = (int)((p->time_remaining_h - hours) * 60);
		snprintf(lines[line_count++], sizeof(lines[0]), "Remaining: %dh %dm", hours, mins);
	}

	if (line_count == 0) {
		p->width = p->height = 0;
		if (p->tree)
			wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->visible = 0;
		return;
	}

	/* Calculate dimensions */
	for (int i = 0; i < line_count; i++) {
		int w = status_text_width(lines[i]);
		max_width = MAX(max_width, w);
	}

	p->width = 2 * padding + max_width;
	p->height = 2 * padding + line_count * row_height + (line_count - 1) * line_spacing;

	/* Clamp position */
	if (p->width > 0 && m->statusbar.area.width > 0) {
		int max_x = m->statusbar.area.width - p->width;
		if (max_x < 0)
			max_x = 0;
		if (popup_x > max_x)
			popup_x = max_x;
		if (popup_x < 0)
			popup_x = 0;
	}

	/* Draw background */
	if (!p->bg && !(p->bg = wlr_scene_tree_create(p->tree)))
		return;
	wlr_scene_node_set_enabled(&p->bg->node, 1);
	wlr_scene_node_set_position(&p->bg->node, 0, 0);
	wl_list_for_each_safe(node, tmp, &p->bg->children, link)
		wlr_scene_node_destroy(node);
	drawrect(p->bg, 0, 0, p->width, p->height, statusbar_popup_bg);

	/* Render text lines */
	for (int i = 0; i < line_count; i++) {
		int row_y = padding + i * (row_height + line_spacing);
		struct wlr_scene_tree *row;
		StatusModule mod = {0};

		row = wlr_scene_tree_create(p->tree);
		if (row) {
			wlr_scene_node_set_position(&row->node, padding, row_y);
			mod.tree = row;
			tray_render_label(&mod, lines[i], 0, row_height, statusbar_fg);
		}
	}

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
			ly < m->statusbar.area.height + p->height) {
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
				popup_x, m->statusbar.area.height);

		/* Check if refresh is needed (every 500ms after delay) */
		stale_refresh = (p->last_fetch_ms == 0 ||
				now < p->last_fetch_ms ||
				(now - p->last_fetch_ms) >= 500);
		need_refresh = stale_refresh &&
				(p->suppress_refresh_until_ms == 0 ||
				 now >= p->suppress_refresh_until_ms);

		if (need_refresh)
			p->refresh_data = 1;

		if (!was_visible || need_refresh ||
				(p->last_render_ms == 0 || (now - p->last_render_ms) >= 100)) {
			renderbatterypopup(m);
			p->last_render_ms = now;
		}
	} else if (p->visible || p->hover_start_ms != 0) {
		p->visible = 0;
		p->suppress_refresh_until_ms = 0;
		p->hover_start_ms = 0;
		wlr_scene_node_set_enabled(&p->tree->node, 0);
	}
}

void
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

void
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

int
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

double
ramused_mb(void)
{
	unsigned long long total, avail;

	if (readmeminfo(&total, &avail) != 0)
		return -1.0;

	if (total <= avail)
		return 0.0;

	return (double)(total - avail) / 1024.0;
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
		/* Also store the device directory */
		snprintf(battery_device_dir, sizeof(battery_device_dir),
				"/sys/class/power_supply/%s", ent->d_name);
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

int
set_backlight_percent(double percent)
{
	unsigned long long max, target;
	FILE *fp;
	int attempted __attribute__((unused)) = 0;

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
	unsigned long long cur;

	if (!battery_available)
		return -1.0;
	if (readulong(battery_capacity_path, &cur) != 0)
		return -1.0;
	if (cur > 100)
		cur = 100;

	return (double)cur;
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

void
volume_invalidate_cache(int is_headset)
{
	if (is_headset)
		volume_last_read_headset_ms = 0;
	else
		volume_last_read_speaker_ms = 0;
}

double
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

double
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

int
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
	int ret;
	int current;
	int is_headset = pipewire_sink_is_headset();
	double vol;
	uint64_t now;
	double base;
	double target;

	/* Always force a fresh read from PipeWire to get accurate mute state */
	volume_invalidate_cache(is_headset);
	vol = pipewire_volume_percent(&is_headset);
	/*
	 * Determine current mute state. If volume_muted is still -1 (unknown),
	 * treat it as unmuted (0) so first click will mute.
	 */
	current = (vol >= 0.0 && volume_muted == 1) ? 1 : 0;
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

int
toggle_pipewire_mic_mute(void)
{
	int ret;
	int current;
	double vol;
	double base;
	double target;

	/* Always force a fresh read from PipeWire to get accurate mute state */
	mic_last_read_ms = 0;
	vol = pipewire_mic_volume_percent();
	/*
	 * Determine current mute state. If mic_muted is still -1 (unknown),
	 * treat it as unmuted (0) so first click will mute.
	 */
	current = (vol >= 0.0 && mic_muted == 1) ? 1 : 0;
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

void
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
		if (m->statusbar.bluetooth.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.bluetooth.tree->node, 0);
			m->statusbar.bluetooth.x = 0;
		}
		if (m->statusbar.steam.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.steam.tree->node, 0);
			m->statusbar.steam.x = 0;
		}
		if (m->statusbar.discord.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.discord.tree->node, 0);
			m->statusbar.discord.x = 0;
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
	if (m->statusbar.bluetooth.tree)
		wlr_scene_node_set_enabled(&m->statusbar.bluetooth.tree->node,
				m->statusbar.bluetooth.width > 0);
	if (m->statusbar.steam.tree)
		wlr_scene_node_set_enabled(&m->statusbar.steam.tree->node,
				m->statusbar.steam.width > 0);
	if (m->statusbar.discord.tree)
		wlr_scene_node_set_enabled(&m->statusbar.discord.tree->node,
				m->statusbar.discord.width > 0);
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
	/* Group connection-related icons together with minimal spacing */
	if (m->statusbar.sysicons.width > 0) {
		wlr_scene_node_set_position(&m->statusbar.sysicons.tree->node, x, 0);
		m->statusbar.sysicons.x = x;
		x += m->statusbar.sysicons.width + 2;
	}
	if (m->statusbar.bluetooth.width > 0) {
		wlr_scene_node_set_position(&m->statusbar.bluetooth.tree->node, x, 0);
		m->statusbar.bluetooth.x = x;
		x += m->statusbar.bluetooth.width + 2;
	}
	if (m->statusbar.steam.width > 0) {
		wlr_scene_node_set_position(&m->statusbar.steam.tree->node, x, 0);
		m->statusbar.steam.x = x;
		x += m->statusbar.steam.width + 2;
	}
	if (m->statusbar.discord.width > 0) {
		wlr_scene_node_set_position(&m->statusbar.discord.tree->node, x, 0);
		m->statusbar.discord.x = x;
		x += m->statusbar.discord.width + spacing;
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

void
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
	if (m->statusbar.bluetooth.tree)
		renderbluetooth(m, bar_area.height);
	if (m->statusbar.steam.tree)
		rendersteam(m, bar_area.height);
	if (m->statusbar.discord.tree)
		renderdiscord(m, bar_area.height);
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
		if (!m->statusbar.clock.tree || !m->showbar)
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

void
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
			request_public_ip_async_ex(1); /* force update when hovering */

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

void
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

void
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

void
refreshstatusicons(void)
{
	Monitor *m;
	int barh;

	wl_list_for_each(m, &mons, link) {
		if (!m->showbar)
			continue;
		barh = m->statusbar.area.height ? m->statusbar.area.height : (int)statusbar_height;
		if (m->statusbar.sysicons.tree)
			rendertrayicons(m, barh);
		if (m->statusbar.bluetooth.tree)
			renderbluetooth(m, barh);
		if (m->statusbar.steam.tree)
			rendersteam(m, barh);
		if (m->statusbar.discord.tree)
			renderdiscord(m, barh);
		positionstatusmodules(m);
	}
}

void
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

void
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
	request_public_ip_async(); /* prefetch public IP in background */
	refreshstatusicons();
	refreshstatustags();
}

void
init_status_refresh_tasks(void)
{
	uint64_t now = monotonic_msec();
	uint32_t offset = 100;

	for (size_t i = 0; i < STATUS_TASKS_COUNT; i++) {
		status_tasks[i].next_due_ms = now + offset;
		offset += 200; /* stagger initial fills to avoid clumping */
	}
}

void
trigger_status_task_now(void (*fn)(void))
{
	uint64_t now = monotonic_msec();

	for (size_t i = 0; i < STATUS_TASKS_COUNT; i++) {
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
	for (size_t i = 0; i < STATUS_TASKS_COUNT; i++) {
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
	return 0;
}

void
schedule_next_status_refresh(void)
{
	uint64_t now = monotonic_msec();
	uint64_t next = UINT64_MAX;

	if (!status_cpu_timer || game_mode_active || htpc_mode_active)
		return;

	for (size_t i = 0; i < STATUS_TASKS_COUNT; i++) {
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

	for (size_t i = 0; i < STATUS_TASKS_COUNT; i++) {
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
			schedule_cpu_popup_refresh(1000);
	} else if (p->visible || p->hover_start_ms != 0) {
		p->visible = 0;
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->hover_idx = -1;
		p->refresh_data = 0;
		p->last_render_ms = 0;
		p->suppress_refresh_until_ms = 0;
		p->hover_start_ms = 0;
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
			ly < m->statusbar.area.height + p->height) {
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

		if (!was_visible) {
			/* Short delay to avoid flicker on quick mouse movements */
			p->suppress_refresh_until_ms = now + 100;
			p->refresh_data = 1; /* Trigger immediate fetch when delay passes */
		}
		p->visible = 1;
		wlr_scene_node_set_enabled(&p->tree->node, 1);
		wlr_scene_node_set_position(&p->tree->node,
				popup_x, m->statusbar.area.height);
		new_hover = ram_popup_hover_index(m, p);
		stale_refresh = (p->last_fetch_ms == 0 ||
				now < p->last_fetch_ms ||
				(now - p->last_fetch_ms) >= ram_popup_refresh_interval_ms);
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
				renderrampopup(m);
			}
		}
		if (!was_visible)
			schedule_ram_popup_refresh(100);
	} else if (p->visible || p->hover_start_ms != 0) {
		p->visible = 0;
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->hover_idx = -1;
		p->refresh_data = 0;
		p->last_render_ms = 0;
		p->suppress_refresh_until_ms = 0;
		p->hover_start_ms = 0;
	}
}

void
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
				icon_mod->width > 0 ? icon_mod->x : p->anchor_x,
				m->statusbar.area.height);
		if (!was_visible) {
			p->anchor_x = icon_mod->width > 0 ? icon_mod->x : p->anchor_x;
			p->anchor_y = m->statusbar.area.height;
			p->anchor_w = icon_mod->width > 0 ? icon_mod->width : p->anchor_w;
			rendernetpopup(m);
		}
	} else if (p->visible || p->hover_start_ms != 0) {
		p->visible = 0;
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->suppress_refresh_until_ms = 0;
		p->hover_start_ms = 0;
		set_status_task_due(refreshstatusnet, now + 60000);
	}
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
	if (!wifi_networks_initialized) {
		wl_list_init(&wifi_networks);
		wifi_networks_initialized = 1;
	}
	if (!vpn_list_initialized) {
		wl_list_init(&vpn_connections);
		vpn_list_initialized = 1;
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
		m->statusbar.bluetooth.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.bluetooth.tree)
			m->statusbar.bluetooth.bg = wlr_scene_tree_create(m->statusbar.bluetooth.tree);
		m->statusbar.steam.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.steam.tree)
			m->statusbar.steam.bg = wlr_scene_tree_create(m->statusbar.steam.tree);
		m->statusbar.discord.tree = wlr_scene_tree_create(m->statusbar.tree);
		if (m->statusbar.discord.tree)
			m->statusbar.discord.bg = wlr_scene_tree_create(m->statusbar.discord.tree);
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
	if (!m->wifi_popup.tree) {
		m->wifi_popup.tree = wlr_scene_tree_create(layers[LyrTop]);
		if (m->wifi_popup.tree) {
			m->wifi_popup.bg = NULL;
			m->wifi_popup.visible = 0;
			m->wifi_popup.ssid[0] = '\0';
			m->wifi_popup.password[0] = '\0';
			m->wifi_popup.password_len = 0;
			m->wifi_popup.cursor_pos = 0;
			m->wifi_popup.button_hover = 0;
			m->wifi_popup.connecting = 0;
			m->wifi_popup.error = 0;
			m->wifi_popup.try_saved = 0;
			m->wifi_popup.connect_pid = -1;
			m->wifi_popup.connect_fd = -1;
			m->wifi_popup.connect_event = NULL;
			wlr_scene_node_set_enabled(&m->wifi_popup.tree->node, 0);
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
			htpc_menu_build();
		m->gamepad_menu.item_count = htpc_menu_item_count;
			wlr_scene_node_set_enabled(&m->gamepad_menu.tree->node, 0);
		}
	}
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
togglestatusbar(const Arg *arg)
{
	(void)arg;
	if (!selmon)
		return;
	selmon->showbar = !selmon->showbar;
	arrangelayers(selmon);
}


#include "nixlytile.h"

/* Ten steps, empty to full. */
static const uint32_t battery_glyphs[2][10] = {
	{ 0xF007A, 0xF007B, 0xF007C, 0xF007D, 0xF007E,
	  0xF007F, 0xF0080, 0xF0081, 0xF0082, 0xF0079 },
	{ 0xF089C, 0xF0086, 0xF0087, 0xF0088, 0xF089D,
	  0xF0089, 0xF089E, 0xF008A, 0xF008B, 0xF0085 },
};
#define BATTERY_STEPS (int)LENGTH(battery_glyphs[0])

uint32_t
bar_battery_glyph(double percent, int charging)
{
	int step = (int)lround(percent * BATTERY_STEPS / 100.0) - 1;

	if (percent < 0.0)
		return GLYPH_BATTERY_UNKNOWN;
	return battery_glyphs[charging != 0][MAX(0, MIN(step, BATTERY_STEPS - 1))];
}

/* State colours also tint icons. */
const float *
bar_icon_color(const float *text_fg)
{
	return text_fg == statusbar_fg ? statusbar_icon_fg : text_fg;
}

void
bar_module_clear(StatusModule *mod)
{
	if (mod->content) {
		nx_scene_node_destroy(&mod->content->node);
		mod->content = NULL;
	}
	mod->width = 0;
	mod->render_sig = 0;
	if (mod->tree)
		nx_scene_node_set_enabled(&mod->tree->node, 0);
}

/* Takes ownership of buf; 0 on failure. */
int
bar_module_attach(StatusModule *mod, struct nx_buffer *buf)
{
	if (!mod->content)
		mod->content = nx_scene_buffer_create(mod->tree, NULL);
	if (mod->content)
		nx_scene_buffer_set_buffer(mod->content, buf);
	nx_buffer_drop(buf);
	return mod->content != NULL;
}

static uint64_t
color_key(const float *c)
{
	uint64_t key = 0;

	for (int i = 0; c && i < 4; i++)
		key = key << 8 | (uint64_t)lroundf(c[i] * 255.0f);
	return key;
}

/* Values, not pointers: themes recolour in place. */
static uint64_t
label_sig(const BarLabel *l, int bar_h)
{
	uint64_t sig = FNV_OFFSET;

	sig = sig_mix(sig, (uint64_t)bar_h);
	sig = sig_mix(sig, l->icon);
	sig = sig_mix(sig, color_key(l->icon_fg));
	sig = sig_mix(sig, color_key(l->text_fg));
	for (const char *c = l->text ? l->text : ""; *c; c++)
		sig = sig_mix(sig, (unsigned char)*c);
	return sig;
}

/* One texture per module. */
void
bar_module_draw(StatusModule *mod, int bar_h, const BarLabel *l)
{
	BarLabel shown = *l;
	struct nx_buffer *buf;
	uint64_t sig;
	int pad;

	if (!mod->tree)
		return;
	if (mod->icons_only && bar_icon_width(bar_h, shown.icon))
		shown.text = NULL;
	sig = label_sig(&shown, bar_h);
	if (sig == mod->render_sig && mod->content && mod->width > 0)
		return;
	buf = bar_label_buffer(&shown, bar_h);
	if (!buf) {
		bar_module_clear(mod);
		return;
	}
	pad = shown.text && *shown.text ? BAR_TEXT_PAD : BAR_ICON_PAD;
	mod->width = buf->width + 2 * pad;
	if (!bar_module_attach(mod, buf)) {
		bar_module_clear(mod);
		return;
	}
	nx_scene_node_set_position(&mod->content->node, pad, 0);
	mod->render_sig = sig;
}

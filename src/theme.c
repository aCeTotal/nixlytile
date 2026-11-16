#include "nixlytile.h"
#include "popup_card.h"

#define THEME_FILE "/.local/state/nixly-theme/nixlytile.conf"
#define TEXT_CONTRAST 4.5
#define GLASS_MIN 0.40
#define GLASS_MAX 0.92
#define EDGE_LIFT 0.15
#define HOVER_LIFT 0.30

enum {
	R_SURFACE, R_LOWEST, R_CONTAINER, R_HIGHEST,
	R_ON_SURFACE, R_ON_VARIANT, R_OUTLINE, R_OUTLINE_VARIANT,
	R_PRIMARY, R_SECONDARY, R_SECONDARY_CONTAINER, R_ON_SECONDARY_CONTAINER,
	R_ERROR, R_GREEN, R_YELLOW,
	ROLES,
};

static const char *const role_names[ROLES] = {
	[R_SURFACE] = "surface",
	[R_LOWEST] = "surface_container_lowest",
	[R_CONTAINER] = "surface_container",
	[R_HIGHEST] = "surface_container_highest",
	[R_ON_SURFACE] = "on_surface",
	[R_ON_VARIANT] = "on_surface_variant",
	[R_OUTLINE] = "outline",
	[R_OUTLINE_VARIANT] = "outline_variant",
	[R_PRIMARY] = "primary",
	[R_SECONDARY] = "secondary",
	[R_SECONDARY_CONTAINER] = "secondary_container",
	[R_ON_SECONDARY_CONTAINER] = "on_secondary_container",
	[R_ERROR] = "error",
	[R_GREEN] = "green",
	[R_YELLOW] = "yellow",
};

#define ALL_ROLES ((1u << ROLES) - 1)

typedef struct {
	float rgb[ROLES][3];
	uint32_t have;
} Palette;

enum alpha { OPAQUE, GLASS, EDGE, HOVER };

/* Bar slots and palette roles. */
static const struct {
	float *dst;
	int role;
	enum alpha alpha;
} bar_slots[] = {
	{ statusbar_fg, R_ON_SURFACE, OPAQUE },
	{ statusbar_dim_fg, R_ON_VARIANT, OPAQUE },
	{ statusbar_icon_fg, R_SECONDARY, OPAQUE },
	{ statusbar_ok_fg, R_GREEN, OPAQUE },
	{ statusbar_warn_fg, R_YELLOW, OPAQUE },
	{ statusbar_crit_fg, R_ERROR, OPAQUE },
	{ statusbar_island_bg, R_LOWEST, GLASS },
	{ statusbar_island_edge, R_OUTLINE_VARIANT, EDGE },
	{ statusbar_hover_bg, R_HIGHEST, HOVER },
	{ statusbar_ws_active_bg, R_SECONDARY_CONTAINER, OPAQUE },
	{ statusbar_ws_active_fg, R_ON_SECONDARY_CONTAINER, OPAQUE },
};

/* Popups and window borders. */
static const struct {
	float *dst;
	int role;
} ui_slots[] = {
	{ card_col_bg, R_CONTAINER },
	{ card_col_edge, R_OUTLINE_VARIANT },
	{ card_col_fg, R_ON_SURFACE },
	{ card_col_dim, R_ON_VARIANT },
	{ card_col_faint, R_OUTLINE },
	{ card_col_green, R_GREEN },
	{ card_col_yellow, R_YELLOW },
	{ card_col_red, R_ERROR },
	{ card_col_blue, R_PRIMARY },
	{ focuscolor, R_PRIMARY },
	{ bordercolor, R_OUTLINE_VARIANT },
	{ urgentcolor, R_ERROR },
	{ rootcolor, R_SURFACE },
};

static Backdrop backdrop;

static int
role_index(const char *name)
{
	for (int r = 0; r < ROLES; r++)
		if (!strcmp(role_names[r], name))
			return r;
	return -1;
}

/* "dark.<role> rrggbb"; other lines ignored. */
static void
parse_line(Palette *p, const char *line)
{
	char name[64];
	unsigned int hex;
	int r;

	if (sscanf(line, "dark.%63s %6x", name, &hex) != 2 || (r = role_index(name)) < 0)
		return;
	p->rgb[r][0] = (float)((hex >> 16) & 0xff) / 255.0f;
	p->rgb[r][1] = (float)((hex >> 8) & 0xff) / 255.0f;
	p->rgb[r][2] = (float)(hex & 0xff) / 255.0f;
	p->have |= 1u << r;
}

static int
load_palette(Palette *p)
{
	char path[PATH_MAX], line[160];
	const char *home = getenv("HOME");
	FILE *fp;

	if (!home)
		return 0;
	snprintf(path, sizeof(path), "%s" THEME_FILE, home);
	if (!(fp = fopen(path, "r")))
		return 0;
	*p = (Palette){ 0 };
	while (fgets(line, sizeof(line), fp))
		parse_line(p, line);
	fclose(fp);
	return p->have == ALL_ROLES;
}

static double
channel_linear(float c)
{
	return c <= 0.04045f ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double
luminance(const float rgb[3])
{
	return 0.2126 * channel_linear(rgb[0]) + 0.7152 * channel_linear(rgb[1])
		+ 0.0722 * channel_linear(rgb[2]);
}

/* Dark fill keeping dim text at 4.5:1. */
static double
glass_alpha(const Palette *p)
{
	double tint = luminance(p->rgb[R_LOWEST]);
	double text = luminance(p->rgb[R_ON_VARIANT]);
	double limit = (text + 0.05) / TEXT_CONTRAST - 0.05;
	double worst = backdrop.valid ? backdrop.y_hi : 0.0;
	double a = worst <= limit ? 0.0 : (worst - limit) / (worst - tint);

	return MAX(GLASS_MIN, MIN(a, GLASS_MAX));
}

static void
set_color(float dst[4], const float rgb[3], double a)
{
	dst[0] = rgb[0];
	dst[1] = rgb[1];
	dst[2] = rgb[2];
	dst[3] = (float)MIN(a, 1.0);
}

static double
slot_alpha(enum alpha kind, double glass)
{
	static const double lift[] = { [EDGE] = EDGE_LIFT, [HOVER] = HOVER_LIFT };

	if (kind == OPAQUE)
		return 1.0;
	return glass + lift[kind];
}

static double
apply_bar(const Palette *p)
{
	double glass = glass_alpha(p);

	for (size_t i = 0; i < LENGTH(bar_slots); i++)
		set_color(bar_slots[i].dst, p->rgb[bar_slots[i].role],
				slot_alpha(bar_slots[i].alpha, glass));
	return glass;
}

/* Recolour from generated palette; 0 if none. */
int
theme_apply(void)
{
	Palette palette;
	double glass;
	Monitor *m;

	if (!load_palette(&palette))
		return 0;
	glass = apply_bar(&palette);
	for (size_t i = 0; i < LENGTH(ui_slots); i++)
		set_color(ui_slots[i].dst, palette.rgb[ui_slots[i].role], 1.0);
	wl_list_for_each(m, &mons, link)
		statusbar_repaint(m);
	hotapply_appearance();
	nx_log(NX_INFO, "theme: recoloured, glass %.2f", glass);
	return 1;
}

void
theme_set_backdrop(const Backdrop *b)
{
	backdrop = *b;
}

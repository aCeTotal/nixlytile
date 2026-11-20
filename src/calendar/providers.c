/* Sync services and picker tiles. */
#include "calendar/view.h"

#define TILE_COLS 4
#define TILE_GAP  10
#define TILE_R    12.0
#define TILE_TOP  12
#define TILE_TEXT 12
#define TILE_ROWS ((PROVIDERS + TILE_COLS - 1) / TILE_COLS)

typedef struct {
	int active, hover;
	int tile_w, tile_h, icon;
} Tiles;

typedef struct {
	const float *rgb;
	double fill, edge, width;
} TileLook;

enum { TILE_IDLE, TILE_HOT, TILE_ON };

static const float white[4] = { 1, 1, 1, 1 };
static const TileLook looks[] = {
	[TILE_IDLE] = { white, 0.045, 0.08, 1.0 },
	[TILE_HOT] = { white, 0.10, 0.08, 1.0 },
	[TILE_ON] = { card_col_blue, 0.20, 0.85, 1.4 },
};

static const Provider providers[PROVIDERS] = {
	{ "google", "Google", "cal-google", 0x4c8bf5, 1, { 0 },
		"Opens your browser to sign in with Google.",
		calendar_config.google_id, calendar_config.google_secret,
		"Google Calendar → Settings → your calendar → Secret address in iCal format." },
	{ "microsoft", "Outlook", "cal-microsoft", 0x3a96dd, 1, { 0 },
		"Opens your browser to sign in with Microsoft.",
		calendar_config.microsoft_id, "",
		"Outlook → Settings → Calendar → Shared calendars → Publish → ICS link." },
	{ "icloud", "iCloud", "cal-apple", 0xd0d4dc, 0,
		{ [FORM_USER] = "Apple ID", [FORM_SECRET] = "App password" },
		"Create an app-specific password at account.apple.com.",
		NULL, NULL, NULL },
	{ "fastmail", "Fastmail", "cal-mail", 0x69a1f0, 0,
		{ [FORM_USER] = "Email", [FORM_SECRET] = "App password" },
		"Fastmail: Settings → Privacy & Security → App passwords.",
		NULL, NULL, NULL },
	{ "yahoo", "Yahoo", "cal-mail", 0x9b6cf7, 0,
		{ [FORM_USER] = "Email", [FORM_SECRET] = "App password" },
		"Yahoo: Account security → Generate app password.",
		NULL, NULL, NULL },
	{ "nextcloud", "Nextcloud", "cal-cloud", 0x2a9fd6, 0,
		{ "Server", "User", "App password" },
		"Nextcloud: Settings → Security → Create new app password.",
		NULL, NULL, NULL },
	{ "caldav", "CalDAV", "cal-server", 0x2fc6c8, 0,
		{ "Server URL", "User", "Password" },
		"Any CalDAV server — calendars are discovered automatically.",
		NULL, NULL, NULL },
	{ "ics", "ICS link", "cal-link", 0xf2a43a, 0,
		{ [FORM_URL] = "Calendar URL" },
		"Paste a secret iCal address, e.g. from Google or Outlook.",
		NULL, NULL, NULL },
};

static Tiles tiles;
static Provider linked;

const Provider *
provider_at(int i)
{
	return &providers[i];
}

const Provider *
provider_effective(int i)
{
	const Provider *p = &providers[i];

	if (!p->oauth || p->client_id[0])
		return p;
	linked = *p;
	linked.id = "ics";
	linked.oauth = 0;
	memset(linked.fields, 0, sizeof(linked.fields));
	linked.fields[FORM_URL] = "Calendar link";
	linked.hint = p->link_hint;
	return &linked;
}

const Provider *
provider_by_id(const char *id)
{
	for (int i = 0; i < PROVIDERS; i++)
		if (!strcmp(providers[i].id, id))
			return &providers[i];
	return NULL;
}

const char *
provider_name(const char *id)
{
	const Provider *p = provider_by_id(id);

	return p ? p->name : id;
}

static Box
tile_box(const Tiles *t, int i)
{
	return (Box){ i % TILE_COLS * (t->tile_w + TILE_GAP),
		i / TILE_COLS * (t->tile_h + TILE_GAP), t->tile_w, t->tile_h };
}

static void
tile_frame(cairo_t *cr, Box b, const TileLook *k)
{
	card_rounded(cr, (CardRound){ b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1,
			TILE_R });
	cairo_set_source_rgba(cr, k->rgb[0], k->rgb[1], k->rgb[2], k->fill);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, k->rgb[0], k->rgb[1], k->rgb[2], k->edge);
	cairo_set_line_width(cr, k->width);
	cairo_stroke(cr);
}

static int
tile_state(const Tiles *t, int i)
{
	if (t->active == i)
		return TILE_ON;
	return t->hover == i ? TILE_HOT : TILE_IDLE;
}

static void
tiles_shapes(cairo_t *cr, CardInk *ink, const void *model)
{
	const Tiles *t = model;

	for (int i = 0; i < PROVIDERS; i++) {
		const Provider *p = &providers[i];
		const IconGlyph *g = icon_glyph_for(p->icon);
		Box b = tile_box(t, i);

		tile_frame(cr, b, &looks[tile_state(t, i)]);
		if (g)
			paint_glyph(cr, (Box){ b.x + (b.w - t->icon) / 2,
					b.y + TILE_TOP, t->icon, t->icon },
					(Glyph){ g->cp, p->color });
		card_ink_hit(ink, b, HIT_PROVIDER + i);
	}
}

static void
tiles_text(CardInk *ink, const void *model)
{
	const Tiles *t = model;
	int base = card_font_height(CARD_FONT_BASE);

	for (int i = 0; i < PROVIDERS; i++) {
		Box b = tile_box(t, i);

		paint_text(ink, &(CardText){ CARD_FONT_BASE, providers[i].name,
				0, 0, t->active == i ? card_col_fg : card_col_dim },
				(Box){ b.x, b.y + b.h - base - TILE_TEXT, b.w, base });
	}
}

static const CardPainter tiles_painter = { tiles_shapes, tiles_text };

void
provider_tiles(Card *card, int w)
{
	int base = card_font_height(CARD_FONT_BASE);

	tiles.active = ui.provider;
	tiles.hover = ui.hover - HIT_PROVIDER;
	tiles.tile_w = (w - (TILE_COLS - 1) * TILE_GAP) / TILE_COLS;
	tiles.icon = base + 12;
	tiles.tile_h = tiles.icon + base + 32;
	card_custom(card, &(CardBlock){ &tiles_painter, &tiles, sizeof(tiles),
			w, TILE_ROWS * tiles.tile_h + (TILE_ROWS - 1) * TILE_GAP });
}

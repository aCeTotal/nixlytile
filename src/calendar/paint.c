/* Calendar drawing primitives. */
#include "calendar/view.h"

#define CHEVRON_ARM 0.18
#define CHEVRON_W   1.8
#define WASH_RADIUS 8.0
#define FIELD_R     8.0
#define FIELD_LINE  1.2

void
paint_rgb(cairo_t *cr, uint32_t rgb, double alpha)
{
	cairo_set_source_rgba(cr, ((rgb >> 16) & 0xff) / 255.0,
			((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0, alpha);
}

void
paint_accent(cairo_t *cr, double alpha)
{
	cairo_set_source_rgba(cr, card_col_blue[0], card_col_blue[1],
			card_col_blue[2], alpha);
}

void
paint_wash(cairo_t *cr, Box b, double alpha)
{
	card_rounded(cr, (CardRound){ b.x, b.y, b.w, b.h,
			MIN(WASH_RADIUS, b.h / 2.0) });
	cairo_set_source_rgba(cr, 1, 1, 1, alpha);
	cairo_fill(cr);
}

/* Left when dir < 0. */
void
paint_chevron(cairo_t *cr, Box b, int dir)
{
	double cx = b.x + b.w / 2.0, cy = b.y + b.h / 2.0;
	double arm = b.h * CHEVRON_ARM;

	cairo_save(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_width(cr, CHEVRON_W);
	cairo_move_to(cr, cx - dir * arm / 2, cy - arm);
	cairo_line_to(cr, cx + dir * arm / 2, cy);
	cairo_line_to(cr, cx - dir * arm / 2, cy + arm);
	cairo_set_source_rgba(cr, card_col_fg[0], card_col_fg[1],
			card_col_fg[2], 0.85);
	cairo_stroke(cr);
	cairo_restore(cr);
}

/* Input box outline by state. */
void
paint_field(cairo_t *cr, Box b, FieldLook look)
{
	static const double idle_alpha[] = {
		[LOOK_IDLE] = 0.10, [LOOK_HOVER] = 0.22,
	};

	card_rounded(cr, (CardRound){ b.x + 0.5, b.y + 0.5, b.w - 1, b.h - 1,
			FIELD_R });
	cairo_set_source_rgba(cr, 1, 1, 1, 0.04);
	cairo_fill_preserve(cr);
	if (look == LOOK_ACTIVE)
		paint_accent(cr, 0.9);
	else if (look == LOOK_BAD)
		cairo_set_source_rgba(cr, card_col_red[0], card_col_red[1],
				card_col_red[2], 0.8);
	else
		cairo_set_source_rgba(cr, 1, 1, 1, idle_alpha[look]);
	cairo_set_line_width(cr, FIELD_LINE);
	cairo_stroke(cr);
}

void
paint_text(CardInk *ink, const CardText *t, Box b)
{
	CardText c = *t;

	c.x = b.x + (b.w - card_font_width(t->font, t->s)) / 2;
	c.y = b.y + (b.h - card_font_height(t->font)) / 2;
	card_ink_text(ink, &c);
}

/* Nerd glyph centered in b. */
void
paint_glyph(cairo_t *cr, Box b, Glyph g)
{
	const struct fcft_glyph *fg = bar_glyph_px(g.cp, b.w);
	cairo_surface_t *mask;

	if (!fg || pixman_image_get_format(fg->pix) != PIXMAN_a8)
		return;
	mask = cairo_image_surface_create_for_data(
			(unsigned char *)pixman_image_get_data(fg->pix),
			CAIRO_FORMAT_A8, fg->width, fg->height,
			pixman_image_get_stride(fg->pix));
	paint_rgb(cr, g.rgb, 1);
	cairo_mask_surface(cr, mask, b.x + (b.w - fg->width) / 2,
			b.y + (b.h - fg->height) / 2);
	cairo_surface_destroy(mask);
}

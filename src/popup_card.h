/* Shared statusbar popup "card" renderer.
 *
 * Builds one ARGB buffer per popup (rounded translucent card, border,
 * header with icon + big value, gauge bar, key/value grid, sections,
 * buttons, mini calendar) with cairo + fcft, and presents it as a
 * single wlr_scene_buffer.  Show animation (fade + slide + gauge
 * sweep) runs on a timer that only exists while a popup is animating,
 * so idle cost is zero.
 */
#ifndef POPUP_CARD_H
#define POPUP_CARD_H

#include <stdint.h>
#include <wlr/types/wlr_scene.h>

#define CARD_MAX_ROWS  40
#define CARD_MAX_HITS  16
#define CARD_MAX_FILLS 6
#define CARD_MAX_BTN   4

/* Accent palette (RGBA, non-premultiplied float) */
extern const float card_col_fg[4];       /* primary text */
extern const float card_col_dim[4];      /* labels / secondary */
extern const float card_col_faint[4];    /* section caps labels */
extern const float card_col_green[4];
extern const float card_col_yellow[4];
extern const float card_col_red[4];
extern const float card_col_blue[4];

typedef struct CardHit {
	int x, y, w, h;
	int id;
} CardHit;

typedef struct CardFill {
	int x, y, w, h;      /* full-size fill area inside the card */
	int full_w;          /* track width (w = current fill of it) */
	float color[4];
} CardFill;

typedef struct Card Card;

Card *card_begin(void);
/* Header: icon (svg asset path, may be NULL), bold title, small caps
 * subtitle under it, big right-aligned value. */
void card_header(Card *c, const char *icon_path, const char *title,
		const char *sub, const char *value);
/* Slim rounded gauge; fill is emitted as a CardFill so the presenter
 * can sweep-animate it. frac clamped to [0,1]. */
void card_gauge(Card *c, double frac, const float accent[4]);
/* Gauge that doubles as a slider: the track is recorded as a hit rect
 * with hit_id, and the fill can later be moved smoothly with
 * popup_view_set_fill_frac(). */
void card_gauge_id(Card *c, double frac, const float accent[4], int hit_id);
/* Two-column key/value row; pass NULL k2 for a single pair. Value
 * colors may be NULL (defaults to fg). */
void card_kv2(Card *c, const char *k1, const char *v1, const float *v1col,
		const char *k2, const char *v2, const float *v2col);
/* Separator line + small caps section label (label may be NULL for a
 * bare separator). */
void card_section(Card *c, const char *label);
/* Plain row: left text + optional right-aligned text. When hit_id >= 0
 * a "Kill"-style button is drawn at the right edge and recorded as a
 * hit rect with that id (hot = hovered). */
void card_text(Card *c, const char *left, const char *right,
		const float *rightcol);
void card_text_btn(Card *c, const char *left, const char *right,
		const float *rightcol, const char *btn_label, int hit_id, int hot);
/* Row of equal buttons; active index gets filled style, hovered index
 * a lighter fill. Hits recorded as id_base + index. */
void card_buttons(Card *c, const char *labels[], const char *icons[],
		int n, int active, int hover, int id_base);
void card_gap(Card *c, int px);
/* Mini month calendar (Mon-first) highlighting mday. */
void card_calendar(Card *c, int year, int mon, int mday);

typedef struct CardResult {
	struct wlr_buffer *buf;
	int w, h;
	CardHit hits[CARD_MAX_HITS];
	int nhits;
	CardFill fills[CARD_MAX_FILLS];
	int nfills;
} CardResult;

/* Rasterize and free the card. Returns 0 on success; result owns buf
 * (drop with wlr_buffer_drop after handing to the scene). */
int card_finish(Card *c, CardResult *out);

/* ── presenter ───────────────────────────────────────────────────── */

typedef struct PopupView {
	struct wlr_scene_tree *content;      /* child of the popup tree */
	struct wlr_scene_buffer *card;
	struct wlr_scene_buffer *fills[CARD_MAX_FILLS];
	int fill_w[CARD_MAX_FILLS];      /* target (resting) fill width */
	int fill_h[CARD_MAX_FILLS];
	int fill_full_w[CARD_MAX_FILLS]; /* track width */
	int fill_disp_w[CARD_MAX_FILLS]; /* currently displayed width */
	int nfills;
	int w, h;
	uint64_t anim_start_ms;
	int animating;
} PopupView;

/* Render a finished card into the popup tree (replaces previous card
 * content). Does not touch animation state. */
void popup_view_apply(PopupView *v, struct wlr_scene_tree *tree,
		CardResult *res);
/* Start the show animation (call on visibility 0 -> 1). */
void popup_view_show(PopupView *v);
/* Stop animating + reset (call when the popup is hidden). */
void popup_view_hide(PopupView *v);
/* Ease fill i to `frac` of its track width (slider feedback). */
void popup_view_set_fill_frac(PopupView *v, int i, double frac);

/* Measure helpers for callers that need text widths in card fonts. */
int card_text_width(const char *s);

#endif

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

#define CARD_MAX_ROWS  56
#define CARD_MAX_HITS  48
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
/* Sine-wave signal meter: amplitude and wave density scale with frac
 * (clamped to [0,1]). Static raster, no fill sweep. */
void card_wave(Card *c, double frac, const float accent[4]);
/* Centered spinner arc + label row. phase [0,1) rotates the arc; pass
 * a time-derived phase so periodic re-renders animate it. */
void card_loading(Card *c, const char *label, double phase);
/* Reserved live-meter row: card draws a faint midline and reports the
 * rect in CardResult.meter_*; the popup overlays card_meter_buffer()
 * frames there. */
void card_meter(Card *c);
/* Two-column key/value row; pass NULL k2 for a single pair. Value
 * colors may be NULL (defaults to fg). */
void card_kv2(Card *c, const char *k1, const char *v1, const float *v1col,
		const char *k2, const char *v2, const float *v2col);
/* kv2 whose second value is a clickable chip: recorded as a hit rect
 * with hit_id, drawn brighter while hot (hovered). */
void card_kv2_btn(Card *c, const char *k1, const char *v1, const float *v1col,
		const char *k2, const char *v2, const float *v2col,
		int hit_id, int hot);
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
/* Strip of proportional display boxes (monitor arrangement).  wr/hr are
 * the displays' relative logical sizes; boxes are drawn left-to-right,
 * `sel` highlighted, and `drag_idx` (>=0) lifted and shifted `drag_dx`
 * px while dragging.  Hits (id_base + index) are recorded at the slot
 * positions, never at the dragged offset. */
#define CARD_DISP_MAX 8
typedef struct {
	char name[20];
	char sub[20];
	float wr, hr;
} CardDisp;
void card_displays(Card *c, const CardDisp *d, int n, int sel,
		int drag_idx, int drag_dx, int id_base);
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
	int meter_x, meter_y, meter_w, meter_h;   /* live-meter rect (w=0: none) */
} CardResult;

/* Rasterize and free the card. Returns 0 on success; result owns buf
 * (drop with wlr_buffer_drop after handing to the scene). */
int card_finish(Card *c, CardResult *out);

/* ── presenter ───────────────────────────────────────────────────── */

typedef struct PopupView {
	struct wlr_scene_tree *content;      /* child of the popup tree */
	struct wlr_scene_buffer *shadow;
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

/* Shared card chrome for popups that render themselves (tray menus):
 * rounded translucent panel with hairline border, hover pill,
 * checkbox/radio marks and a submenu chevron in the card style. */
struct wlr_buffer *card_panel_buffer(int w, int h);
/* Soft floating drop shadow sized for a w×h card; buffer extends
 * CARD_SHADOW_MARGIN px on every side, position at (-margin,-margin).
 * radius = the card's corner radius (<= 0 for the default card radius). */
#define CARD_SHADOW_MARGIN 24
struct wlr_buffer *card_shadow_buffer(int w, int h, double radius);
struct wlr_buffer *card_meter_buffer(int w, int h, const float accent[4],
		const float *hist, int nhist, int head, double phase);
struct wlr_buffer *card_hover_buffer(int w, int h);
struct wlr_buffer *card_mark_buffer(int radio, int size, int state);
struct wlr_buffer *card_chevron_buffer(int size);

#endif

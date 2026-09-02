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
#define CARD_MAX_BTN   6

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
/* Reserved spectrum-analyzer row: card draws a faint baseline and
 * reports the rect in CardResult.wave_*; the popup overlays
 * card_spectrum_buffer() frames there. frac (clamped to [0,1]) is kept
 * for callers that pass it straight through to the buffer. */
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
/* text_btn with the button pinned to the right card edge (right text
 * sits left of it); hit rect is the button only — Kill rows. */
void card_text_rbtn(Card *c, const char *left, const char *right,
		const float *rightcol, const char *btn_label, int hit_id, int hot);
/* Row with two buttons pinned to the right card edge ([btn1][btn2],
 * btn2 outermost); each button is its own hit rect. */
void card_text_btn2(Card *c, const char *left,
		const char *btn1, int hit_id1, int hot1,
		const char *btn2, int hit_id2, int hot2);
/* Full-width accent-tinted button row (Radio ON/OFF); brighter while
 * hot. */
void card_big_btn(Card *c, const char *label, const float accent[4],
		int hit_id, int hot);
/* QR code row: size×size module matrix (row-major, bit0 = dark) drawn
 * on a white plate with quiet zone, centered. Matrix is caller-owned
 * and only read during card_finish. */
void card_qr(Card *c, const uint8_t *modules, int size);
/* text_btn with an svg icon (asset path, drawn at text height) left of
 * the label — device lists. icon_path may be NULL. */
void card_icon_text_btn(Card *c, const char *icon_path, const char *left,
		const char *right, const float *rightcol,
		const char *btn_label, int hit_id, int hot);
/* icon_text_btn with the button pinned to the right card edge, so
 * buttons across rows stack in one column; right text sits left of it */
void card_icon_text_rbtn(Card *c, const char *icon_path, const char *left,
		const char *right, const float *rightcol,
		const char *btn_label, int hit_id, int hot);
/* Full-row-clickable row (BT-device hover style, but no button): icon
 * + left text, right text, optional status icon (svg asset path) at
 * the right card edge; the whole row is the hit rect and washes on
 * hover. icon_path/right/sicon may be NULL. */
void card_icon_text_hit(Card *c, const char *icon_path, const char *left,
		const char *right, const float *rightcol, const char *sicon,
		int hit_id, int hot);
/* icon_text_rbtn with up to two small status icons (svg asset paths,
 * drawn at text height, right-aligned left of the button) in place of
 * the right text — BT device signal/battery. Either may be NULL. */
void card_icon_text_rbtn_icons(Card *c, const char *icon_path,
		const char *left, const char *sicon1, const char *sicon2,
		const char *btn_label, int hit_id, int hot);
/* Row of equal buttons; active index gets filled style, hovered index
 * a lighter fill. Hits recorded as id_base + index. */
void card_buttons(Card *c, const char *labels[], const char *icons[],
		int n, int active, int hover, int id_base);
/* Same, but buttons whose bit is set in red_mask are drawn in red
 * (overclock / danger entries). */
void card_buttons_mask(Card *c, const char *labels[],
		int n, int active, int hover, int id_base, int red_mask);
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
/* Override the card's minimum content width (default CARD_MIN_W) —
 * for compact menus that should hug their rows. */
void card_min_w(Card *c, int w);
/* Where the card will be shown (layout coords, monitor it belongs to):
 * the backdrop only darkens when the card overlaps a visible client
 * there — over bare wallpaper it stays translucent.  Consumed by the
 * next card_finish; without it any client visible on selmon darkens. */
struct Monitor;
void card_at(struct Monitor *m, int x, int y);
/* Icon + label row with a full-width hover pill (compact menus).
 * Icon is an svg asset path drawn at text height. */
void card_icon_text(Card *c, const char *icon_path, const char *label,
		const float *labelcol, int hit_id, int hot);
/* Editable temp→speed step curve (fan popup): n points (temp °C, pct),
 * `sel` highlighted.  One hit rect covering exactly the plot area is
 * recorded with hit_id; callers map cursor→(temp, pct) linearly with
 * CARD_CURVE_TMIN/TMAX on x and 100→0 top→bottom on y. */
#define CARD_CURVE_PTS_MAX 8
#define CARD_CURVE_TMIN 20
#define CARD_CURVE_TMAX 100
void card_curve(Card *c, const uint8_t *temps, const uint8_t *pcts, int n,
		int sel, const float accent[4], int hit_id);

typedef struct CardResult {
	struct wlr_buffer *buf;
	int w, h;
	CardHit hits[CARD_MAX_HITS];
	int nhits;
	CardFill fills[CARD_MAX_FILLS];
	int nfills;
	int meter_x, meter_y, meter_w, meter_h;   /* live-meter rect (w=0: none) */
	int wave_x, wave_y, wave_w, wave_h;       /* spectrum rect (w=0: none) */
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
/* Move fill i to `frac` immediately — pointer drags, where the fill
 * must track the cursor 1:1 (the eased variant trails ~4 frames behind
 * and feels rubbery under the finger). */
void popup_view_drag_fill_frac(PopupView *v, int i, double frac);

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
/* Persistent ping-pong raster pair for the per-frame meter overlay:
 * card_meter_raster() draws directly into one of two pre-allocated
 * CPU buffers (no per-frame surface alloc + memcpy) and returns the
 * one just drawn — set it on the scene node, do NOT drop it.  The
 * buffers alternate because the scene may still reference the
 * previous frame's.  Free with card_meter_raster_finish() when the
 * meter goes away (also called internally on a size change). */
typedef struct {
	struct wlr_buffer *buf[2];
	int next;
	int w, h;
} MeterRaster;
struct wlr_buffer *card_meter_raster(MeterRaster *mr, int w, int h,
		const float accent[4], const float *hist, int nhist, int head,
		double phase);
void card_meter_raster_finish(MeterRaster *mr);
/* Spectrum-analyzer frame for the card_wave rect: bottom-aligned bars
 * whose envelope scales with frac, animated by t (seconds). */
struct wlr_buffer *card_spectrum_buffer(int w, int h, const float accent[4],
		double frac, double t);
struct wlr_buffer *card_hover_buffer(int w, int h);
struct wlr_buffer *card_mark_buffer(int radio, int size, int state);
struct wlr_buffer *card_chevron_buffer(int size);

#endif

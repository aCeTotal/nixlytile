/* State shared by overview files. */
#ifndef OVERVIEW_STATE_H
#define OVERVIEW_STATE_H

#include "nixlytile.h"
#include "client.h"
#include "popup_card.h"
#include "overview/overview.h"

#define OV_RADIUS   14
#define OV_LANE_PAD 18
#define OV_BADGE    38

typedef enum { DETAIL_NONE, DETAIL_PATH, DETAIL_REMOTE } DetailKind;

typedef struct {
	double pos, vel, target;
} Motion;

typedef struct {
	const float *rgba;
	float alpha;
} Paint;

typedef struct {
	Client *c;
	struct nx_box live; /* real window, zoom origin */
	struct nx_box from; /* grid box before reflow */
	struct nx_box slot; /* grid box */
	Motion lift;

	char title[256];
	char detail[PATH_MAX];
	DetailKind kind;

	struct nx_scene_tree *tree;
	struct nx_scene_rect *shadow;
	Snapshot *thumb;
	struct nx_scene_rect *ghost;
	struct nx_scene_rect *chip;
	struct nx_scene_buffer *label;
	int label_w, label_h, label_cap;
} Cell;

typedef struct {
	uint64_t ws_id;
	Workspace *ws; /* valid until the next move */
	int fresh;
	struct nx_box from, box;

	struct nx_scene_rect *lane;
	struct nx_scene_rect *badge;
	struct nx_scene_buffer *digits;
	int digits_w, digits_h;
} Row;

struct Overview {
	Monitor *mon;
	int closing;
	Client *activate;

	Cell *cells;
	int n_cells, sel;
	Row *rows;
	int n_rows;

	Motion zoom;  /* 0 real layout, 1 grid */
	Motion flow;  /* reflow after a move */
	Motion focus; /* selection glide */
	struct nx_box ring_from;

	struct nx_scene_tree *tree;
	struct nx_scene_buffer *backdrop;
	struct nx_scene_rect *tint;
	struct nx_scene_tree *chrome;
	struct nx_scene_rect *glow;
	struct nx_scene_tree *tiles;
	struct nx_scene_rect *ring;
};

extern struct Overview ov;

/* overview.c */
void overview_redraw(void);
void overview_select(int i);
void overview_reflow(void);
int overview_revalidate(void);
void overview_cancel(void);
void overview_choose(void);

/* grid.c */
int grid_collect(Monitor *m);
struct nx_box grid_screen_box(Client *c, Workspace *ref);
struct nx_box box_lerp(struct nx_box a, struct nx_box b, double t);

/* cell.c */
void cell_build(Cell *cell);
void cell_destroy(Cell *cell);
void cell_refit(Cell *cell);
void cell_relabel(Cell *cell);
void cell_apply(Cell *cell);
struct nx_box cell_box(const Cell *cell);

/* label.c */
void label_describe(Cell *cell);
void label_render(Cell *cell, int cap);

/* detail.c */
void detail_fetch(void);
void detail_drop(void);

/* chrome.c */
struct nx_scene_rect *shaped_rect(struct nx_scene_tree *parent,
		const struct nx_scene_rect_shape *shape);
void place_rect(struct nx_scene_rect *r, struct nx_box b, Paint paint);
void chrome_build(void);
void chrome_rows(void);
void chrome_drop_row(Row *row);
void chrome_apply(void);
struct nx_box selection_box(void);

/* backdrop.c */
struct nx_scene_buffer *backdrop_attach(struct nx_scene_tree *parent, Monitor *m);

/* keys.c */
void select_toward(int dx, int dy);

#endif

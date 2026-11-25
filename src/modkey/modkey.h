#ifndef MODKEY_MODKEY_H
#define MODKEY_MODKEY_H

#include "nixlytile.h"

#define PENDING_MAX 16
#define MODKEY_BOUNCE_MS 200   /* Super usually returns by then */
#define MODKEY_SKEW_MS 30      /* EC reorders edges per scan */
#define MODKEY_DROPOUT_MS 4000 /* longest measured: 3.3 s */
#define MODKEY_STREAM_MS 8     /* edges in the same scan */

/* EC-reported Super state. */
struct SuperState {
	uint32_t up_ms;
	uint32_t down_ms;
	uint32_t typed_ms; /* last text, hops included */
	uint32_t last_ms;
	void (*last)(const Arg *); /* last Super bind */
	uint32_t gone_mods; /* modifiers just released */
	uint32_t gone_ms;
	int keyed;    /* key pressed since Super */
	int fresh;    /* press began a hold */
	int blipped;  /* hold began keyless and short */
	int bound;    /* Super bind this hold */
	int anchored; /* Super bind this dropout */
	int texted;   /* text this dropout, hops aside */
	int missed;
};

/* Key events waiting on Super. */
struct Pending {
	struct nx_keyboard_key_event ev[PENDING_MAX];
	uint32_t mods[PENDING_MAX];
	xkb_keysym_t sym[PENDING_MAX];   /* bind with Super */
	xkb_keysym_t plain[PENDING_MAX]; /* bind without Super */
	uint32_t late[PENDING_MAX];      /* modifiers right after */
	uint32_t released[PENDING_MAX];  /* modifiers released with Super */
	struct nx_keyboard_modifiers seen[PENDING_MAX];
	int n;
	int lead;
	int latched;       /* binds at the deadline */
	uint32_t until_ms; /* event time */
	unsigned seq;
	struct wl_event_source *timer;
};

extern struct SuperState super;
extern struct Pending pending;

/* chord.c */
uint32_t key_mod(uint32_t keycode);
int key_down(struct nx_keyboard *kb, uint32_t keycode);
/* Bind this key would fire. */
const Key *chord_binding(struct nx_keyboard *kb, uint32_t mods,
		uint32_t keycode);
/* Some Super chord binds this key. */
int chord_reachable(struct nx_keyboard *kb, uint32_t mods, uint32_t keycode);
int binding_hops(const Key *k);

/* latch.c */
void super_pressed(uint32_t time_msec);
void super_released(uint32_t time_msec);
void super_ran(const Key *k, uint32_t time_msec);
int super_hop(const Key *k);
int super_tap(struct nx_keyboard *kb, uint32_t mods, uint32_t keycode);
int super_latched(struct nx_keyboard *kb,
		const struct nx_keyboard_key_event *event, uint32_t mods);

/* skew.c */
void super_gone(uint32_t mod, uint32_t time_msec);
uint32_t super_gone_with(void);

/* queue.c */
int pending_presses(uint32_t keycode);
void pending_push(const struct nx_keyboard_key_event *event, uint32_t mods,
		uint32_t released);
void pending_type(void);
void pending_bind(KeyboardGroup *group);
void pending_rebase(struct nx_keyboard *kb);

#endif

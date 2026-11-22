#ifndef MODKEY_MODKEY_H
#define MODKEY_MODKEY_H

#include "nixlytile.h"

#define PENDING_MAX 16

/* EC-reported Super state. */
struct SuperState {
	uint32_t up_ms;
	uint32_t typed_ms;
	uint32_t latch_ms; /* last bind under a dropout */
	void (*last)(const Arg *); /* last Super bind */
	uint32_t gone_mods; /* modifiers just released */
	uint32_t gone_ms;
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

/* dropout.c */
void super_ran(const Key *k);

/* queue.c */
int pending_presses(uint32_t keycode);
void pending_push(const struct nx_keyboard_key_event *event, uint32_t mods,
		uint32_t released);
void pending_type(void);
void pending_bind(KeyboardGroup *group);
void pending_rebase(struct nx_keyboard *kb);

#endif

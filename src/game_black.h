#ifndef GAME_BLACK_H
#define GAME_BLACK_H

/* Verdicts for the wrapper's black-screen probe. */
enum {
	GB_NONE,      /* no game client mapped */
	GB_NOBUFFER,  /* game window exists, nothing committed yet */
	GB_BLACK,     /* committing frames, all sampled pixels black */
	GB_CONTENT,   /* rendering real content */
};

/* Sample the current game surface; *out_appid gets its app id (may be
 * NULL) when a client was found. */
int game_black_state(const char **out_appid);

#endif

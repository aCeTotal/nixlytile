#ifndef JAILD_H
#define JAILD_H

#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/types.h>

#define PICKS_MAX 64

typedef struct {
	const char *path;
	int tree;
	int dir;
} Pick;

typedef struct {
	struct ucred peer;
	pid_t target;
	Pick picks[PICKS_MAX];
	int n;
} Grant;

/* jaild_attach.c */
const char *jail_inject(const Grant *g);

#endif

/* Shared word read by the nixly-gate LADSPA plugin. */

#include "nixlytile.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define GATE_FILE "nixly-gate"

static uint32_t *gate_word;

void
mic_gate_setup(void)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	char path[PATH_MAX];
	struct stat st;
	void *map;
	int fd;

	if (dir)
		snprintf(path, sizeof(path), "%s/" GATE_FILE, dir);
	else
		snprintf(path, sizeof(path), "/run/user/%u/" GATE_FILE, getuid());

	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "mic gate: open %s", path);
		return;
	}
	if (fstat(fd, &st) < 0 ||
			(st.st_size < (off_t)sizeof(*gate_word) &&
			 ftruncate(fd, sizeof(*gate_word)) < 0)) {
		wlr_log_errno(WLR_ERROR, "mic gate: size %s", path);
		close(fd);
		return;
	}
	map = mmap(NULL, sizeof(*gate_word), PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		wlr_log_errno(WLR_ERROR, "mic gate: mmap %s", path);
		return;
	}
	gate_word = map;
	__atomic_store_n(gate_word, 0, __ATOMIC_RELEASE);
}

void
mic_gate_set(uint32_t closed)
{
	uint32_t old;

	if (!gate_word)
		return;
	old = __atomic_exchange_n(gate_word, closed, __ATOMIC_RELEASE);
	if ((old ^ closed) & MIC_GATE_TALK)
		refreshstatusmic();
}

int
mic_gate_talk_closed(void)
{
	return gate_word &&
		(__atomic_load_n(gate_word, __ATOMIC_ACQUIRE) & MIC_GATE_TALK);
}

void
mic_gate_cleanup(void)
{
	if (!gate_word)
		return;
	__atomic_store_n(gate_word, 0, __ATOMIC_RELEASE);
	munmap(gate_word, sizeof(*gate_word));
	gate_word = NULL;
}

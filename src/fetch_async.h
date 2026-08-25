#ifndef NIXLYTILE_FETCH_ASYNC_H
#define NIXLYTILE_FETCH_ASYNC_H

#include <stddef.h>

/*
 * Run a shell command in the background and hand its complete stdout to
 * `done` on the compositor event loop once the command exits. The
 * non-blocking replacement for popen() in paths that run while the UI
 * must stay responsive (statusbar popup refreshes).
 *
 * `out` is NUL-terminated and only valid during the callback. Returns 0
 * when the fetch was started, -1 otherwise (no free slot / spawn failed)
 * — on -1 the callback is never invoked.
 */
typedef void (*fetch_done_fn)(const char *out, size_t len, void *data);
int fetch_async(const char *cmd, fetch_done_fn done, void *data);

#endif /* NIXLYTILE_FETCH_ASYNC_H */

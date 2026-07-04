/*
 * diag.h — always-on diagnostic logger.
 *
 * Writes detailed render / fullscreen / tile diagnostics to
 * /tmp/nixlytile-diag.log (truncated fresh each session).  Purpose: answer
 * "why did the tile / fullscreen freeze or not work" without needing
 * WLR_DEBUG — a per-monitor render-state heartbeat, a freeze detector that
 * names the likely cause, and discrete transition events (fullscreen
 * enter/exit, tile freeze/unfreeze, commit/scene-build failures).
 */
#ifndef DIAG_H
#define DIAG_H

/* Open /tmp/nixlytile-diag.log. Call once from setup(). No-op-safe: if the
 * file can't be opened, every diag_logf() silently does nothing. */
void diag_init(void);

/* Append one timestamped line. `cat` is a short category tag (e.g. "MON",
 * "FREEZE", "FS", "TILE", "COMMITFAIL"). Format-checked like printf. */
void diag_logf(const char *cat, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#endif /* DIAG_H */

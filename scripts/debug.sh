#!/usr/bin/env bash
# Debug-run nixlytile and collect all logs + crash/system info into one
# timestamped report dir. Any extra args are forwarded to the compositor
# (e.g. -s "alacritty"). Quit the compositor normally to end the session.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/nixlytile"
LOGDIR="/tmp/nixlytile"                       # compositor's own log dir (hardcoded in src)
STAMP="$(date +%Y%m%d-%H%M%S)"
REPORT="$LOGDIR/debug-$STAMP"
SESSLOG="$REPORT/session.log"

[ -x "$BIN" ] || { echo "no binary: $BIN — run 'make' first" >&2; exit 1; }
mkdir -p "$REPORT"

# --- pick backend: nested if a display exists, else real DRM/TTY ---
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
	export WLR_BACKENDS=wayland; MODE="nested-wayland"
elif [ -n "${DISPLAY:-}" ]; then
	export WLR_BACKENDS=x11; MODE="nested-x11"
else
	MODE="drm-tty"
fi

# --- max verbosity + crash capture ---
export WLR_DEBUG=1                             # wlroots verbose logging
export NIXLY_DEBUG=1                           # enable nixlytile's own log files (wlroots.log/WLR_DEBUG.log)
export WLR_RENDERER=gles2                      # raw binary links gles2-only wlroots; ignore any global vulkan sessionVar
ulimit -c unlimited                            # allow core dumps
START_TS="$(date '+%Y-%m-%d %H:%M:%S')"        # for coredumpctl --since

echo "== nixlytile debug =="
echo "mode:    $MODE"
echo "report:  $REPORT"
echo "binary:  $BIN ($(stat -c %y "$BIN" | cut -d. -f1))"
echo "args:    ${*:-<none, using compiled autostart>}"
echo "start:   $START_TS"
echo "-> launching (quit compositor to finish)..."
echo

# Run with -d (WLR_DEBUG log level). Tee combined stdout+stderr to session log.
"$BIN" -d "$@" > >(tee "$SESSLOG") 2>&1
EXIT=$?
END_TS="$(date '+%Y-%m-%d %H:%M:%S')"

# --- snapshot the compositor's own logs ---
for f in wlroots WLR_DEBUG xwayland diagnostics audio errors game_debug; do
	[ -f "$LOGDIR/$f.log" ] && cp "$LOGDIR/$f.log" "$REPORT/$f.log"
done

# --- system / gpu / stack info ---
{
	echo "=== nixlytile debug report $STAMP ==="
	echo "mode=$MODE  exit_code=$EXIT"
	echo "start=$START_TS  end=$END_TS"
	echo; echo "--- uname ---"; uname -a
	echo; echo "--- gpu (drm) ---"; lspci -nnk 2>/dev/null | grep -iA3 'vga\|3d\|display'
	echo; echo "--- versions ---"
	for p in wlroots-0.19 wlroots wayland-server pixman-1; do
		v=$(pkg-config --modversion "$p" 2>/dev/null) && echo "$p = $v"
	done
	command -v glxinfo >/dev/null && { echo; echo "--- mesa ---"; glxinfo -B 2>/dev/null | grep -i 'opengl\|device\|vendor'; }
	echo; echo "--- env ---"
	echo "WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-} DISPLAY=${DISPLAY:-} XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-}"
	echo "XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-} SEATD/libseat=$(command -v seatd || echo n/a)"
} > "$REPORT/sysinfo.txt" 2>&1

# --- crash detection ---
CRASHED=0
if [ "$EXIT" -ge 128 ]; then
	CRASHED=1
	echo "signal $((EXIT-128)) (exit $EXIT)" > "$REPORT/crash.txt"
	# pull matching core dump metadata + stack (systemd-coredump)
	coredumpctl --since "$START_TS" info nixlytile >> "$REPORT/crash.txt" 2>&1
fi
grep -qiE 'segfault|assertion|backtrace|SIGSEGV|SIGABRT' "$SESSLOG" "$REPORT"/*.log 2>/dev/null && CRASHED=1

# --- summary ---
echo
echo "== summary =="
echo "exit code: $EXIT $( [ "$CRASHED" = 1 ] && echo '(CRASH DETECTED)')"
echo "duration:  $START_TS -> $END_TS"
if [ -s "$REPORT/errors.log" ]; then
	echo "-- errors.log (last 15) --"; tail -n 15 "$REPORT/errors.log"
fi
HITS="$(grep -rniE 'error|fail|segfault|assert|backtrace|critical' "$SESSLOG" 2>/dev/null | grep -viE 'no error|0 error' | tail -n 20)"
[ -n "$HITS" ] && { echo "-- session.log flagged lines (last 20) --"; echo "$HITS"; }
echo
echo "full report: $REPORT"
ls -1 "$REPORT"
exit $EXIT

/* config_loader.h — loads ~/.config/nixlytile/config.kdl into runtime globals. */
#ifndef NIXLYTILE_CONFIG_LOADER_H
#define NIXLYTILE_CONFIG_LOADER_H

#include <stddef.h>

/* Returns 1 if a config file was found and parsed; 0 if not present or
 * parse error (defaults remain). Path used is logged. Safe to call before
 * setup() — does NOT touch wlroots state. */
int load_config(void);

/* Reload config from disk and hot-apply to running state (xkb keymap,
 * libinput devices, monitor scale/mode, colors, keybindings, autostart
 * diff). Returns 1 on success.  Safe to call from the wayland event
 * loop signal handler. */
int reload_config(void);

/* Internal: location of the config file (set after first load). */
extern char nixlytile_config_path[];

#endif

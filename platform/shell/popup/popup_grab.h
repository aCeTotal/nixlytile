#ifndef PLATFORM_SHELL_POPUP_POPUP_GRAB_H
#define PLATFORM_SHELL_POPUP_POPUP_GRAB_H

#include "platform/shell/xdg/xdg_shell.h"

// Creates the grab if missing.
struct nx_xdg_popup_grab *shell_popup_grab_from_seat(
	struct nx_xdg_shell *shell, struct nx_seat *seat);
void shell_popup_grab_add(struct nx_xdg_popup_grab *grab,
	struct nx_xdg_popup *popup);
// Ends grab after last popup.
void shell_popup_grab_remove(struct nx_xdg_popup *popup);

#endif

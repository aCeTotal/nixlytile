#ifndef XWAYLAND_SERVER_SOCKETS_H
#define XWAYLAND_SERVER_SOCKETS_H

// Returns the display number, or -1.
int xwayland_open_display_sockets(int socks[2]);
void xwayland_unlink_display_sockets(int display);

#endif

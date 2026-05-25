.POSIX:
.SUFFIXES:

include config.mk

SRC = src

# flags for compiling
CPPFLAGS_EXTRA = -I$(SRC) -I. -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L \
	-DVERSION=\"$(VERSION)\" $(XWAYLAND)
DEVCFLAGS = -g -Wpedantic -Wall -Wextra -Wdeclaration-after-statement \
	-Wno-unused-parameter -Wshadow -Wunused-macros -Werror=strict-prototypes \
	-Werror=implicit -Werror=return-type -Werror=incompatible-pointer-types \
	-Wfloat-conversion

# CFLAGS / LDFLAGS
PKGS      = wayland-server xkbcommon libinput libdrm $(XLIBS) fcft pixman-1 libsystemd
WP_INCS = -I$(shell $(PKG_CONFIG) --variable=prefix wayland-protocols 2>/dev/null)/include
NLCFLAGS = `$(PKG_CONFIG) --cflags $(PKGS)` $(WLR_INCS) $(WP_INCS) $(CPPFLAGS_EXTRA) $(DEVCFLAGS) $(CFLAGS)
LDLIBS    = `$(PKG_CONFIG) --libs $(PKGS)` $(WLR_LIBS) -lm -lpthread $(LIBS)

# Allow C99 style declarations in all modules
MOD_CFLAGS = $(NLCFLAGS) -Wno-declaration-after-statement

# Compositor module object files
MOD_OBJS = globals.o client.o layout.o input.o output.o \
           gamemode.o client_utils.o gpu.o draw.o layer.o workspace.o anim.o \
           dwl_ipc.o dwl-ipc-unstable-v2-protocol.o window_ipc.o \
           config_parser.o config_loader.o apptoggle.o

PROTO_HDRS = $(SRC)/cursor-shape-v1-protocol.h $(SRC)/pointer-constraints-unstable-v1-protocol.h \
             $(SRC)/wlr-layer-shell-unstable-v1-protocol.h $(SRC)/wlr-output-power-management-unstable-v1-protocol.h \
             $(SRC)/xdg-shell-protocol.h $(SRC)/content-type-v1-protocol.h $(SRC)/tearing-control-v1-protocol.h \
             $(SRC)/tablet-v2-protocol.h $(SRC)/dwl-ipc-unstable-v2-protocol.h

all: nixlytile
nixlytile: nixlytile.o util.o $(MOD_OBJS)
	$(CC) nixlytile.o util.o $(MOD_OBJS) $(MOD_CFLAGS) $(LDFLAGS) $(LDLIBS) -o $@

# Core compositor
nixlytile.o: $(SRC)/nixlytile.c $(SRC)/nixlytile.h $(SRC)/client.h config.mk $(PROTO_HDRS)
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
util.o: $(SRC)/util.c $(SRC)/util.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<

# Compositor modules
globals.o: $(SRC)/globals.c $(SRC)/nixlytile.h $(SRC)/client.h $(SRC)/config.h config.mk $(PROTO_HDRS)
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
client.o: $(SRC)/client.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
layout.o: $(SRC)/layout.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
input.o: $(SRC)/input.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
output.o: $(SRC)/output.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
gamemode.o: $(SRC)/gamemode.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
client_utils.o: $(SRC)/client_utils.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
gpu.o: $(SRC)/gpu.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
draw.o: $(SRC)/draw.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
layer.o: $(SRC)/layer.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
workspace.o: $(SRC)/workspace.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
anim.o: $(SRC)/anim.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
dwl_ipc.o: $(SRC)/dwl_ipc.c $(SRC)/nixlytile.h $(SRC)/dwl-ipc-unstable-v2-protocol.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
window_ipc.o: $(SRC)/window_ipc.c $(SRC)/nixlytile.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
config_parser.o: $(SRC)/config_parser.c $(SRC)/config_parser.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
config_loader.o: $(SRC)/config_loader.c $(SRC)/config_loader.h $(SRC)/config_parser.h $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
apptoggle.o: $(SRC)/apptoggle.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
dwl-ipc-unstable-v2-protocol.o: $(SRC)/dwl-ipc-unstable-v2-protocol.c
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
# wayland-scanner generated protocol headers (output into src/)
WAYLAND_SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`
WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`

$(SRC)/cursor-shape-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml $@
$(SRC)/pointer-constraints-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@
$(SRC)/wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@
$(SRC)/wlr-output-power-management-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		protocols/wlr-output-power-management-unstable-v1.xml $@
$(SRC)/xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
$(SRC)/content-type-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/staging/content-type/content-type-v1.xml $@
$(SRC)/tearing-control-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/staging/tearing-control/tearing-control-v1.xml $@
$(SRC)/tablet-v2-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/tablet/tablet-v2.xml $@
$(SRC)/dwl-ipc-unstable-v2-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		protocols/dwl-ipc-unstable-v2.xml $@
$(SRC)/dwl-ipc-unstable-v2-protocol.c: $(SRC)/dwl-ipc-unstable-v2-protocol.h
	$(WAYLAND_SCANNER) private-code \
		protocols/dwl-ipc-unstable-v2.xml $@

$(SRC)/config.h:
	cp $(SRC)/config.def.h $@
clean:
	rm -f nixlytile *.o $(SRC)/*-protocol.h $(SRC)/*-protocol.c

dist: clean
	mkdir -p nixlytile-$(VERSION)
	cp -R Makefile config.mk protocols \
		nixlytile.1 nixlytile.desktop src \
		nixlytile-$(VERSION)
	tar -caf nixlytile-$(VERSION).tar.gz nixlytile-$(VERSION)
	rm -rf nixlytile-$(VERSION)

install: nixlytile
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	rm -f $(DESTDIR)$(PREFIX)/bin/nixlytile
	cp -f nixlytile $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/nixlytile
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp -f nixlytile.1 $(DESTDIR)$(MANDIR)/man1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/nixlytile.1
	mkdir -p $(DESTDIR)$(DATADIR)/wayland-sessions
	cp -f nixlytile.desktop $(DESTDIR)$(DATADIR)/wayland-sessions/nixlytile.desktop
	chmod 644 $(DESTDIR)$(DATADIR)/wayland-sessions/nixlytile.desktop
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nixlytile $(DESTDIR)$(MANDIR)/man1/nixlytile.1 \
		$(DESTDIR)$(DATADIR)/wayland-sessions/nixlytile.desktop

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CPPFLAGS) $(NLCFLAGS) -o $@ -c $<

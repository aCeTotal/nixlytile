.POSIX:
.SUFFIXES:

include config.mk

# flags for compiling
CPPFLAGS_EXTRA = -I. -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L \
	-DVERSION=\"$(VERSION)\" $(XWAYLAND)
DEVCFLAGS = -g -Wpedantic -Wall -Wextra -Wdeclaration-after-statement \
	-Wno-unused-parameter -Wshadow -Wunused-macros -Werror=strict-prototypes \
	-Werror=implicit -Werror=return-type -Werror=incompatible-pointer-types \
	-Wfloat-conversion

# CFLAGS / LDFLAGS
PKGS      = wayland-server xkbcommon libinput libdrm $(XLIBS) fcft pixman-1 libsystemd gdk-pixbuf-2.0 cairo librsvg-2.0
NLCFLAGS = `$(PKG_CONFIG) --cflags $(PKGS)` $(WLR_INCS) $(CPPFLAGS_EXTRA) $(DEVCFLAGS) $(CFLAGS)
LDLIBS    = `$(PKG_CONFIG) --libs $(PKGS)` $(WLR_LIBS) -lm $(LIBS)

all: nixlytile
nixlytile: nixlytile.o util.o
	$(CC) nixlytile.o util.o $(NLCFLAGS) $(LDFLAGS) $(LDLIBS) -o $@
nixlytile.o: nixlytile.c client.h config.h config.mk cursor-shape-v1-protocol.h \
	pointer-constraints-unstable-v1-protocol.h wlr-layer-shell-unstable-v1-protocol.h \
	wlr-output-power-management-unstable-v1-protocol.h xdg-shell-protocol.h \
	content-type-v1-protocol.h tearing-control-v1-protocol.h
util.o: util.c util.h

# wayland-scanner is a tool which generates C headers and rigging for Wayland
# protocols, which are specified in XML. wlroots requires you to rig these up
# to your build system yourself and provide them in the include path.
WAYLAND_SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`
WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`

cursor-shape-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml $@
pointer-constraints-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@
wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@
wlr-output-power-management-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		protocols/wlr-output-power-management-unstable-v1.xml $@
xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
content-type-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/staging/content-type/content-type-v1.xml $@
tearing-control-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/staging/tearing-control/tearing-control-v1.xml $@

config.h:
	cp config.def.h $@
clean:
	rm -f nixlytile *.o *-protocol.h

dist: clean
	mkdir -p nixlytile-$(VERSION)
	cp -R LICENSE* Makefile CHANGELOG.md README.md client.h config.def.h \
		config.mk protocols nixlytile.1 nixlytile.c util.c util.h nixlytile.desktop images \
		nixlytile-$(VERSION)
	tar -caf nixlytile-$(VERSION).tar.gz nixlytile-$(VERSION)
	rm -rf nixlytile-$(VERSION)

install: nixlytile
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	rm -f $(DESTDIR)$(PREFIX)/bin/nixlytile
	cp -f nixlytile $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/nixlytile
	mkdir -p $(DESTDIR)$(DATADIR)/nixlytile/images
	cp -r images/svg $(DESTDIR)$(DATADIR)/nixlytile/images/
	mkdir -p $(DESTDIR)$(DATADIR)/nixlytile
	cp -f config.conf.example $(DESTDIR)$(DATADIR)/nixlytile/config.conf.example
	chmod 644 $(DESTDIR)$(DATADIR)/nixlytile/config.conf.example
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

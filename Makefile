.POSIX:
.SUFFIXES:

include config.mk

SRC = src

# flags for compiling
CPPFLAGS_EXTRA = -I$(SRC) -I$(SRC)/videoplayer -I. -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L \
	-DVERSION=\"$(VERSION)\" $(XWAYLAND)
DEVCFLAGS = -g -Wpedantic -Wall -Wextra -Wdeclaration-after-statement \
	-Wno-unused-parameter -Wshadow -Wunused-macros -Werror=strict-prototypes \
	-Werror=implicit -Werror=return-type -Werror=incompatible-pointer-types \
	-Wfloat-conversion

# CFLAGS / LDFLAGS
PKGS      = wayland-server xkbcommon libinput libdrm $(XLIBS) fcft pixman-1 libsystemd gdk-pixbuf-2.0 cairo librsvg-2.0 \
            libavformat libavcodec libavutil libswscale libswresample libpipewire-0.3 libass
NLCFLAGS = `$(PKG_CONFIG) --cflags $(PKGS)` $(WLR_INCS) $(CPPFLAGS_EXTRA) $(DEVCFLAGS) $(CFLAGS)
LDLIBS    = `$(PKG_CONFIG) --libs $(PKGS)` $(WLR_LIBS) -lm -lpthread $(LIBS)

# Allow C99 style declarations in all modules
MOD_CFLAGS = $(NLCFLAGS) -Wno-declaration-after-statement

# Video player object files
VP_OBJS = videoplayer.o videoplayer_decode.o videoplayer_render.o videoplayer_audio.o videoplayer_ui.o

# Compositor module object files
MOD_OBJS = globals.o client.o layout.o btrtile.o input.o gamepad.o output.o \
           statusbar.o tray.o network.o launcher.o nixpkgs.o gaming.o media.o \
           popup.o bluetooth.o config.o htpc.o draw.o layer.o fancontrol.o

PROTO_HDRS = $(SRC)/cursor-shape-v1-protocol.h $(SRC)/pointer-constraints-unstable-v1-protocol.h \
             $(SRC)/wlr-layer-shell-unstable-v1-protocol.h $(SRC)/wlr-output-power-management-unstable-v1-protocol.h \
             $(SRC)/xdg-shell-protocol.h $(SRC)/content-type-v1-protocol.h $(SRC)/tearing-control-v1-protocol.h

all: nixlytile
nixlytile: nixlytile.o util.o $(MOD_OBJS) $(VP_OBJS)
	$(CC) nixlytile.o util.o $(MOD_OBJS) $(VP_OBJS) $(MOD_CFLAGS) $(LDFLAGS) $(LDLIBS) -o $@

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
btrtile.o: $(SRC)/btrtile.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
input.o: $(SRC)/input.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
gamepad.o: $(SRC)/gamepad.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
output.o: $(SRC)/output.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
statusbar.o: $(SRC)/statusbar.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
tray.o: $(SRC)/tray.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
network.o: $(SRC)/network.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
launcher.o: $(SRC)/launcher.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
nixpkgs.o: $(SRC)/nixpkgs.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
gaming.o: $(SRC)/gaming.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
media.o: $(SRC)/media.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
popup.o: $(SRC)/popup.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
bluetooth.o: $(SRC)/bluetooth.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
config.o: $(SRC)/config.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
htpc.o: $(SRC)/htpc.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
draw.o: $(SRC)/draw.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
layer.o: $(SRC)/layer.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<
fancontrol.o: $(SRC)/fancontrol.c $(SRC)/nixlytile.h $(SRC)/client.h
	$(CC) $(CPPFLAGS) $(MOD_CFLAGS) -o $@ -c $<

# Video player modules
VP_DIR = $(SRC)/videoplayer
VP_CFLAGS = $(NLCFLAGS) -Wno-declaration-after-statement

videoplayer.o: $(VP_DIR)/videoplayer.c $(VP_DIR)/videoplayer.h
	$(CC) $(CPPFLAGS) $(VP_CFLAGS) -o $@ -c $<
videoplayer_decode.o: $(VP_DIR)/videoplayer_decode.c $(VP_DIR)/videoplayer.h
	$(CC) $(CPPFLAGS) $(VP_CFLAGS) -o $@ -c $<
videoplayer_render.o: $(VP_DIR)/videoplayer_render.c $(VP_DIR)/videoplayer.h
	$(CC) $(CPPFLAGS) $(VP_CFLAGS) -o $@ -c $<
videoplayer_audio.o: $(VP_DIR)/videoplayer_audio.c $(VP_DIR)/videoplayer.h
	$(CC) $(CPPFLAGS) $(VP_CFLAGS) -o $@ -c $<
videoplayer_ui.o: $(VP_DIR)/videoplayer_ui.c $(VP_DIR)/videoplayer.h
	$(CC) $(CPPFLAGS) $(VP_CFLAGS) -o $@ -c $<

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

$(SRC)/config.h:
	cp $(SRC)/config.def.h $@
clean:
	rm -f nixlytile *.o $(SRC)/*-protocol.h game_params.conf

dist: clean
	mkdir -p nixlytile-$(VERSION)
	cp -R Makefile config.mk protocols \
		nixlytile.1 nixlytile.desktop images src \
		nixlytile-$(VERSION)
	tar -caf nixlytile-$(VERSION).tar.gz nixlytile-$(VERSION)
	rm -rf nixlytile-$(VERSION)

# Generate game_params.conf from game_launch_params.h
# Format: APPID|nvidia_params|amd_params|amd_amdvlk_params|intel_params
game_params.conf: $(SRC)/game_launch_params.h
	@echo "Generating game_params.conf from game_launch_params.h..."
	@echo "# Auto-generated from game_launch_params.h" > $@
	@echo "# Format: APPID|nvidia_params|amd_params|amd_amdvlk_params|intel_params" >> $@
	@echo "# Generated: $$(date)" >> $@
	@echo "" >> $@
	@awk ' \
		/\.game_id *= *"/ { \
			match($$0, /"[^"]+"/); \
			game_id = substr($$0, RSTART+1, RLENGTH-2) \
		} \
		/\.nvidia *= *"/ { \
			match($$0, /"[^"]*"/); \
			nvidia = substr($$0, RSTART+1, RLENGTH-2) \
		} \
		/\.amd *= *"/ && !/\.amd_amdvlk/ { \
			match($$0, /"[^"]*"/); \
			amd = substr($$0, RSTART+1, RLENGTH-2) \
		} \
		/\.amd_amdvlk *= *"/ { \
			match($$0, /"[^"]*"/); \
			amdvlk = substr($$0, RSTART+1, RLENGTH-2) \
		} \
		/\.intel *= *"/ { \
			match($$0, /"[^"]*"/); \
			intel = substr($$0, RSTART+1, RLENGTH-2) \
		} \
		/^\t\},$$/ || /^\t\}$$/ { \
			if (game_id != "" && game_id !~ /NULL/) { \
				print game_id "|" nvidia "|" amd "|" amdvlk "|" intel \
			} \
			game_id = ""; nvidia = ""; amd = ""; amdvlk = ""; intel = "" \
		} \
	' $(SRC)/game_launch_params.h >> $@

install: nixlytile game_params.conf
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	rm -f $(DESTDIR)$(PREFIX)/bin/nixlytile
	cp -f nixlytile $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/nixlytile
	mkdir -p $(DESTDIR)$(DATADIR)/nixlytile/images
	cp -r images/svg $(DESTDIR)$(DATADIR)/nixlytile/images/
	mkdir -p $(DESTDIR)$(DATADIR)/nixlytile
	cp -f config.conf.example $(DESTDIR)$(DATADIR)/nixlytile/config.conf.example
	chmod 644 $(DESTDIR)$(DATADIR)/nixlytile/config.conf.example
	cp -f game_params.conf $(DESTDIR)$(DATADIR)/nixlytile/game_params.conf
	chmod 644 $(DESTDIR)$(DATADIR)/nixlytile/game_params.conf
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

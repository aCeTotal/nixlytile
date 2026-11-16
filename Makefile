include config.mk

BUILD = build
PROTO = $(BUILD)/protocols

PKGS = wayland-server wayland-client xkbcommon libinput libdrm libudev libseat gbm vulkan \
	pixman-1 libdisplay-info xcb xcb-composite xcb-ewmh xcb-icccm xcb-randr \
	xcb-render xcb-res xcb-xfixes xcb-errors fcft fontconfig libsystemd cairo librsvg-2.0 gdk-pixbuf-2.0 \
	glib-2.0 libqrencode libturbojpeg

WARNINGS = -Wpedantic -Wall -Wextra -Wno-unused-parameter -Wshadow -Wunused-macros \
	-Werror=strict-prototypes -Werror=implicit -Werror=return-type \
	-Werror=incompatible-pointer-types -Wfloat-conversion

NXCFLAGS = $(shell $(PKG_CONFIG) --cflags $(PKGS)) -I. -Isrc -I$(BUILD) -I$(PROTO) \
	-D_POSIX_C_SOURCE=200809L -DXWAYLAND -DVERSION=\"$(VERSION)\" -DXWAYLAND_PATH=\"$(XWAYLAND_PATH)\" -DICON_FONT_DIR=\"$(ICON_FONT_DIR)\" \
	-DGAZE_MODEL_DIR=\"$(GAZE_MODEL_DIR)\" \
	-g $(OPTFLAGS) $(WARNINGS) $(CFLAGS)
LDLIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lquirc -lm -lpthread $(LIBS)

WAYLAND_SCANNER = $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
WP = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
HWDATA = $(shell $(PKG_CONFIG) --variable=pkgdatadir hwdata)

PROTOCOL_XMLS = \
	$(WP)/stable/linux-dmabuf/linux-dmabuf-v1.xml \
	$(WP)/stable/presentation-time/presentation-time.xml \
	$(WP)/stable/tablet/tablet-v2.xml \
	$(WP)/stable/viewporter/viewporter.xml \
	$(WP)/stable/xdg-shell/xdg-shell.xml \
	$(WP)/staging/alpha-modifier/alpha-modifier-v1.xml \
	$(WP)/staging/color-management/color-management-v1.xml \
	$(WP)/staging/color-representation/color-representation-v1.xml \
	$(WP)/staging/commit-timing/commit-timing-v1.xml \
	$(WP)/staging/content-type/content-type-v1.xml \
	$(WP)/staging/cursor-shape/cursor-shape-v1.xml \
	$(WP)/staging/ext-data-control/ext-data-control-v1.xml \
	$(WP)/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml \
	$(WP)/staging/ext-idle-notify/ext-idle-notify-v1.xml \
	$(WP)/staging/ext-image-capture-source/ext-image-capture-source-v1.xml \
	$(WP)/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml \
	$(WP)/staging/ext-session-lock/ext-session-lock-v1.xml \
	$(WP)/staging/fifo/fifo-v1.xml \
	$(WP)/staging/fractional-scale/fractional-scale-v1.xml \
	$(WP)/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml \
	$(WP)/staging/security-context/security-context-v1.xml \
	$(WP)/staging/single-pixel-buffer/single-pixel-buffer-v1.xml \
	$(WP)/staging/tearing-control/tearing-control-v1.xml \
	$(WP)/staging/xdg-activation/xdg-activation-v1.xml \
	$(WP)/staging/xdg-dialog/xdg-dialog-v1.xml \
	$(WP)/staging/xdg-system-bell/xdg-system-bell-v1.xml \
	$(WP)/staging/xwayland-shell/xwayland-shell-v1.xml \
	$(WP)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml \
	$(WP)/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml \
	$(WP)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml \
	$(WP)/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml \
	$(WP)/unstable/primary-selection/primary-selection-unstable-v1.xml \
	$(WP)/unstable/relative-pointer/relative-pointer-unstable-v1.xml \
	$(WP)/unstable/text-input/text-input-unstable-v3.xml \
	$(WP)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
	$(WP)/unstable/xdg-foreign/xdg-foreign-unstable-v2.xml \
	$(WP)/unstable/xdg-output/xdg-output-unstable-v1.xml \
	$(wildcard protocols/*.xml)
CLIENT_PROTOCOLS = xdg-shell xdg-decoration-unstable-v1 xdg-activation-v1 linux-dmabuf-v1 \
	linux-drm-syncobj-v1 presentation-time pointer-gestures-unstable-v1 \
	relative-pointer-unstable-v1

PROTOCOLS = $(basename $(notdir $(PROTOCOL_XMLS)))
PROTO_HDRS = $(PROTOCOLS:%=$(PROTO)/%-protocol.h) $(CLIENT_PROTOCOLS:%=$(PROTO)/%-client-protocol.h)
PROTO_SRCS = $(PROTOCOLS:%=$(PROTO)/%-protocol.c)
SHADERS = $(wildcard platform/vulkan/shaders/*.vert platform/vulkan/shaders/*.frag)
SHADER_HDRS = $(SHADERS:%=$(BUILD)/%.h)
PNPIDS = $(BUILD)/platform/drm/pnp_ids.c
GENERATED = $(PROTO_HDRS) $(SHADER_HDRS)

HELPERS = src/nixly_fand.c src/nixly_diskd.c src/nixly_jaild.c src/jaild_attach.c
SRCS = $(filter-out $(HELPERS),$(wildcard src/*.c src/*/*.c)) $(shell find platform -name '*.c')
OBJS = $(SRCS:%.c=$(BUILD)/%.o) $(PROTO_SRCS:%.c=%.o) $(PNPIDS:%.c=%.o)

xml_of = $(filter %/$(1).xml,$(PROTOCOL_XMLS))

all: nixlytile nixly-fand nixly-diskd nixly-jaild

nixlytile: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: %.c | $(GENERATED)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(NXCFLAGS) -MMD -MP -o $@ -c $<

$(PROTO)/%-protocol.o: $(PROTO)/%-protocol.c
	$(CC) $(CPPFLAGS) $(NXCFLAGS) -o $@ -c $<

$(PNPIDS:%.c=%.o): $(PNPIDS) | $(GENERATED)
	$(CC) $(CPPFLAGS) $(NXCFLAGS) -o $@ -c $<

.SECONDEXPANSION:
$(PROTO)/%-client-protocol.h: $$(call xml_of,$$*)
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $< $@
$(PROTO)/%-protocol.h: $$(call xml_of,$$*)
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) server-header $< $@
$(PROTO)/%-protocol.c: $$(call xml_of,$$*)
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) private-code $< $@

$(BUILD)/platform/vulkan/shaders/%.h: platform/vulkan/shaders/%
	@mkdir -p $(@D)
	glslang -V --quiet $< -o $@ --vn $(subst .,_,$*)_data

$(PNPIDS): platform/drm/pnp_ids.awk $(HWDATA)/pnp.ids
	@mkdir -p $(@D)
	awk -f platform/drm/pnp_ids.awk $(HWDATA)/pnp.ids > $@

src/config.h:
	cp src/config.def.h $@

nixly-fand: src/nixly_fand.c
	$(CC) -O2 -Wall -Wextra -o $@ src/nixly_fand.c -ldl
nixly-diskd: src/nixly_diskd.c
	$(CC) -O2 -Wall -Wextra -o $@ src/nixly_diskd.c
nixly-jaild: src/nixly_jaild.c src/jaild_attach.c src/jaild.h src/priv_sock.h
	$(CC) -O2 -Wall -Wextra -o $@ src/nixly_jaild.c src/jaild_attach.c

clean:
	rm -rf $(BUILD) nixlytile nixly-fand nixly-diskd nixly-jaild

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f nixlytile nixly-fand nixly-diskd nixly-jaild $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/nixlytile $(DESTDIR)$(PREFIX)/bin/nixly-fand \
		$(DESTDIR)$(PREFIX)/bin/nixly-diskd $(DESTDIR)$(PREFIX)/bin/nixly-jaild
	mkdir -p $(DESTDIR)$(DATADIR)/nixlytile/images
	cp -r images/svg $(DESTDIR)$(DATADIR)/nixlytile/images/
	cp -r theme $(DESTDIR)$(DATADIR)/nixlytile/
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp -f nixlytile.1 $(DESTDIR)$(MANDIR)/man1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/nixlytile.1
	mkdir -p $(DESTDIR)$(DATADIR)/wayland-sessions
	cp -f nixlytile.desktop $(DESTDIR)$(DATADIR)/wayland-sessions/nixlytile.desktop
	chmod 644 $(DESTDIR)$(DATADIR)/wayland-sessions/nixlytile.desktop
	mkdir -p $(DESTDIR)$(DATADIR)/xdg-desktop-portal/portals
	cp -f nixlytile.portal $(DESTDIR)$(DATADIR)/xdg-desktop-portal/portals/nixlytile.portal
	chmod 644 $(DESTDIR)$(DATADIR)/xdg-desktop-portal/portals/nixlytile.portal
	cp -f nixlytile-portals.conf $(DESTDIR)$(DATADIR)/xdg-desktop-portal/nixlytile-portals.conf
	chmod 644 $(DESTDIR)$(DATADIR)/xdg-desktop-portal/nixlytile-portals.conf
	mkdir -p $(DESTDIR)$(DATADIR)/polkit-1/actions
	cp -f org.nixlytile.jail-grant.policy $(DESTDIR)$(DATADIR)/polkit-1/actions/
	chmod 644 $(DESTDIR)$(DATADIR)/polkit-1/actions/org.nixlytile.jail-grant.policy

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nixlytile $(DESTDIR)$(PREFIX)/bin/nixly-fand \
		$(DESTDIR)$(PREFIX)/bin/nixly-diskd $(DESTDIR)$(PREFIX)/bin/nixly-jaild \
		$(DESTDIR)$(MANDIR)/man1/nixlytile.1 \
		$(DESTDIR)$(DATADIR)/wayland-sessions/nixlytile.desktop \
		$(DESTDIR)$(DATADIR)/xdg-desktop-portal/portals/nixlytile.portal \
		$(DESTDIR)$(DATADIR)/xdg-desktop-portal/nixlytile-portals.conf \
		$(DESTDIR)$(DATADIR)/polkit-1/actions/org.nixlytile.jail-grant.policy
	rm -rf $(DESTDIR)$(DATADIR)/nixlytile/theme

.PHONY: all clean install uninstall

-include $(OBJS:.o=.d)

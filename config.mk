_VERSION = 0.8-dev
VERSION  = `git describe --tags --dirty 2>/dev/null || echo $(_VERSION)`

PKG_CONFIG = pkg-config

PREFIX = /usr/local
MANDIR = $(PREFIX)/share/man
DATADIR = $(PREFIX)/share

XWAYLAND_PATH = `$(PKG_CONFIG) --variable=xwayland xwayland`

# Extra font dir for bar icons; empty uses system fonts.
ICON_FONT_DIR =

# Gaze tracking model directory.
GAZE_MODEL_DIR = $(DATADIR)/nixlytile/gaze

OPTFLAGS = -O3

CC = cc

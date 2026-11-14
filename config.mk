_VERSION = 0.8-dev
VERSION  = `git describe --tags --dirty 2>/dev/null || echo $(_VERSION)`

PKG_CONFIG = pkg-config

PREFIX = /usr/local
MANDIR = $(PREFIX)/share/man
DATADIR = $(PREFIX)/share

XWAYLAND_PATH = `$(PKG_CONFIG) --variable=xwayland xwayland`

OPTFLAGS = -O3

CC = cc

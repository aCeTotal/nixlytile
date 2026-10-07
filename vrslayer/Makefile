.POSIX:
.SUFFIXES:
.SUFFIXES: .c .o

PREFIX  = /usr/local
LIBDIR  = $(PREFIX)/lib
DATADIR = $(PREFIX)/share
BITS    = 64
# Overrides .POSIX default -O1.
OPTFLAGS = -O2

LIB      = libnixly_vrs.so
MANIFEST = nixly_vrs_$(BITS).json
OBJS     = layer.o instance.o registry.o device.o features.o pipeline.o \
           rate.o rendering.o upload.o pattern.o timing.o control.o frame.o \
           submit.o connect.o drawrate.o

VRS_CFLAGS  = -std=c11 -g -fPIC -fvisibility=hidden -D_GNU_SOURCE -DVK_NO_PROTOTYPES \
              -Wall -Wextra -Wpedantic -Wshadow -Wfloat-conversion \
              -Wstrict-prototypes $(CFLAGS) $(OPTFLAGS)
VRS_LDFLAGS = -shared -Wl,-z,defs $(LDFLAGS)
LDLIBS      = -lm -lpthread

all: $(LIB) $(MANIFEST)

$(LIB): $(OBJS)
	$(CC) $(VRS_LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

.c.o:
	$(CC) $(VRS_CFLAGS) -c $<

$(OBJS): layer.h link.h pattern.h control.h

$(MANIFEST): manifest.json.in
	sed -e 's|@LIB@|$(LIBDIR)/$(LIB)|' -e 's|@BITS@|$(BITS)|g' \
		manifest.json.in > $@

install: all
	mkdir -p $(DESTDIR)$(LIBDIR) $(DESTDIR)$(DATADIR)/vulkan/implicit_layer.d
	cp -f $(LIB) $(DESTDIR)$(LIBDIR)/$(LIB)
	cp -f $(MANIFEST) $(DESTDIR)$(DATADIR)/vulkan/implicit_layer.d/$(MANIFEST)

clean:
	rm -f $(LIB) $(MANIFEST) $(OBJS)

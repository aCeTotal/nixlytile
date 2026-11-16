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
DISPATCH = dispatch/layer.o dispatch/instance.o dispatch/registry.o dispatch/device.o \
           dispatch/features.o dispatch/gpu.o dispatch/chainsize.o
FRAME    = frame/frame.o frame/submit.o frame/swap.o frame/connect.o
PASSES   = passes/passes.o passes/handlemap.o passes/pipestate.o passes/passconvert.o \
           passes/renderpass.o passes/framebuffer.o passes/beginpass.o passes/neutral.o \
           passes/pipeline.o passes/drawrate.o passes/drawrule.o passes/recording.o \
           passes/rendering.o passes/perpixel.o passes/shaderobject.o
SPIRV    = spirv/shader.o spirv/flow.o spirv/fold.o spirv/write.o
FOVEA    = fovea/map.o fovea/rate.o fovea/upload.o fovea/present.o fovea/worker.o
OBJS     = $(DISPATCH) $(FRAME) $(PASSES) $(SPIRV) $(FOVEA)

LAYER_HDRS  = dispatch/layer.h dispatch/gpu.h fovea/fovea.h fovea/map.h link.h
PASSES_HDRS = passes/passes.h passes/handlemap.h $(LAYER_HDRS)

VRS_CFLAGS  = -std=c11 -g -fPIC -fvisibility=hidden -D_GNU_SOURCE -DVK_NO_PROTOTYPES \
              -I. -Idispatch -Ipasses -Ispirv -Ifovea \
              -Wall -Wextra -Wpedantic -Wshadow -Wfloat-conversion \
              -Wstrict-prototypes $(CFLAGS) $(OPTFLAGS)
VRS_LDFLAGS = -shared -Wl,-z,defs $(LDFLAGS)
LDLIBS      = -lm -lpthread

all: $(LIB) $(MANIFEST)

$(LIB): $(OBJS)
	$(CC) $(VRS_LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

.c.o:
	$(CC) $(VRS_CFLAGS) -c -o $@ $<

$(FOVEA) dispatch/chainsize.o dispatch/features.o dispatch/gpu.o dispatch/instance.o \
	dispatch/registry.o frame/connect.o frame/frame.o frame/swap.o: $(LAYER_HDRS)
$(PASSES) dispatch/device.o dispatch/layer.o frame/submit.o \
	spirv/shader.o: $(PASSES_HDRS)
$(SPIRV): spirv/spirv.h

$(MANIFEST): manifest.json.in
	sed -e 's|@LIB@|$(LIBDIR)/$(LIB)|' -e 's|@BITS@|$(BITS)|g' \
		manifest.json.in > $@

install: all
	mkdir -p $(DESTDIR)$(LIBDIR) $(DESTDIR)$(DATADIR)/vulkan/implicit_layer.d
	cp -f $(LIB) $(DESTDIR)$(LIBDIR)/$(LIB)
	cp -f $(MANIFEST) $(DESTDIR)$(DATADIR)/vulkan/implicit_layer.d/$(MANIFEST)

clean:
	rm -f $(LIB) $(MANIFEST) $(OBJS)

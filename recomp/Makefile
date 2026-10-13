# bootgate build: generated code (gen/) compiled once; runtime/HLE rebuilt on change. Use make -j2 at most.
CC ?= gcc
GENFLAGS = -O1 -w -fno-strict-aliasing -I. -Igen
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
ifneq ($(findstring mingw,$(shell $(CC) -dumpmachine)),)
# MSYS2 UCRT64: keep the console (no -mwindows) and our own main() (no SDL2main)
SDL_LIBS := $(filter-out -mwindows -lSDL2main,$(SDL_LIBS))
EXE = hgboot.exe
LDFLAGS_OS =
else
EXE = hgboot
LDFLAGS_OS = -rdynamic
endif
ifneq ($(SDL_LIBS),)
SDL_CFLAGS += -DHAVE_SDL
endif
RTFLAGS = -O1 -g -Wall -Wno-unused -fno-strict-aliasing -I. -Igen
GENSRC = $(wildcard gen/part*.c) gen/table.c $(wildcard gen/vu1_aot_*.c)
GENOBJ = $(patsubst gen/%.c,obj/%.o,$(GENSRC))
all: $(EXE)
# VU1 programs compiled ahead of time (vu1gen.py): the flags of vu1.c, whose semantics they inline
obj/vu1_aot_%.o: gen/vu1_aot_%.c vu1_core.h ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -O2 -ffp-contract=off -g0 -c $< -o $@
obj/%.o: gen/%.c ee_runtime.h
	@mkdir -p obj
	$(CC) $(GENFLAGS) -c $< -o $@
obj/runtime.o: runtime.c ee_runtime.h hle.h compat.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
obj/hle.o: hle.c ee_runtime.h hle.h compat.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
obj/iop_pad_mc.o: iop_pad_mc.c ee_runtime.h hle.h compat.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
obj/overrides.o: overrides.c ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
obj/gs.o: gs.c ee_runtime.h hle.h compat.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -O2 -ffp-contract=off -c $< -o $@
obj/dmac.o: dmac.c ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
obj/host_sdl.o: host_sdl.c ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) $(SDL_CFLAGS) -c $< -o $@
obj/vu1.o: vu1.c vu1_core.h ee_runtime.h hle.h vu1_state.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -O2 -ffp-contract=off -c $< -o $@
obj/ipu.o: ipu.c ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -O2 -c $< -o $@
obj/vu1_ref.o: vu1_ref.c ee_runtime.h hle.h vu1_state.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -O2 -ffp-contract=off -c $< -o $@
obj/iop_cpu.o: iop_cpu.c ee_runtime.h hle.h iop.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -O2 -c $< -o $@
obj/iop_kernel.o: iop_kernel.c ee_runtime.h hle.h iop.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -O2 -c $< -o $@
obj/spu2.o: spu2.c ee_runtime.h hle.h iop.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -O2 -c $< -o $@
obj/interp.o: interp.c ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
obj/compat.o: compat.c compat.h ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
$(EXE): $(GENOBJ) obj/runtime.o obj/hle.o obj/interp.o obj/iop_pad_mc.o obj/overrides.o obj/gs.o obj/dmac.o obj/host_sdl.o obj/vu1.o obj/vu1_ref.o obj/ipu.o obj/iop_cpu.o obj/iop_kernel.o obj/spu2.o obj/compat.o
	$(CC) $(LDFLAGS_OS) -o $@ $^ -lm -lpthread $(SDL_LIBS)
clean:
	rm -rf obj hgboot hgboot.exe

# bootgate build: generated code (gen/) compiled once; runtime/HLE rebuilt on change. Use make -j2 at most.
CC ?= gcc
GENFLAGS = -O1 -w -fno-strict-aliasing -I. -Igen
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
# objects per OS (obj, obj-linux), so a drive shared by Windows and Linux keeps both builds
ifneq ($(findstring mingw,$(shell $(CC) -dumpmachine)),)
# MSYS2 UCRT64: keep the console (no -mwindows) and our own main() (no SDL2main)
SDL_LIBS := $(filter-out -mwindows -lSDL2main,$(SDL_LIBS))
EXE = hgboot.exe
O = obj
LDFLAGS_OS =
else
EXE = hgboot
O = obj-linux
LDFLAGS_OS = -rdynamic
OS_OBJ = $(O)/glibc23.o   # (the C23 strtol/sscanf names, so the game needs glibc 2.34 only)
endif
ifneq ($(SDL_LIBS),)
SDL_CFLAGS += -DHAVE_SDL
endif
# Vulkan (GS on the GPU, gsvk.c): used when pkg-config finds it; shaders are compiled to SPIR-V headers in gen/
VK_LIBS := $(shell pkg-config --libs vulkan 2>/dev/null)
ifneq ($(VK_LIBS),)
VK_CFLAGS = -DHAVE_VULKAN
VK_OBJ = $(O)/gsvk.o
endif
RTFLAGS = -O1 -g -Wall -Wno-unused -fno-strict-aliasing -I. -Igen
GENSRC = $(wildcard gen/part*.c) gen/table.c $(wildcard gen/vu1_aot_*.c)
GENOBJ = $(patsubst gen/%.c,$(O)/%.o,$(GENSRC))
all: $(EXE)
# VU1 programs compiled ahead of time (vu1gen.py): the flags of vu1.c, whose semantics they inline
$(O)/vu1_aot_%.o: gen/vu1_aot_%.c vu1_core.h ee_runtime.h hle.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -O2 -ffp-contract=off -g0 -c $< -o $@
$(O)/%.o: gen/%.c ee_runtime.h
	@mkdir -p $(O)
	$(CC) $(GENFLAGS) -c $< -o $@
$(O)/runtime.o: runtime.c ee_runtime.h hle.h compat.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -c $< -o $@
$(O)/hle.o: hle.c ee_runtime.h hle.h compat.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -c $< -o $@
$(O)/iop_pad_mc.o: iop_pad_mc.c ee_runtime.h hle.h compat.h settings.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -c $< -o $@
$(O)/overrides.o: overrides.c ee_runtime.h hle.h settings.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -c $< -o $@
$(O)/options.o: game/options.c game/guest.h ee_runtime.h hle.h settings.h hdtex.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -c $< -o $@
$(O)/gs.o: gs.c ee_runtime.h hle.h compat.h gsvk.h settings.h hdtex.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) $(VK_CFLAGS) -O2 -ffp-contract=off -c $< -o $@
gen/gs_sprite_spv.h: vk/gs_sprite.comp
	glslangValidator -V --target-env vulkan1.1 --vn gs_sprite_spv -o $@ $<
gen/gs_batch_spv.h: vk/gs_sprite.comp
	glslangValidator -V --target-env vulkan1.1 -DBATCH --vn gs_batch_spv -o $@ $<
gen/gs_batchc_spv.h: vk/gs_sprite.comp
	glslangValidator -V --target-env vulkan1.1 -DBATCH -DCOPIES --vn gs_batchc_spv -o $@ $<
$(O)/gsvk.o: gsvk.c gsvk.h gen/gs_sprite_spv.h gen/gs_batch_spv.h gen/gs_batchc_spv.h ee_runtime.h hle.h settings.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -c $< -o $@
$(O)/hdtex.o: hdtex.c hdtex.h settings.h ee_runtime.h hle.h third_party/stb_image.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) $(VK_CFLAGS) -O2 -c $< -o $@
$(O)/dmac.o: dmac.c ee_runtime.h hle.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -c $< -o $@
$(O)/settings.o: settings.c settings.h hle.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) $(SDL_CFLAGS) -c $< -o $@
$(O)/host_sdl.o: host_sdl.c ee_runtime.h hle.h gsvk.h settings.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) $(SDL_CFLAGS) $(VK_CFLAGS) -c $< -o $@
$(O)/vu1.o: vu1.c vu1_core.h ee_runtime.h hle.h vu1_state.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -O2 -ffp-contract=off -c $< -o $@
$(O)/ipu.o: ipu.c ee_runtime.h hle.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -O2 -c $< -o $@
$(O)/vu1_ref.o: vu1_ref.c ee_runtime.h hle.h vu1_state.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -O2 -ffp-contract=off -c $< -o $@
$(O)/iop_cpu.o: iop_cpu.c ee_runtime.h hle.h iop.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -O2 -c $< -o $@
$(O)/iop_kernel.o: iop_kernel.c ee_runtime.h hle.h iop.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -O2 -c $< -o $@
$(O)/spu2.o: spu2.c ee_runtime.h hle.h iop.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -O2 -c $< -o $@
$(O)/interp.o: interp.c ee_runtime.h hle.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -c $< -o $@
$(O)/compat.o: compat.c compat.h ee_runtime.h hle.h
	@mkdir -p $(O); $(CC) $(RTFLAGS) -c $< -o $@
$(O)/glibc23.o: glibc23.c
	@mkdir -p $(O); $(CC) $(RTFLAGS) -c $< -o $@
$(EXE): $(GENOBJ) $(O)/runtime.o $(O)/hle.o $(O)/interp.o $(O)/iop_pad_mc.o $(O)/overrides.o $(O)/options.o $(O)/settings.o $(O)/gs.o $(O)/dmac.o $(O)/host_sdl.o $(O)/vu1.o $(O)/vu1_ref.o $(O)/ipu.o $(O)/iop_cpu.o $(O)/iop_kernel.o $(O)/spu2.o $(O)/compat.o $(O)/hdtex.o $(VK_OBJ) $(OS_OBJ)
	$(CC) $(LDFLAGS_OS) -o $@ $^ -lm -lpthread $(SDL_LIBS) $(VK_LIBS)
clean:
	rm -rf $(O) $(EXE)

# bootgate build: generated code (gen/) compiled once; runtime/HLE rebuilt on change. Use make -j2 at most.
CC ?= gcc
GENFLAGS = -O1 -w -fno-strict-aliasing -I. -Igen
RTFLAGS = -O1 -g -Wall -Wno-unused -fno-strict-aliasing -I. -Igen
GENSRC = $(wildcard gen/part*.c) gen/table.c
GENOBJ = $(patsubst gen/%.c,obj/%.o,$(GENSRC))
all: hgboot
obj/%.o: gen/%.c ee_runtime.h
	@mkdir -p obj
	$(CC) $(GENFLAGS) -c $< -o $@
obj/runtime.o: runtime.c ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
obj/hle.o: hle.c ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
obj/iop_pad_mc.o: iop_pad_mc.c ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
obj/overrides.o: overrides.c ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
obj/interp.o: interp.c ee_runtime.h hle.h
	@mkdir -p obj; $(CC) $(RTFLAGS) -c $< -o $@
hgboot: $(GENOBJ) obj/runtime.o obj/hle.o obj/interp.o obj/iop_pad_mc.o obj/overrides.o
	$(CC) -rdynamic -o $@ $^ -lm
clean:
	rm -rf obj hgboot

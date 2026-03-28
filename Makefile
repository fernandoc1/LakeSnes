
CC = g++
CFLAGS = -g -I ./snes -I ./zip -I ./mem_viewer

execname = lakesnes
trace_dump_exec = ltrc_dump
sdlcflags = `sdl2-config --cflags`
sdlldflags = `sdl2-config --libs`
dlldflags = -ldl

# Source files
cfiles = snes/spc.c snes/dsp.c snes/apu.c snes/dma.c snes/ppu.c snes/cart.c snes/input.c snes/statehandler.c snes/snes.c snes/snes_other.c \
 zip/zip.c tracing.c main.c
cppfiles = snes/cpu.cpp snes/trace_recorder.cpp snes/rom_disasm.cpp
hfiles = snes/spc.h snes/dsp.h snes/apu.h snes/cpu.h snes/dma.h snes/ppu.h snes/cart.h snes/input.h snes/statehandler.h snes/snes.h \
 snes/trace_recorder.h snes/rom_disasm.h zip/zip.h zip/miniz.h tracing.h

# Object files
ofiles = $(cfiles:.c=.o) $(cppfiles:.cpp=.cpp.o)

.PHONY: all clean

all: libs $(execname) $(trace_dump_exec)

libs:
	make -C mem_viewer
	make -C hooklib
	ln -sf mem_viewer/libmemviewer.so ./
	ln -sf mem_viewer/mem_viewer_helper ./
	ln -sf hooklib/libhooklib.so ./

%.o: %.c $(hfiles)
	$(CC) $(CFLAGS) $(sdlcflags) -c -o $@ $<

%.cpp.o: %.cpp $(hfiles)
	$(CC) $(CFLAGS) $(sdlcflags) -c -o $@ $<

$(execname): $(ofiles)
	$(CC) $(CFLAGS) -o $@ $(ofiles) $(sdlldflags) $(dlldflags) -L. -lmemviewer -Wl,-rpath,'$$ORIGIN'

$(trace_dump_exec): trace_dump.cpp snes/trace_recorder.cpp.o snes/cpu.cpp.o snes/spc.o snes/dsp.o snes/apu.o snes/dma.o snes/ppu.o snes/cart.o snes/input.o snes/statehandler.o snes/snes.o snes/snes_other.o zip/zip.o
	$(CC) $(CFLAGS) $(sdlcflags) -o $@ trace_dump.cpp snes/trace_recorder.cpp.o snes/cpu.cpp.o snes/spc.o snes/dsp.o snes/apu.o snes/dma.o snes/ppu.o snes/cart.o snes/input.o snes/statehandler.o snes/snes.o snes/snes_other.o zip/zip.o $(sdlldflags) -L. -lmemviewer -Wl,-rpath,'$$ORIGIN'

clean:
	make -C mem_viewer clean
	make -C hooklib clean
	rm -f $(execname) $(trace_dump_exec) $(ofiles)

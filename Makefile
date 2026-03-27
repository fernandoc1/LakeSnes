
CC = g++
CFLAGS = -g -I ./snes -I ./zip -I ./mem_viewer

execname = lakesnes
sdlcflags = `sdl2-config --cflags`
sdlldflags = `sdl2-config --libs`

# Source files
cfiles = snes/spc.c snes/dsp.c snes/apu.c snes/dma.c snes/ppu.c snes/cart.c snes/input.c snes/statehandler.c snes/snes.c snes/snes_other.c \
 zip/zip.c tracing.c main.c
cppfiles = snes/cpu.cpp snes/trace_recorder.cpp
hfiles = snes/spc.h snes/dsp.h snes/apu.h snes/cpu.h snes/dma.h snes/ppu.h snes/cart.h snes/input.h snes/statehandler.h snes/snes.h \
 snes/trace_recorder.h zip/zip.h zip/miniz.h tracing.h

# Object files
ofiles = $(cfiles:.c=.o) $(cppfiles:.cpp=.cpp.o)

.PHONY: all clean

all: libs $(execname)

libs:
	make -C mem_viewer
	ln -sf mem_viewer/libmemviewer.so ./
	ln -sf mem_viewer/mem_viewer_helper ./

%.o: %.c $(hfiles)
	$(CC) $(CFLAGS) $(sdlcflags) -c -o $@ $<

%.cpp.o: %.cpp $(hfiles)
	$(CC) $(CFLAGS) $(sdlcflags) -c -o $@ $<

$(execname): $(ofiles)
	$(CC) $(CFLAGS) -o $@ $(ofiles) $(sdlldflags) -L. -lmemviewer -Wl,-rpath,'$$ORIGIN'

clean:
	make -C mem_viewer clean
	rm -f $(execname) $(ofiles)

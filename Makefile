
CC = g++
CFLAGS = -g -I ./snes -I ./zip -I ./mem_viewer

execname = lakesnes
sdlcflags = `sdl2-config --cflags`
sdlldflags = `sdl2-config --libs`

# Source files
cfiles = snes/spc.c snes/dsp.c snes/apu.c snes/cpu.c snes/dma.c snes/ppu.c snes/cart.c snes/input.c snes/statehandler.c snes/snes.c snes/snes_other.c \
 zip/zip.c tracing.c main.c
hfiles = snes/spc.h snes/dsp.h snes/apu.h snes/cpu.h snes/dma.h snes/ppu.h snes/cart.h snes/input.h snes/statehandler.h snes/snes.h \
 zip/zip.h zip/miniz.h tracing.h

# Object files
ofiles = $(cfiles:.c=.o)

.PHONY: all clean

all: libs $(execname)

libs:
	make -C mem_viewer
	mv mem_viewer/libmem_viewer.so ./

%.o: %.c $(hfiles)
	$(CC) $(CFLAGS) $(sdlcflags) -c -o $@ $<

$(execname): $(ofiles)
	$(CC) $(CFLAGS) -o $@ $(ofiles) $(sdlldflags) -L. -lmem_viewer -Wl,-rpath,'$$ORIGIN'

clean:
	make -C mem_viewer clean
	rm -f $(execname) $(ofiles)

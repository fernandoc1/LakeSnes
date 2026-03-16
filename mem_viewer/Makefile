CC := cc
CFLAGS := -Wall -Wextra -Werror -std=c11 -fPIC -Iinclude $(shell pkg-config --cflags sdl2)
LDFLAGS :=
SDL_LIBS := $(shell pkg-config --libs sdl2)

LIB_NAME := libmem_viewer.so
LIB_OBJS := src/mem_viewer.o
APP := test_mem_viewer
APP_OBJS := examples/test_app.o

.PHONY: all clean run test

all: $(LIB_NAME) $(APP)

$(LIB_NAME): $(LIB_OBJS)
	$(CC) -shared $(LDFLAGS) -o $@ $^ $(SDL_LIBS)

$(APP): $(APP_OBJS) $(LIB_NAME)
	$(CC) $(CFLAGS) -o $@ $(APP_OBJS) -L. -lmem_viewer $(SDL_LIBS) -Wl,-rpath,'$$ORIGIN'

src/%.o: src/%.c include/mem_viewer.h
	$(CC) $(CFLAGS) -c $< -o $@

examples/%.o: examples/%.c include/mem_viewer.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(APP)
	./$(APP)

test: $(APP)
	@printf '%s\n' 'Built test_mem_viewer. Run ./test_mem_viewer and close all windows manually to finish the test.'

clean:
	rm -f $(LIB_OBJS) $(APP_OBJS) $(LIB_NAME) $(APP)

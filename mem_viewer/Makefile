CC := gcc
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS := $(shell pkg-config --libs sdl2)
X11_LIBS := -lX11
CFLAGS := -Wall -Wextra -Werror -std=c11 -fPIC -I. $(GTK_CFLAGS) $(SDL_CFLAGS)
LDFLAGS :=

LIB := libmem_viewer.so
LIB_OBJS := mem_viewer.o
TEST := test_mem_viewer
TEST_OBJS := tests/test_mem_viewer.o
MANUAL_TEST := manual_window_test
MANUAL_TEST_OBJS := tests/manual_window_test.o

.PHONY: all clean test manual-test

all: $(LIB) $(TEST) $(MANUAL_TEST)

$(LIB): $(LIB_OBJS)
	$(CC) -shared $(LDFLAGS) -o $@ $^ $(GTK_LIBS) $(X11_LIBS) -lpthread

$(TEST): $(TEST_OBJS) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) -L. -lmem_viewer $(GTK_LIBS) $(SDL_LIBS) $(X11_LIBS) -lpthread -Wl,-rpath,'$$ORIGIN'

$(MANUAL_TEST): $(MANUAL_TEST_OBJS) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(MANUAL_TEST_OBJS) -L. -lmem_viewer $(GTK_LIBS) $(SDL_LIBS) $(X11_LIBS) -lpthread -Wl,-rpath,'$$ORIGIN'

mem_viewer.o: mem_viewer.c mem_viewer.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/test_mem_viewer.o: tests/test_mem_viewer.c mem_viewer.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/manual_window_test.o: tests/manual_window_test.c mem_viewer.h
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST)
	broadwayd :5 >/tmp/mem_viewer_broadway.log 2>&1 & pid=$$!; \
	sleep 1; \
	GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 SDL_VIDEODRIVER=dummy ./$(TEST) --auto-exit; \
	status=$$?; \
	kill $$pid >/dev/null 2>&1; \
	wait $$pid 2>/dev/null; \
	exit $$status

manual-test: $(MANUAL_TEST)
	./$(MANUAL_TEST)

clean:
	rm -f $(LIB_OBJS) $(TEST_OBJS) $(MANUAL_TEST_OBJS) $(LIB) $(TEST) $(MANUAL_TEST)

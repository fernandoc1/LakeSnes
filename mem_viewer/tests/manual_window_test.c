#include "mem_viewer.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mem_viewer_debug_set_window_position(MemViewer *viewer, int x, int y);
int mem_viewer_debug_scroll_to_offset(MemViewer *viewer, size_t offset);

static void fill_memory(uint8_t *memory, size_t size)
{
    for (size_t i = 0U; i < size; ++i) {
        memory[i] = (uint8_t)((i * 37U + (i >> 1U)) & 0xFFU);
    }
}

static int parse_size_t_arg(const char *text, size_t *value)
{
    char *end;
    unsigned long long parsed;

    parsed = strtoull(text, &end, 0);
    if (*text == '\0' || *end != '\0') {
        return -1;
    }
    *value = (size_t)parsed;
    return 0;
}

static int parse_int_arg(const char *text, int *value)
{
    char *end;
    long parsed;

    parsed = strtol(text, &end, 0);
    if (*text == '\0' || *end != '\0') {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    enum { memory_size = 4096 };
    uint8_t memory[memory_size];
    MemViewer *viewer;
    SDL_Window *sdl_window;
    size_t offset;
    int viewer_x;
    int viewer_y;
    int running;

    offset = 0U;
    viewer_x = 120;
    viewer_y = 120;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            if (parse_size_t_arg(argv[++i], &offset) != 0 || offset >= memory_size) {
                fprintf(stderr, "invalid --offset value\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--x") == 0 && i + 1 < argc) {
            if (parse_int_arg(argv[++i], &viewer_x) != 0) {
                fprintf(stderr, "invalid --x value\n");
                return 1;
            }
            continue;
        }
        if (strcmp(argv[i], "--y") == 0 && i + 1 < argc) {
            if (parse_int_arg(argv[++i], &viewer_y) != 0) {
                fprintf(stderr, "invalid --y value\n");
                return 1;
            }
            continue;
        }

        fprintf(stderr, "usage: %s [--offset N] [--x N] [--y N]\n", argv[0]);
        return 1;
    }

    fill_memory(memory, sizeof(memory));

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    sdl_window = SDL_CreateWindow(
        "mem_viewer SDL probe",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        320,
        120,
        0
    );
    if (sdl_window == NULL) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    viewer = mem_viewer_open(memory, sizeof(memory));
    if (viewer == NULL) {
        fprintf(stderr, "mem_viewer_open failed\n");
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_set_window_position(viewer, viewer_x, viewer_y) != 0) {
        fprintf(stderr, "failed to move GTK viewer window\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_scroll_to_offset(viewer, offset) != 0) {
        fprintf(stderr, "failed to scroll GTK viewer to offset %zu\n", offset);
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    printf(
        "Opened GTK memory viewer at screen position (%d, %d), scrolled to byte offset %zu.\n"
        "Close the SDL window or press Escape in it to exit.\n",
        viewer_x,
        viewer_y,
        offset
    );

    running = 1;
    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            }
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                running = 0;
            }
        }
        SDL_Delay(16);
    }

    mem_viewer_destroy(viewer);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();
    return 0;
}

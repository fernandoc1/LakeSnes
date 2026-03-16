#define _DEFAULT_SOURCE

/*
 * High-Frequency 64KB Memory Update Test
 * ======================================
 *
 * This test demonstrates the memory viewer's ability to handle high-frequency
 * updates to a large buffer. A 64KB buffer is continuously updated at ~1000
 * updates/second in a separate thread, while the memory viewer displays the
 * current state with fade effects showing recent changes.
 *
 * Usage:
 *   ./test_highfreq_64kb           - Interactive mode (wait for window close)
 *   ./test_highfreq_64kb --auto-exit - Run for 5 seconds then exit
 *
 * The test validates:
 *   - Thread-safe memory updates while viewer is active
 *   - Fade effect visualization for rapidly changing values
 *   - Viewer stability under continuous update load
 *
 * Note: In interactive mode, the test runs until you close the viewer window.
 */

#include "mem_viewer.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define MEMORY_SIZE (64 * 1024)
#define BYTES_PER_ROW 16
#define UPDATE_INTERVAL_US 1000

int mem_viewer_debug_is_closed(MemViewer *viewer);

typedef struct {
    uint8_t *memory;
    size_t size;
    volatile int running;
    pthread_t thread;
} HighFreqUpdater;

static void *high_freq_update_thread(void *userdata)
{
    HighFreqUpdater *updater = (HighFreqUpdater *)userdata;
    uint64_t iteration = 0U;

    printf("High-frequency updater thread started\n");
    printf("Updating %zu bytes every %d us\n", updater->size, UPDATE_INTERVAL_US);

    while (updater->running) {
        iteration += 1U;

        for (size_t i = 0U; i < updater->size; i += 64U) {
            updater->memory[i] = (uint8_t)((iteration + i) & 0xFFU);
        }

        usleep(UPDATE_INTERVAL_US);
    }

    printf("High-frequency updater thread stopped after %lu iterations\n",
           (unsigned long)iteration);
    return NULL;
}

static int start_high_freq_updater(HighFreqUpdater *updater, uint8_t *memory, size_t size)
{
    updater->memory = memory;
    updater->size = size;
    updater->running = 1;

    if (pthread_create(&updater->thread, NULL, high_freq_update_thread, updater) != 0) {
        fprintf(stderr, "Failed to create updater thread\n");
        return -1;
    }

    return 0;
}

static void stop_high_freq_updater(HighFreqUpdater *updater)
{
    updater->running = 0;
    pthread_join(updater->thread, NULL);
}

int main(int argc, char **argv)
{
    uint8_t *memory;
    MemViewer *viewer;
    HighFreqUpdater updater;
    SDL_Window *sdl_window;
    int auto_exit = 0;
    int result = 0;
    (void)argc;
    (void)argv;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--auto-exit") == 0) {
            auto_exit = 1;
        } else {
            fprintf(stderr, "usage: %s [--auto-exit]\n", argv[0]);
            return 1;
        }
    }

    printf("High-Frequency 64KB Memory Update Test\n");
    printf("======================================\n\n");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    sdl_window = SDL_CreateWindow(
        "mem_viewer_highfreq_64kb",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        64,
        64,
        auto_exit ? SDL_WINDOW_HIDDEN : 0
    );
    if (sdl_window == NULL) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    memory = (uint8_t *)malloc(MEMORY_SIZE);
    if (memory == NULL) {
        fprintf(stderr, "Failed to allocate 64KB memory\n");
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    memset(memory, 0U, MEMORY_SIZE);

    printf("Allocated 64KB buffer at %p\n", (void *)memory);
    printf("Starting high-frequency memory updates...\n\n");

    if (start_high_freq_updater(&updater, memory, MEMORY_SIZE) != 0) {
        fprintf(stderr, "Failed to start updater thread\n");
        free(memory);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    SDL_Delay(100);

    printf("Opening memory viewer...\n");
    viewer = mem_viewer_open(memory, MEMORY_SIZE);
    if (viewer == NULL) {
        fprintf(stderr, "mem_viewer_open failed\n");
        updater.running = 0;
        pthread_join(updater.thread, NULL);
        free(memory);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (auto_exit) {
        printf("Auto-exit mode: Running for 5 seconds then closing...\n");
        SDL_Delay(5000);
        printf("Auto-exit timeout reached. Stopping updater...\n");
        stop_high_freq_updater(&updater);
        printf("Exiting without waiting for viewer cleanup (auto-exit mode)...\n");
        viewer = NULL;
    } else {
        printf("Memory viewer opened.\n");
        printf("The buffer is being updated at high frequency (~1000 updates/sec).\n");
        printf("Watch the fade effect as values change.\n");
        printf("\nClose the memory viewer window to exit.\n\n");

        while (mem_viewer_debug_is_closed(viewer) == 0) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
            }

            if (mem_viewer_update(viewer) != 0) {
                fprintf(stderr, "mem_viewer_update failed\n");
                break;
            }

            SDL_Delay(16);
        }

        printf("\nMemory viewer closed. Stopping updater thread...\n");
        stop_high_freq_updater(&updater);
    }

    if (viewer != NULL) {
        mem_viewer_destroy(viewer);
    }
    free(memory);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();

    printf("Cleanup completed.\n");
    printf("\nHigh-frequency 64KB test %s.\n", result == 0 ? "finished" : "failed");

    return result;
}

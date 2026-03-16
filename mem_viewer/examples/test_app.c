#include "mem_viewer.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>

static void fill_demo_memory(uint8_t *memory, size_t size, uint8_t seed)
{
    size_t i;

    for (i = 0; i < size; ++i) {
        memory[i] = (uint8_t)(((i * 37u) ^ (i >> 1) ^ seed) & 0xFFu);
    }
}

int main(void)
{
    enum { demo_size = 4096, viewer_count = 3 };
    uint8_t memory[viewer_count][demo_size];
    MemViewer *viewers[viewer_count];
    int closed[viewer_count];
    int open_count;
    int i;

    for (i = 0; i < viewer_count; ++i) {
        fill_demo_memory(memory[i], demo_size, (uint8_t)(0x10 + (i * 0x20)));
        closed[i] = 0;
    }

    for (i = 0; i < viewer_count; ++i) {
        viewers[i] = mem_viewer_open(memory[i], demo_size);
        if (viewers[i] == NULL) {
            fprintf(stderr, "failed to open viewer %d\n", i + 1);
            while (--i >= 0) {
                mem_viewer_destroy(viewers[i]);
            }
            return 1;
        }
    }

    for (i = 0; i < viewer_count; ++i) {
        fill_demo_memory(memory[i], demo_size, (uint8_t)(0x80 + (i * 0x20)));
        if (mem_viewer_update_memory(viewers[i], memory[i], demo_size) != 0) {
            fprintf(stderr, "failed to update viewer %d\n", i + 1);
            while (i >= 0) {
                mem_viewer_destroy(viewers[i]);
                --i;
            }
            return 1;
        }
    }

    open_count = viewer_count;
    while (open_count > 0) {
        SDL_Delay(50);

        for (i = 0; i < viewer_count; ++i) {
            if (closed[i]) {
                continue;
            }

            if (mem_viewer_update_memory(viewers[i], memory[i], demo_size) == 0) {
                continue;
            }

            closed[i] = 1;
            --open_count;
            mem_viewer_destroy(viewers[i]);
            viewers[i] = NULL;
        }
    }

    for (i = 0; i < viewer_count; ++i) {
        mem_viewer_destroy(viewers[i]);
    }

    printf("multi-window memory viewer test passed\n");
    return 0;
}

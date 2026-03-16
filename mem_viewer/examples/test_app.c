#include "mem_viewer.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>

typedef struct {
    MemViewer *viewer;
    uint8_t *first;
    uint8_t *second;
    size_t size;
    uint8_t base_seed;
} UpdateThreadData;

static void fill_demo_memory(uint8_t *memory, size_t size, uint8_t seed)
{
    size_t i;

    for (i = 0; i < size; ++i) {
        memory[i] = (uint8_t)(((i * 37u) ^ (i >> 1) ^ seed) & 0xFFu);
    }
}

static int update_thread_main(void *userdata)
{
    UpdateThreadData *data;
    int step;

    data = (UpdateThreadData *)userdata;
    for (step = 0; step < 6; ++step) {
        SDL_Delay(150);
        if ((step % 2) == 0) {
            fill_demo_memory(data->second, data->size, (uint8_t)(data->base_seed + 0x40 + step));
            if (mem_viewer_update_memory(data->viewer, data->second, data->size) != 0) {
                return 1;
            }
        } else {
            fill_demo_memory(data->first, data->size, (uint8_t)(data->base_seed + step));
            if (mem_viewer_update_memory(data->viewer, data->first, data->size) != 0) {
                return 1;
            }
        }
    }

    return 0;
}

int main(void)
{
    enum { demo_size = 4096, viewer_count = 2 };
    uint8_t first_memory[viewer_count][demo_size];
    uint8_t second_memory[viewer_count][demo_size];
    MemViewer *viewers[viewer_count];
    SDL_Thread *threads[viewer_count];
    UpdateThreadData thread_data[viewer_count];
    int i;
    int thread_result;

    for (i = 0; i < viewer_count; ++i) {
        fill_demo_memory(first_memory[i], demo_size, (uint8_t)(0x10 + (i * 16)));
        fill_demo_memory(second_memory[i], demo_size, (uint8_t)(0x80 + (i * 16)));

        viewers[i] = mem_viewer_open(first_memory[i], demo_size);
        if (viewers[i] == NULL) {
            fprintf(stderr, "failed to open viewer %d\n", i + 1);
            while (--i >= 0) {
                mem_viewer_destroy(viewers[i]);
            }
            return 1;
        }

        thread_data[i].viewer = viewers[i];
        thread_data[i].first = first_memory[i];
        thread_data[i].second = second_memory[i];
        thread_data[i].size = demo_size;
        thread_data[i].base_seed = (uint8_t)(i * 16);

        threads[i] = SDL_CreateThread(update_thread_main, "mem-viewer-update-test", &thread_data[i]);
        if (threads[i] == NULL) {
            fprintf(stderr, "failed to start update thread %d\n", i + 1);
            while (i >= 0) {
                mem_viewer_destroy(viewers[i]);
                --i;
            }
            return 1;
        }
    }

    for (i = 0; i < viewer_count; ++i) {
        SDL_WaitThread(threads[i], &thread_result);
        if (thread_result != 0) {
            fprintf(stderr, "memory update test failed for viewer %d\n", i + 1);
            return 1;
        }
    }

    SDL_Delay(2000);

    for (i = 0; i < viewer_count; ++i) {
        mem_viewer_destroy(viewers[i]);
    }

    printf("multi-window memory viewer test passed\n");
    return 0;
}

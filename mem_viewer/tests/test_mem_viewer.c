#include "mem_viewer.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t mem_viewer_debug_copy_text(MemViewer *viewer, char *buffer, size_t buffer_size);
int mem_viewer_debug_set_byte(MemViewer *viewer, size_t offset, uint8_t value);
int mem_viewer_debug_select_offset(MemViewer *viewer, size_t offset);
size_t mem_viewer_debug_get_selected_offset(MemViewer *viewer);
int mem_viewer_debug_set_auto_refresh(MemViewer *viewer, int enabled);
int mem_viewer_debug_set_search(MemViewer *viewer, const char *text, int decimal_mode);
int mem_viewer_debug_search_next(MemViewer *viewer);
int mem_viewer_debug_search_previous(MemViewer *viewer);
int mem_viewer_debug_set_changed_only(MemViewer *viewer, int enabled);
int mem_viewer_debug_get_visible_line_count(MemViewer *viewer);

static int text_contains(MemViewer *viewer, const char *needle)
{
    char *buffer;
    size_t required;
    int found;

    required = mem_viewer_debug_copy_text(viewer, NULL, 0U);
    if (required == 0U) {
        return 0;
    }

    buffer = (char *)malloc(required);
    if (buffer == NULL) {
        return 0;
    }

    if (mem_viewer_debug_copy_text(viewer, buffer, required) == 0U) {
        free(buffer);
        return 0;
    }

    found = strstr(buffer, needle) != NULL;
    free(buffer);
    return found;
}

static void pump_sdl_events(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
    }
}

int main(void)
{
    uint8_t memory[32];
    MemViewer *viewer;
    SDL_Window *sdl_window;

    for (size_t i = 0U; i < sizeof(memory); ++i) {
        memory[i] = (uint8_t)i;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    sdl_window = SDL_CreateWindow(
        "mem_viewer_sdl_probe",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        64,
        64,
        SDL_WINDOW_HIDDEN
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

    pump_sdl_events();

    if (!text_contains(viewer, "00000000: 00 01 02 03")) {
        fprintf(stderr, "GTK viewer did not render the initial bytes\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_get_selected_offset(viewer) != 0U) {
        fprintf(stderr, "GTK viewer did not initialize the offset selector to byte zero\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    memory[0] = 0xAA;
    memory[1] = 0xBB;
    memory[31] = 0xCC;
    if (mem_viewer_update(viewer) != 0) {
        fprintf(stderr, "mem_viewer_update failed\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (!text_contains(viewer, "00000000: AA BB 02 03")) {
        fprintf(stderr, "GTK viewer did not refresh after mem_viewer_update\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_set_byte(viewer, 5U, 0x7E) != 0) {
        fprintf(stderr, "failed to update a single byte through the GTK byte editor path\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (memory[5] != 0x7E) {
        fprintf(stderr, "single-byte edit did not update backing memory\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (!text_contains(viewer, "00000000: AA BB 02 03 04 7E")) {
        fprintf(stderr, "GTK viewer did not refresh after a single-byte edit\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_set_changed_only(viewer, 1) != 0) {
        fprintf(stderr, "failed to enable changed-lines-only filter\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_get_visible_line_count(viewer) != 1) {
        fprintf(stderr, "changed-lines-only filter did not reduce the view to one changed line\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_set_changed_only(viewer, 0) != 0) {
        fprintf(stderr, "failed to disable changed-lines-only filter\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    memory[20] = 0x7E;
    if (mem_viewer_update(viewer) != 0) {
        fprintf(stderr, "mem_viewer_update failed after creating a second search match\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_set_search(viewer, "7E", 0) != 0) {
        fprintf(stderr, "failed to set hex search value\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_get_selected_offset(viewer) != 5U) {
        fprintf(stderr, "hex search did not select the first matching byte\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_search_next(viewer) != 0 || mem_viewer_debug_get_selected_offset(viewer) != 20U) {
        fprintf(stderr, "search next did not advance to the second match\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_search_previous(viewer) != 0 || mem_viewer_debug_get_selected_offset(viewer) != 5U) {
        fprintf(stderr, "search previous did not return to the first match\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_set_search(viewer, "126", 1) != 0) {
        fprintf(stderr, "failed to set decimal search value\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_get_selected_offset(viewer) != 5U) {
        fprintf(stderr, "decimal search did not select the expected match\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_select_offset(viewer, 18U) != 0) {
        fprintf(stderr, "failed to select a byte offset through the GTK selection path\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_get_selected_offset(viewer) != 18U) {
        fprintf(stderr, "selecting a byte did not update the offset entry\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_set_auto_refresh(viewer, 1) != 0) {
        fprintf(stderr, "failed to enable auto refresh\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    memory[6] = 0x9C;
    SDL_Delay(250);

    if (!text_contains(viewer, "00000000: AA BB 02 03 04 7E 9C")) {
        fprintf(stderr, "GTK viewer did not refresh automatically after memory changed\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_set_auto_refresh(viewer, 0) != 0) {
        fprintf(stderr, "failed to disable auto refresh\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    pump_sdl_events();
    mem_viewer_destroy(viewer);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();
    printf("mem_viewer GTK/SDL integration test passed\n");
    return 0;
}

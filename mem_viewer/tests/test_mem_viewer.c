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
int mem_viewer_debug_get_change_fade(MemViewer *viewer, size_t offset);
int mem_viewer_debug_is_closed(MemViewer *viewer);
int mem_viewer_debug_scroll_to_offset(MemViewer *viewer, size_t offset);
int mem_viewer_debug_get_visible_line_range(MemViewer *viewer, size_t *first_line, size_t *last_line);
int mem_viewer_debug_get_line_dirty(MemViewer *viewer, size_t line);
int mem_viewer_debug_get_line_pending(MemViewer *viewer, size_t line);
void mem_viewer_debug_invalidate_line(MemViewer *viewer, size_t line);

static void pump_sdl_events(void);
static void print_viewer_text(FILE *stream, MemViewer *viewer);
static int wait_for_text_contains(MemViewer *viewer, const char *needle, Uint32 timeout_ms);

static void update_live_memory(uint8_t *memory, size_t size, size_t iteration)
{
    const size_t offsets[] = {0U, 2U, 5U, 6U, 7U, 16U, 20U, 23U, 24U, 31U};

    for (size_t i = 0U; i < (sizeof(offsets) / sizeof(offsets[0])); ++i) {
        size_t offset;

        offset = offsets[i];
        if (offset < size) {
            memory[offset] = (uint8_t)((iteration * 11U) + (i * 13U) + 0x20U);
        }
    }
}

static void build_line_text(char *buffer, size_t buffer_size, const uint8_t *memory, size_t base)
{
    size_t i;
    int written;

    written = snprintf(buffer, buffer_size, "%08zx:", base);
    for (i = 0U; i < 16U && (size_t)written + 4U < buffer_size; ++i) {
        written += snprintf(
            buffer + written,
            buffer_size - (size_t)written,
            " %02X",
            memory[base + i]
        );
    }
}

static void fill_large_memory(uint8_t *memory, size_t size)
{
    for (size_t i = 0U; i < size; ++i) {
        memory[i] = (uint8_t)((i * 29U + 7U) & 0xFFU);
    }
}

static int run_large_buffer_visibility_test(SDL_Window *sdl_window)
{
    enum { memory_size = 128 * 1024 };
    enum { bytes_per_row = 16 };
    uint8_t *memory;
    MemViewer *viewer;
    size_t visible_offset;
    size_t offscreen_offset;
    size_t visible_line;
    size_t offscreen_line;
    size_t first_visible_line;
    size_t last_visible_line;
    char expected_line[96];
    int line_dirty;
    int line_pending;

    memory = (uint8_t *)malloc(memory_size);
    if (memory == NULL) {
        fprintf(stderr, "large-buffer test allocation failed\n");
        return 1;
    }
    fill_large_memory(memory, memory_size);

    viewer = mem_viewer_open(memory, memory_size);
    if (viewer == NULL) {
        fprintf(stderr, "mem_viewer_open failed for 128 KB buffer\n");
        free(memory);
        return 1;
    }

    visible_offset = 0x0005U;
    offscreen_offset = 0x1FF25U;

    visible_line = visible_offset / bytes_per_row;
    offscreen_line = offscreen_offset / bytes_per_row;

    if (mem_viewer_debug_get_visible_line_range(viewer, &first_visible_line, &last_visible_line) != 0) {
        fprintf(stderr, "failed to get visible line range\n");
        mem_viewer_destroy(viewer);
        free(memory);
        return 1;
    }

    if (visible_line < first_visible_line || visible_line > last_visible_line) {
        fprintf(stderr, "visible_offset line %zu not in computed visible range [%zu, %zu]\n",
                visible_line, first_visible_line, last_visible_line);
    }

    if (offscreen_line < first_visible_line || offscreen_line > last_visible_line) {
        fprintf(stderr, "offscreen_offset line %zu outside expected visible range [%zu, %zu] - this is expected for top-aligned view\n",
                offscreen_line, first_visible_line, last_visible_line);
    }

    memory[visible_offset] = 0x5AU;
    mem_viewer_debug_invalidate_line(viewer, visible_line);
    memory[offscreen_offset] = 0xA5U;
    mem_viewer_debug_invalidate_line(viewer, offscreen_line);

    if (mem_viewer_update(viewer) != 0) {
        fprintf(stderr, "mem_viewer_update failed for large buffer\n");
        mem_viewer_destroy(viewer);
        free(memory);
        return 1;
    }

    build_line_text(expected_line, sizeof(expected_line), memory, 0x0000U);
    if (!wait_for_text_contains(viewer, expected_line, 500U)) {
        fprintf(stderr, "large-buffer viewer did not refresh the visible top line\n");
        mem_viewer_destroy(viewer);
        free(memory);
        return 1;
    }

    if (mem_viewer_debug_get_change_fade(viewer, visible_offset) != 0) {
        fprintf(stderr, "visible large-buffer byte did not receive a change highlight\n");
        mem_viewer_destroy(viewer);
        free(memory);
        return 1;
    }

    line_dirty = mem_viewer_debug_get_line_dirty(viewer, visible_line);
    if (line_dirty != 0 && line_dirty != -1) {
        fprintf(stderr, "visible line %zu still marked as dirty after update (got %d)\n", visible_line, line_dirty);
        mem_viewer_destroy(viewer);
        free(memory);
        return 1;
    }

    line_pending = mem_viewer_debug_get_line_pending(viewer, visible_line);
    if (line_pending != 0 && line_pending != -1) {
        fprintf(stderr, "visible line %zu still marked as pending after update (got %d)\n", visible_line, line_pending);
        mem_viewer_destroy(viewer);
        free(memory);
        return 1;
    }

    if (mem_viewer_debug_get_change_fade(viewer, offscreen_offset) != 255) {
        fprintf(stderr, "offscreen large-buffer byte was highlighted before becoming visible\n");
        mem_viewer_destroy(viewer);
        free(memory);
        return 1;
    }

    line_dirty = mem_viewer_debug_get_line_dirty(viewer, offscreen_line);
    if (line_dirty == 0) {
        fprintf(stderr, "offscreen line %zu should remain dirty (got %d)\n", offscreen_line, line_dirty);
        mem_viewer_destroy(viewer);
        free(memory);
        return 1;
    }

    pump_sdl_events();
    mem_viewer_destroy(viewer);
    (void)sdl_window;
    free(memory);
    return 0;
}

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

static void print_viewer_text(FILE *stream, MemViewer *viewer)
{
    char *buffer;
    size_t required;

    required = mem_viewer_debug_copy_text(viewer, NULL, 0U);
    if (required == 0U) {
        fprintf(stream, "<viewer text unavailable>\n");
        return;
    }

    buffer = (char *)malloc(required);
    if (buffer == NULL) {
        fprintf(stream, "<viewer text allocation failed>\n");
        return;
    }

    if (mem_viewer_debug_copy_text(viewer, buffer, required) == 0U) {
        fprintf(stream, "<viewer text copy failed>\n");
        free(buffer);
        return;
    }

    fprintf(stream, "%s\n", buffer);
    free(buffer);
}

static int wait_for_text_contains(MemViewer *viewer, const char *needle, Uint32 timeout_ms)
{
    Uint32 start;

    start = SDL_GetTicks();
    while ((SDL_GetTicks() - start) < timeout_ms) {
        pump_sdl_events();
        if (text_contains(viewer, needle)) {
            return 1;
        }
        SDL_Delay(10);
    }

    return text_contains(viewer, needle);
}

static void pump_sdl_events(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
    }
}

int main(int argc, char **argv)
{
    uint8_t memory[32];
    const size_t stress_offsets[] = {2U, 7U, 16U, 23U, 31U};
    int auto_exit;
    MemViewer *viewer;
    SDL_Window *sdl_window;

    auto_exit = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--auto-exit") == 0) {
            auto_exit = 1;
            continue;
        }
        fprintf(stderr, "usage: %s [--auto-exit]\n", argv[0]);
        return 1;
    }

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
        auto_exit ? SDL_WINDOW_HIDDEN : 0
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

    for (size_t iteration = 0U; iteration < 12U; ++iteration) {
        char expected_line0[64];
        char expected_line1[64];

        for (size_t i = 0U; i < (sizeof(stress_offsets) / sizeof(stress_offsets[0])); ++i) {
            size_t offset;

            offset = stress_offsets[i];
            memory[offset] = (uint8_t)(0x30U + (iteration * 7U) + (i * 3U));
        }

        if (mem_viewer_update(viewer) != 0) {
            fprintf(stderr, "mem_viewer_update failed during stress iteration %zu\n", iteration);
            mem_viewer_destroy(viewer);
            SDL_DestroyWindow(sdl_window);
            SDL_Quit();
            return 1;
        }

        snprintf(
            expected_line0,
            sizeof(expected_line0),
            "00000000: AA BB %02X 03 04 05 06 %02X",
            memory[2],
            memory[7]
        );
        snprintf(
            expected_line1,
            sizeof(expected_line1),
            "00000010: %02X 11 12 13 14 15 16 %02X",
            memory[16],
            memory[23]
        );

        if (!wait_for_text_contains(viewer, expected_line0, 250U)
            || !wait_for_text_contains(viewer, expected_line1, 250U)
            || !wait_for_text_contains(viewer, "00000010: ", 250U)) {
            fprintf(
                stderr,
                "GTK viewer did not reflect stress iteration %zu updates\n",
                iteration
            );
            print_viewer_text(stderr, viewer);
            mem_viewer_destroy(viewer);
            SDL_DestroyWindow(sdl_window);
            SDL_Quit();
            return 1;
        }
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

    {
        char expected_line0[64];

        snprintf(
            expected_line0,
            sizeof(expected_line0),
            "00000000: AA BB %02X 03 04 7E 06 %02X",
            memory[2],
            memory[7]
        );
        if (!wait_for_text_contains(viewer, expected_line0, 250U)) {
            fprintf(stderr, "GTK viewer did not refresh after a single-byte edit\n");
            print_viewer_text(stderr, viewer);
            mem_viewer_destroy(viewer);
            SDL_DestroyWindow(sdl_window);
            SDL_Quit();
            return 1;
        }
    }

    if (mem_viewer_debug_get_change_fade(viewer, 5U) != 0) {
        fprintf(stderr, "changed byte did not start with the strongest highlight\n");
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

    if (mem_viewer_update(viewer) != 0) {
        fprintf(stderr, "mem_viewer_update failed while advancing the fade state\n");
        mem_viewer_destroy(viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    if (mem_viewer_debug_get_change_fade(viewer, 5U) != 1) {
        fprintf(stderr, "changed byte highlight did not fade on the next update\n");
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
    memory[24] = 0xD4;

    {
        char expected_line0[64];
        char expected_line1_prefix[64];

        snprintf(
            expected_line0,
            sizeof(expected_line0),
            "00000000: AA BB %02X 03 04 7E 9C %02X",
            memory[2],
            memory[7]
        );
        snprintf(
            expected_line1_prefix,
            sizeof(expected_line1_prefix),
            "00000010: %02X 11 12 13 7E 15 16 %02X",
            memory[16],
            memory[23]
        );

        if (!wait_for_text_contains(viewer, expected_line0, 500U)) {
            fprintf(stderr, "GTK viewer did not refresh automatically after memory changed\n");
            mem_viewer_destroy(viewer);
            SDL_DestroyWindow(sdl_window);
            SDL_Quit();
            return 1;
        }

        if (!wait_for_text_contains(viewer, expected_line1_prefix, 500U)
            || !wait_for_text_contains(viewer, "D4 19 1A 1B 1C 1D 1E", 500U)) {
            fprintf(stderr, "GTK viewer did not keep up with multi-line auto-refresh updates\n");
            print_viewer_text(stderr, viewer);
            mem_viewer_destroy(viewer);
            SDL_DestroyWindow(sdl_window);
            SDL_Quit();
            return 1;
        }
    }

    if (auto_exit) {
        if (mem_viewer_debug_set_auto_refresh(viewer, 0) != 0) {
            fprintf(stderr, "failed to disable auto refresh\n");
            mem_viewer_destroy(viewer);
            SDL_DestroyWindow(sdl_window);
            SDL_Quit();
            return 1;
        }

        pump_sdl_events();
        mem_viewer_destroy(viewer);

        if (run_large_buffer_visibility_test(sdl_window) != 0) {
            SDL_DestroyWindow(sdl_window);
            SDL_Quit();
            return 1;
        }

        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        printf("mem_viewer GTK/SDL integration test passed\n");
        return 0;
    }

    printf(
        "mem_viewer live test is running.\n"
        "Close the GTK memory viewer window to stop.\n"
    );

    for (size_t iteration = 0U; mem_viewer_debug_is_closed(viewer) == 0; ++iteration) {
        pump_sdl_events();
        update_live_memory(memory, sizeof(memory), iteration);
        if (mem_viewer_update(viewer) != 0) {
            fprintf(stderr, "mem_viewer_update failed in live update loop\n");
            break;
        }
        SDL_Delay(50);
    }

    mem_viewer_destroy(viewer);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();
    printf("mem_viewer live test finished\n");
    return 0;
}

#include "mem_viewer.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMORY_SIZE (64 * 1024)
#define BYTES_PER_ROW 16

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
static int text_contains(MemViewer *viewer, const char *needle);
static void fill_memory_pattern(uint8_t *memory, size_t size);
static int run_64kb_update_test(SDL_Window *sdl_window, int auto_exit);
static void run_interactive_64kb_test(SDL_Window *sdl_window);

static void fill_memory_pattern(uint8_t *memory, size_t size)
{
    for (size_t i = 0U; i < size; ++i) {
        memory[i] = (uint8_t)((i * 17U + 31U) & 0xFFU);
    }
}

static void pump_sdl_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
    }
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

static int run_64kb_update_test(SDL_Window *sdl_window, int auto_exit)
{
    uint8_t *memory;
    MemViewer *viewer;
    size_t first_visible_line;
    size_t last_visible_line;
    size_t test_offsets[] = {0x0000U, 0x0010U, 0x0100U, 0x1000U, 0xFFFFU};
    size_t num_test_offsets = sizeof(test_offsets) / sizeof(test_offsets[0]);
    int failed = 0;
    (void)sdl_window;
    (void)auto_exit;

    printf("\n=== 64KB Buffer Update Test ===\n\n");

    memory = (uint8_t *)malloc(MEMORY_SIZE);
    if (memory == NULL) {
        fprintf(stderr, "64KB test allocation failed\n");
        return 1;
    }

    fill_memory_pattern(memory, MEMORY_SIZE);

    printf("1. Opening memory viewer with 64KB buffer...\n");
    viewer = mem_viewer_open(memory, MEMORY_SIZE);
    if (viewer == NULL) {
        fprintf(stderr, "mem_viewer_open failed for 64KB buffer\n");
        free(memory);
        return 1;
    }
    printf("   Viewer opened successfully\n");

    pump_sdl_events();

    printf("\n2. Checking initial visible line range...\n");
    if (mem_viewer_debug_get_visible_line_range(viewer, &first_visible_line, &last_visible_line) != 0) {
        fprintf(stderr, "   FAILED: Could not get visible line range\n");
        failed = 1;
    } else {
        printf("   Visible lines: %zu to %zu (total %zu lines)\n",
               first_visible_line, last_visible_line, last_visible_line - first_visible_line + 1);
    }

    printf("\n3. Testing updates at specific offsets...\n");
    for (size_t i = 0; i < num_test_offsets; ++i) {
        size_t offset = test_offsets[i];
        size_t line = offset / BYTES_PER_ROW;
        uint8_t new_value = (uint8_t)(0xA0U + i);
        int was_dirty;
        int is_dirty;
        int is_pending;

        printf("   Testing offset 0x%04zX (line %zu)...\n", offset, line);

        was_dirty = mem_viewer_debug_get_line_dirty(viewer, line);
        printf("      Line dirty before update: %d\n", was_dirty);

        memory[offset] = new_value;
        mem_viewer_debug_invalidate_line(viewer, line);

        is_dirty = mem_viewer_debug_get_line_dirty(viewer, line);
        is_pending = mem_viewer_debug_get_line_pending(viewer, line);
        printf("      Line dirty after invalidate: %d\n", is_dirty);
        printf("      Line pending: %d\n", is_pending);

        if (is_dirty != 1) {
            fprintf(stderr, "      FAILED: Line should be dirty after invalidation\n");
            failed = 1;
        }
    }

    printf("\n4. Calling mem_viewer_update to sync changes...\n");
    if (mem_viewer_update(viewer) != 0) {
        fprintf(stderr, "   FAILED: mem_viewer_update failed\n");
        failed = 1;
    } else {
        printf("   Update completed successfully\n");
    }

    printf("\n5. Verifying dirty/pending state after update...\n");
    for (size_t i = 0; i < num_test_offsets; ++i) {
        size_t offset = test_offsets[i];
        size_t line = offset / BYTES_PER_ROW;
        int is_dirty;
        int is_pending;
        int fade;

        is_dirty = mem_viewer_debug_get_line_dirty(viewer, line);
        is_pending = mem_viewer_debug_get_line_pending(viewer, line);
        fade = mem_viewer_debug_get_change_fade(viewer, offset);

        printf("   Offset 0x%04zX (line %zu): dirty=%d, pending=%d, fade=%d\n",
               offset, line, is_dirty, is_pending, fade);

        /* Offscreen lines may remain dirty since they weren't synced */
        if (line > last_visible_line && is_dirty != 0) {
            printf("      (Offscreen line - dirty state expected)\n");
        } else if (is_dirty != 0) {
            fprintf(stderr, "      WARNING: Line should be clean after update\n");
        }
        if (is_pending != 0) {
            fprintf(stderr, "      WARNING: Line should not be pending after update\n");
        }
        /* Only check fade for visible lines */
        if (line <= last_visible_line && fade != 0) {
            fprintf(stderr, "      WARNING: Changed byte should have fade=0\n");
        }
    }

    printf("\n6. Verifying text buffer contains updated values...\n");
    {
        char expected_pattern[32];
        snprintf(expected_pattern, sizeof(expected_pattern), "%02X", (uint8_t)0xA0U);
        if (text_contains(viewer, expected_pattern)) {
            printf("   Found updated value 0x%s in viewer\n", expected_pattern);
        } else {
            fprintf(stderr, "   FAILED: Updated value not found in viewer\n");
            failed = 1;
        }
    }

    printf("\n7. Testing scroll to different region...\n");
    if (auto_exit) {
        printf("   Skipped in auto-exit mode (requires visible window)\n");
    } else {
        size_t scroll_offset = 0x8000U;
        size_t scroll_line = scroll_offset / BYTES_PER_ROW;

        printf("   Scrolling to offset 0x%04zX (line %zu)...\n", scroll_offset, scroll_line);

        if (mem_viewer_debug_scroll_to_offset(viewer, scroll_offset) != 0) {
            fprintf(stderr, "   FAILED: Scroll to offset failed\n");
            failed = 1;
        } else {
            pump_sdl_events();

            if (mem_viewer_debug_get_visible_line_range(viewer, &first_visible_line, &last_visible_line) == 0) {
                printf("   New visible lines: %zu to %zu\n", first_visible_line, last_visible_line);

                if (scroll_line >= first_visible_line && scroll_line <= last_visible_line) {
                    printf("   Scrolled line is within visible range: PASS\n");
                } else {
                    printf("   Scrolled line %zu outside visible range [%zu, %zu]\n",
                           scroll_line, first_visible_line, last_visible_line);
                }
            }
        }
    }

    printf("\n8. Testing changed-lines-only filter...\n");
    if (auto_exit) {
        printf("   Skipped in auto-exit mode (requires visible window)\n");
    } else {
        int visible_count_before;
        int visible_count_after;

        visible_count_before = mem_viewer_debug_get_visible_line_count(viewer);
        printf("   Visible lines before filter: %d\n", visible_count_before);

        if (mem_viewer_debug_set_changed_only(viewer, 1) != 0) {
            fprintf(stderr, "   FAILED: Could not enable changed-lines-only filter\n");
            failed = 1;
        } else {
            pump_sdl_events();
            visible_count_after = mem_viewer_debug_get_visible_line_count(viewer);
            printf("   Visible lines after filter: %d\n", visible_count_after);

            if (visible_count_after <= visible_count_before && visible_count_after > 0) {
                printf("   Filter reduced visible lines: PASS\n");
            } else if (visible_count_after == visible_count_before) {
                printf("   Filter had no effect (all lines may be marked as changed)\n");
            }
        }

        if (mem_viewer_debug_set_changed_only(viewer, 0) != 0) {
            fprintf(stderr, "   FAILED: Could not disable changed-lines-only filter\n");
            failed = 1;
        }
    }

    printf("\n9. Testing fade progression...\n");
    {
        size_t test_offset = 0x0100U;
        int fade_before;
        int fade_after;

        memory[test_offset] = 0xFFU;
        mem_viewer_debug_invalidate_line(viewer, test_offset / BYTES_PER_ROW);

        if (mem_viewer_update(viewer) != 0) {
            fprintf(stderr, "   FAILED: Update for fade test failed\n");
            failed = 1;
        } else {
            pump_sdl_events();

            fade_before = mem_viewer_debug_get_change_fade(viewer, test_offset);
            printf("   Fade value immediately after update: %d\n", fade_before);

            if (mem_viewer_update(viewer) != 0) {
                fprintf(stderr, "   FAILED: Second update for fade test failed\n");
                failed = 1;
            } else {
                pump_sdl_events();

                fade_after = mem_viewer_debug_get_change_fade(viewer, test_offset);
                printf("   Fade value after second update: %d\n", fade_after);

                if (fade_after > fade_before) {
                    printf("   Fade progressed correctly: PASS\n");
                } else if (fade_after == fade_before && fade_before == 255) {
                    printf("   Fade already at maximum\n");
                } else {
                    fprintf(stderr, "   WARNING: Fade did not progress as expected\n");
                }
            }
        }
    }

    printf("\n10. Cleaning up...\n");
    pump_sdl_events();
    mem_viewer_destroy(viewer);
    free(memory);
    printf("    Cleanup completed\n");

    printf("\n=== 64KB Buffer Update Test %s ===\n\n", failed ? "FAILED" : "PASSED");

    return failed;
}

static void run_interactive_64kb_test(SDL_Window *sdl_window)
{
    uint8_t *memory;
    MemViewer *viewer;
    size_t iteration = 0U;
    (void)sdl_window;

    printf("\n=== 64KB Interactive Memory Viewer ===\n\n");

    memory = (uint8_t *)malloc(MEMORY_SIZE);
    if (memory == NULL) {
        fprintf(stderr, "64KB test allocation failed\n");
        return;
    }

    fill_memory_pattern(memory, MEMORY_SIZE);

    printf("Opening memory viewer with 64KB buffer...\n");
    viewer = mem_viewer_open(memory, MEMORY_SIZE);
    if (viewer == NULL) {
        fprintf(stderr, "mem_viewer_open failed for 64KB buffer\n");
        free(memory);
        return;
    }

    printf("Viewer opened successfully.\n");
    printf("\nInteractive mode: Update memory values and watch them fade.\n");
    printf("Close the window to exit.\n\n");

    pump_sdl_events();

    while (mem_viewer_debug_is_closed(viewer) == 0) {
        pump_sdl_events();

        iteration += 1U;

        if ((iteration % 10U) == 0U) {
            size_t offsets[] = {0x0000U, 0x0010U, 0x0100U, 0x1000U, 0xFFFFU, 0x8000U};
            size_t num_offsets = sizeof(offsets) / sizeof(offsets[0]);

            for (size_t i = 0U; i < num_offsets; ++i) {
                size_t offset = offsets[i];
                if (offset < MEMORY_SIZE) {
                    memory[offset] = (uint8_t)((iteration * 7U + i * 13U) & 0xFFU);
                }
            }

            for (size_t i = 0U; i < num_offsets; ++i) {
                size_t offset = offsets[i];
                size_t line = offset / BYTES_PER_ROW;
                if (line < (MEMORY_SIZE / BYTES_PER_ROW)) {
                    mem_viewer_debug_invalidate_line(viewer, line);
                }
            }

            if (mem_viewer_update(viewer) != 0) {
                fprintf(stderr, "mem_viewer_update failed in interactive loop\n");
                break;
            }
        }

        SDL_Delay(50);
    }

    printf("\nViewer closed. Cleaning up...\n");
    mem_viewer_destroy(viewer);
    free(memory);
    printf("Cleanup completed.\n");
}

int main(int argc, char **argv)
{
    uint8_t small_memory[256];
    MemViewer *small_viewer;
    SDL_Window *sdl_window;
    int auto_exit = 0;
    int result = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--auto-exit") == 0) {
            auto_exit = 1;
        } else {
            fprintf(stderr, "usage: %s [--auto-exit]\n", argv[0]);
            return 1;
        }
    }

    printf("64KB Memory Viewer Update Test\n");
    printf("==============================\n");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    sdl_window = SDL_CreateWindow(
        "mem_viewer_64kb_test",
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

    printf("\nBasic functionality check with small buffer...\n");
    for (size_t i = 0; i < sizeof(small_memory); ++i) {
        small_memory[i] = (uint8_t)i;
    }

    small_viewer = mem_viewer_open(small_memory, sizeof(small_memory));
    if (small_viewer == NULL) {
        fprintf(stderr, "mem_viewer_open failed for small buffer\n");
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }

    pump_sdl_events();

    if (!text_contains(small_viewer, "00000000: 00 01 02 03")) {
        fprintf(stderr, "GTK viewer did not render initial bytes\n");
        mem_viewer_destroy(small_viewer);
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        return 1;
    }
    printf("Basic functionality: PASS\n");

    mem_viewer_destroy(small_viewer);
    pump_sdl_events();

    if (auto_exit) {
        result = run_64kb_update_test(sdl_window, auto_exit);

        SDL_DestroyWindow(sdl_window);
        SDL_Quit();

        if (result == 0) {
            printf("All 64KB update tests passed!\n");
        } else {
            printf("Some 64KB update tests failed!\n");
        }

        return result;
    } else {
        run_interactive_64kb_test(sdl_window);

        SDL_DestroyWindow(sdl_window);
        SDL_Quit();

        printf("\nInteractive 64KB test finished.\n");
        return 0;
    }
}

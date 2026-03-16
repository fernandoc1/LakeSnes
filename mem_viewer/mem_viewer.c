#include "mem_viewer.h"

#include <SDL.h>

#include <stdint.h>
#include <stdlib.h>

#define MEM_VIEWER_BYTES_PER_ROW 16
#define MEM_VIEWER_PADDING 12
#define MEM_VIEWER_GLYPH_W 5
#define MEM_VIEWER_GLYPH_H 7
#define MEM_VIEWER_ADDRESS_DIGITS 8

struct MemViewer {
    const uint8_t *memory;
    size_t size;
    SDL_Window *window;
    SDL_Surface *surface;
    SDL_mutex *lock;
    Uint32 window_id;
    int window_width;
    int window_height;
    int rows_visible;
    int font_scale;
    int cell_w;
    int cell_h;
    size_t first_row;
};

typedef struct {
    char ch;
    uint8_t rows[MEM_VIEWER_GLYPH_H];
} Glyph;

static int g_sdl_video_users = 0;
static SDL_mutex *g_manager_lock = NULL;
static SDL_cond *g_manager_cond = NULL;
static SDL_Thread *g_manager_thread = NULL;
static MemViewer **g_viewers = NULL;
static size_t g_viewer_count = 0;
static size_t g_viewer_capacity = 0;
static int g_manager_running = 0;
static int g_manager_stop_requested = 0;

static int mem_viewer_ensure_video(void);
static void mem_viewer_release_video(void);
static int mem_viewer_ensure_manager_primitives(void);
static int mem_viewer_ensure_manager_thread_locked(void);
static int mem_viewer_register_viewer_locked(MemViewer *viewer);
static void mem_viewer_unregister_viewer_locked(MemViewer *viewer);
static int mem_viewer_manager_main(void *userdata);
static void mem_viewer_draw(MemViewer *viewer);

static const Glyph g_glyphs[] = {
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E } },
    { '6', { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E } },
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { ':', { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 } },
};

static void mem_viewer_close_window(MemViewer *viewer)
{
    if (viewer == NULL || viewer->window == NULL) {
        return;
    }

    SDL_DestroyWindow(viewer->window);
    viewer->window = NULL;
    viewer->surface = NULL;
    viewer->window_id = 0;
    mem_viewer_release_video();
}

static const Glyph *mem_viewer_find_glyph(char ch)
{
    size_t i;

    for (i = 0; i < sizeof(g_glyphs) / sizeof(g_glyphs[0]); ++i) {
        if (g_glyphs[i].ch == ch) {
            return &g_glyphs[i];
        }
    }

    return NULL;
}

static int mem_viewer_ensure_video(void)
{
    if (g_sdl_video_users == 0) {
        if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
            if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
                return -1;
            }
        }
    }

    ++g_sdl_video_users;
    return 0;
}

static void mem_viewer_release_video(void)
{
    if (g_sdl_video_users <= 0) {
        return;
    }

    --g_sdl_video_users;
    if (g_sdl_video_users == 0) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

static int mem_viewer_ensure_manager_primitives(void)
{
    if (g_manager_lock == NULL) {
        g_manager_lock = SDL_CreateMutex();
        if (g_manager_lock == NULL) {
            return -1;
        }
    }

    if (g_manager_cond == NULL) {
        g_manager_cond = SDL_CreateCond();
        if (g_manager_cond == NULL) {
            return -1;
        }
    }

    return 0;
}

static int mem_viewer_ensure_manager_thread_locked(void)
{
    SDL_Thread *thread;

    while (g_manager_running && g_manager_stop_requested) {
        SDL_CondWait(g_manager_cond, g_manager_lock);
    }

    if (g_manager_running) {
        return 0;
    }

    g_manager_stop_requested = 0;
    thread = SDL_CreateThread(mem_viewer_manager_main, "mem-viewer-manager", NULL);
    if (thread == NULL) {
        return -1;
    }

    g_manager_thread = thread;
    g_manager_running = 1;
    return 0;
}

static int mem_viewer_register_viewer_locked(MemViewer *viewer)
{
    MemViewer **new_viewers;
    size_t new_capacity;

    if (g_viewer_count == g_viewer_capacity) {
        new_capacity = g_viewer_capacity == 0 ? 4 : g_viewer_capacity * 2;
        new_viewers = (MemViewer **)realloc(g_viewers, new_capacity * sizeof(*new_viewers));
        if (new_viewers == NULL) {
            return -1;
        }

        g_viewers = new_viewers;
        g_viewer_capacity = new_capacity;
    }

    g_viewers[g_viewer_count] = viewer;
    ++g_viewer_count;
    return 0;
}

static void mem_viewer_unregister_viewer_locked(MemViewer *viewer)
{
    size_t i;

    for (i = 0; i < g_viewer_count; ++i) {
        if (g_viewers[i] != viewer) {
            continue;
        }

        for (; i + 1 < g_viewer_count; ++i) {
            g_viewers[i] = g_viewers[i + 1];
        }
        --g_viewer_count;
        return;
    }
}

static int mem_viewer_total_cell_w(const MemViewer *viewer)
{
    return viewer->cell_w;
}

static int mem_viewer_total_cell_h(const MemViewer *viewer)
{
    return viewer->cell_h;
}

static void mem_viewer_put_pixel(SDL_Surface *surface, int x, int y, uint32_t color)
{
    uint8_t *row;
    uint32_t *pixel;

    if (x < 0 || y < 0 || x >= surface->w || y >= surface->h) {
        return;
    }

    row = (uint8_t *)surface->pixels + (y * surface->pitch);
    pixel = (uint32_t *)(row + (x * 4));
    *pixel = color;
}

static void mem_viewer_fill_rect(SDL_Surface *surface, int x, int y, int w, int h, uint32_t color)
{
    int yy;
    int xx;

    for (yy = 0; yy < h; ++yy) {
        for (xx = 0; xx < w; ++xx) {
            mem_viewer_put_pixel(surface, x + xx, y + yy, color);
        }
    }
}

static void mem_viewer_draw_glyph(
    SDL_Surface *surface,
    const MemViewer *viewer,
    int x,
    int y,
    char ch,
    uint32_t color
)
{
    const Glyph *glyph;
    int gy;
    int gx;

    glyph = mem_viewer_find_glyph(ch);
    if (glyph == NULL) {
        return;
    }

    for (gy = 0; gy < MEM_VIEWER_GLYPH_H; ++gy) {
        for (gx = 0; gx < MEM_VIEWER_GLYPH_W; ++gx) {
            if ((glyph->rows[gy] >> (MEM_VIEWER_GLYPH_W - 1 - gx)) & 1) {
                mem_viewer_fill_rect(
                    surface,
                    x + (gx * viewer->font_scale),
                    y + (gy * viewer->font_scale),
                    viewer->font_scale,
                    viewer->font_scale,
                    color
                );
            }
        }
    }
}

static void mem_viewer_draw_text(
    SDL_Surface *surface,
    const MemViewer *viewer,
    int x,
    int y,
    const char *text,
    uint32_t color
)
{
    int cursor_x;

    cursor_x = x;
    while (*text != '\0') {
        if (*text != ' ') {
            mem_viewer_draw_glyph(surface, viewer, cursor_x, y, *text, color);
        }
        cursor_x += mem_viewer_total_cell_w(viewer);
        ++text;
    }
}

static void mem_viewer_format_hex32(uint32_t value, char out[MEM_VIEWER_ADDRESS_DIGITS + 1])
{
    static const char digits[] = "0123456789ABCDEF";
    int i;

    for (i = MEM_VIEWER_ADDRESS_DIGITS - 1; i >= 0; --i) {
        out[i] = digits[value & 0x0F];
        value >>= 4;
    }
    out[MEM_VIEWER_ADDRESS_DIGITS] = '\0';
}

static void mem_viewer_format_hex8(uint8_t value, char out[3])
{
    static const char digits[] = "0123456789ABCDEF";

    out[0] = digits[(value >> 4) & 0x0F];
    out[1] = digits[value & 0x0F];
    out[2] = '\0';
}

static size_t mem_viewer_total_rows(const MemViewer *viewer)
{
    return (viewer->size + MEM_VIEWER_BYTES_PER_ROW - 1) / MEM_VIEWER_BYTES_PER_ROW;
}

static void mem_viewer_clamp_scroll(MemViewer *viewer)
{
    size_t total_rows;
    size_t max_first_row;

    total_rows = mem_viewer_total_rows(viewer);
    max_first_row = 0;
    if (total_rows > (size_t)viewer->rows_visible) {
        max_first_row = total_rows - (size_t)viewer->rows_visible;
    }
    if (viewer->first_row > max_first_row) {
        viewer->first_row = max_first_row;
    }
}

static void mem_viewer_refresh_layout(MemViewer *viewer)
{
    SDL_GetWindowSize(viewer->window, &viewer->window_width, &viewer->window_height);
    viewer->surface = SDL_GetWindowSurface(viewer->window);
    viewer->rows_visible = (viewer->window_height - (MEM_VIEWER_PADDING * 2)) / mem_viewer_total_cell_h(viewer);
    if (viewer->rows_visible < 1) {
        viewer->rows_visible = 1;
    }
    mem_viewer_clamp_scroll(viewer);
}

static int mem_viewer_prepare_window(MemViewer *viewer)
{
    viewer->window = SDL_CreateWindow(
        "Memory Viewer",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        760,
        420,
        SDL_WINDOW_RESIZABLE
    );
    if (viewer->window == NULL) {
        return -1;
    }

    viewer->window_id = SDL_GetWindowID(viewer->window);
    SDL_ShowWindow(viewer->window);
    mem_viewer_refresh_layout(viewer);
    if (viewer->surface == NULL) {
        mem_viewer_close_window(viewer);
        return -1;
    }

    mem_viewer_draw(viewer);
    return 0;
}

static void mem_viewer_scroll(MemViewer *viewer, int delta_rows)
{
    size_t total_rows;
    size_t max_first_row;
    long next_row;

    total_rows = mem_viewer_total_rows(viewer);
    max_first_row = 0;
    if (total_rows > (size_t)viewer->rows_visible) {
        max_first_row = total_rows - (size_t)viewer->rows_visible;
    }

    next_row = (long)viewer->first_row + delta_rows;
    if (next_row < 0) {
        next_row = 0;
    }
    if ((size_t)next_row > max_first_row) {
        next_row = (long)max_first_row;
    }

    viewer->first_row = (size_t)next_row;
}

static void mem_viewer_draw(MemViewer *viewer)
{
    uint32_t bg;
    uint32_t fg;
    int row;
    size_t row_index;
    int y;

    if (viewer->surface == NULL) {
        return;
    }

    bg = SDL_MapRGB(viewer->surface->format, 10, 12, 16);
    fg = SDL_MapRGB(viewer->surface->format, 120, 255, 160);

    SDL_LockMutex(viewer->lock);
    SDL_LockSurface(viewer->surface);
    SDL_FillRect(viewer->surface, NULL, bg);

    for (row = 0; row < viewer->rows_visible; ++row) {
        char address_text[MEM_VIEWER_ADDRESS_DIGITS + 1];
        size_t base;
        int x;
        int col;

        row_index = viewer->first_row + (size_t)row;
        base = row_index * MEM_VIEWER_BYTES_PER_ROW;
        if (base >= viewer->size) {
            break;
        }

        y = MEM_VIEWER_PADDING + (row * mem_viewer_total_cell_h(viewer));
        mem_viewer_format_hex32((uint32_t)base, address_text);
        mem_viewer_draw_text(viewer->surface, viewer, MEM_VIEWER_PADDING, y, address_text, fg);
        mem_viewer_draw_text(
            viewer->surface,
            viewer,
            MEM_VIEWER_PADDING + (MEM_VIEWER_ADDRESS_DIGITS * mem_viewer_total_cell_w(viewer)),
            y,
            ":",
            fg
        );

        x = MEM_VIEWER_PADDING + ((MEM_VIEWER_ADDRESS_DIGITS + 1) * mem_viewer_total_cell_w(viewer));
        for (col = 0; col < MEM_VIEWER_BYTES_PER_ROW; ++col) {
            char byte_text[3];
            size_t index;

            index = base + (size_t)col;
            if (index >= viewer->size) {
                break;
            }

            mem_viewer_format_hex8(viewer->memory[index], byte_text);
            mem_viewer_draw_text(viewer->surface, viewer, x, y, byte_text, fg);
            x += mem_viewer_total_cell_w(viewer) * 2;
        }
    }

    SDL_UnlockSurface(viewer->surface);
    SDL_UnlockMutex(viewer->lock);
    SDL_UpdateWindowSurface(viewer->window);
}

static MemViewer *mem_viewer_find_by_window_id_locked(Uint32 window_id)
{
    size_t i;

    for (i = 0; i < g_viewer_count; ++i) {
        if (g_viewers[i] != NULL && g_viewers[i]->window_id == window_id) {
            return g_viewers[i];
        }
    }

    return NULL;
}

static void mem_viewer_handle_event(MemViewer *viewer, const SDL_Event *event)
{
    if (viewer == NULL || event == NULL) {
        return;
    }

    if (event->type == SDL_WINDOWEVENT) {
        if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            mem_viewer_refresh_layout(viewer);
        } else if (event->window.event == SDL_WINDOWEVENT_CLOSE) {
            mem_viewer_close_window(viewer);
        }
        return;
    }

    if (event->type == SDL_MOUSEWHEEL) {
        mem_viewer_scroll(viewer, -event->wheel.y);
        return;
    }

    if (event->type == SDL_KEYDOWN) {
        switch (event->key.keysym.sym) {
        case SDLK_ESCAPE:
            mem_viewer_close_window(viewer);
            break;
        case SDLK_UP:
            mem_viewer_scroll(viewer, -1);
            break;
        case SDLK_DOWN:
            mem_viewer_scroll(viewer, 1);
            break;
        case SDLK_PAGEUP:
            mem_viewer_scroll(viewer, -viewer->rows_visible);
            break;
        case SDLK_PAGEDOWN:
            mem_viewer_scroll(viewer, viewer->rows_visible);
            break;
        case SDLK_HOME:
            viewer->first_row = 0;
            break;
        case SDLK_END:
            mem_viewer_scroll(viewer, (int)mem_viewer_total_rows(viewer));
            break;
        default:
            break;
        }
    }
}

static int mem_viewer_manager_main(void *userdata)
{
    (void)userdata;

    for (;;) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            Uint32 window_id;
            MemViewer *target;
            size_t i;

            window_id = 0;
            switch (event.type) {
            case SDL_WINDOWEVENT:
                window_id = event.window.windowID;
                break;
            case SDL_MOUSEWHEEL:
                window_id = event.wheel.windowID;
                break;
            case SDL_KEYDOWN:
                window_id = event.key.windowID;
                break;
            case SDL_QUIT:
                SDL_LockMutex(g_manager_lock);
                for (i = 0; i < g_viewer_count; ++i) {
                    mem_viewer_close_window(g_viewers[i]);
                }
                SDL_UnlockMutex(g_manager_lock);
                continue;
            default:
                continue;
            }

            SDL_LockMutex(g_manager_lock);
            target = mem_viewer_find_by_window_id_locked(window_id);
            mem_viewer_handle_event(target, &event);
            SDL_UnlockMutex(g_manager_lock);
        }

        SDL_LockMutex(g_manager_lock);
        if (g_manager_stop_requested || g_viewer_count == 0) {
            g_manager_running = 0;
            g_manager_stop_requested = 0;
            g_manager_thread = NULL;
            SDL_CondBroadcast(g_manager_cond);
            SDL_UnlockMutex(g_manager_lock);
            break;
        }

        for (size_t i = 0; i < g_viewer_count; ++i) {
            if (g_viewers[i]->window != NULL) {
                mem_viewer_draw(g_viewers[i]);
            }
        }
        SDL_UnlockMutex(g_manager_lock);

        SDL_Delay(16);
    }

    return 0;
}

MemViewer *mem_viewer_open(const void *memory, size_t size)
{
    MemViewer *viewer;

    if (memory == NULL || size == 0) {
        return NULL;
    }

    if (mem_viewer_ensure_video() != 0) {
        return NULL;
    }

    if (mem_viewer_ensure_manager_primitives() != 0) {
        mem_viewer_release_video();
        return NULL;
    }

    viewer = (MemViewer *)calloc(1, sizeof(*viewer));
    if (viewer == NULL) {
        mem_viewer_release_video();
        return NULL;
    }

    viewer->lock = SDL_CreateMutex();
    if (viewer->lock == NULL) {
        mem_viewer_destroy(viewer);
        return NULL;
    }

    viewer->memory = (const uint8_t *)memory;
    viewer->size = size;
    viewer->font_scale = 2;
    viewer->cell_w = (MEM_VIEWER_GLYPH_W + 4) * viewer->font_scale;
    viewer->cell_h = (MEM_VIEWER_GLYPH_H + 1) * viewer->font_scale;
    viewer->rows_visible = 1;

    if (mem_viewer_prepare_window(viewer) != 0) {
        mem_viewer_destroy(viewer);
        return NULL;
    }

    SDL_LockMutex(g_manager_lock);
    if (mem_viewer_register_viewer_locked(viewer) != 0 ||
        mem_viewer_ensure_manager_thread_locked() != 0) {
        mem_viewer_unregister_viewer_locked(viewer);
        SDL_UnlockMutex(g_manager_lock);
        mem_viewer_destroy(viewer);
        return NULL;
    }
    SDL_UnlockMutex(g_manager_lock);

    return viewer;
}

int mem_viewer_update_memory(MemViewer *viewer, const void *memory, size_t size)
{
    if (viewer == NULL || viewer->window == NULL || memory == NULL || size == 0) {
        return -1;
    }

    SDL_LockMutex(viewer->lock);
    viewer->memory = (const uint8_t *)memory;
    viewer->size = size;
    mem_viewer_clamp_scroll(viewer);
    SDL_UnlockMutex(viewer->lock);
    return 0;
}

void mem_viewer_destroy(MemViewer *viewer)
{
    SDL_Thread *thread_to_join;

    if (viewer == NULL) {
        return;
    }

    thread_to_join = NULL;
    if (g_manager_lock != NULL) {
        SDL_LockMutex(g_manager_lock);
        mem_viewer_unregister_viewer_locked(viewer);
        if (g_viewer_count == 0 && g_manager_running) {
            g_manager_stop_requested = 1;
            thread_to_join = g_manager_thread;
        }
        mem_viewer_close_window(viewer);
        SDL_UnlockMutex(g_manager_lock);
    } else {
        mem_viewer_close_window(viewer);
    }

    if (thread_to_join != NULL) {
        SDL_WaitThread(thread_to_join, NULL);
    }

    if (viewer->lock != NULL) {
        SDL_DestroyMutex(viewer->lock);
        viewer->lock = NULL;
    }

    free(viewer);
}

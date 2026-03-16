#include "mem_viewer.h"

#include <gtk/gtk.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>

#define MEM_VIEWER_BYTES_PER_ROW 16U

typedef int (*MemViewerGtkTask)(void *userdata);

typedef struct {
    MemViewerGtkTask task;
    void *userdata;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int completed;
    int result;
} MemViewerSyncCall;

struct MemViewer {
    uint8_t *memory;
    uint8_t *displayed_memory;
    uint8_t *changed_lines;
    uint8_t *change_fade;
    size_t size;
    size_t line_count;
    pthread_mutex_t lock;
    int lock_initialized;
    int closed;
    int auto_refresh_enabled;
    guint auto_refresh_source_id;
    GtkWidget *window;
    GtkWidget *scroller;
    GtkWidget *text_view;
    GtkWidget *status_label;
    GtkWidget *offset_entry;
    GtkWidget *value_entry;
    GtkWidget *auto_refresh_toggle;
    GtkWidget *search_entry;
    GtkWidget *search_decimal_toggle;
    GtkWidget *search_prev_button;
    GtkWidget *search_next_button;
    GtkWidget *changed_only_toggle;
    GtkTextTag *search_match_tag;
    GtkTextTag *search_current_tag;
    GtkTextTag *hidden_line_tag;
    GtkTextTag *change_tags[255];
    size_t *search_matches;
    size_t search_match_count;
    size_t search_match_index;
};

typedef struct {
    MemViewer *viewer;
    char *buffer;
    size_t buffer_size;
    size_t required_size;
} MemViewerCopyTextArgs;

typedef struct {
    MemViewer *viewer;
    int x;
    int y;
} MemViewerMoveWindowArgs;

typedef struct {
    MemViewer *viewer;
    size_t offset;
} MemViewerScrollArgs;

typedef struct {
    MemViewer *viewer;
    size_t offset;
    uint8_t value;
} MemViewerSetByteArgs;

typedef struct {
    MemViewer *viewer;
    size_t offset;
} MemViewerSelectOffsetArgs;

typedef struct {
    MemViewer *viewer;
    size_t offset;
} MemViewerGetOffsetArgs;

typedef struct {
    MemViewer *viewer;
    int enabled;
} MemViewerAutoRefreshArgs;

typedef struct {
    MemViewer *viewer;
    const char *text;
    int decimal_mode;
} MemViewerSearchArgs;

typedef struct {
    MemViewer *viewer;
    int direction;
} MemViewerSearchStepArgs;

typedef struct {
    MemViewer *viewer;
    int enabled;
} MemViewerChangedOnlyArgs;

typedef struct {
    MemViewer *viewer;
    int visible_line_count;
} MemViewerVisibleLinesArgs;

typedef struct {
    MemViewer *viewer;
    size_t offset;
    int fade_value;
} MemViewerFadeArgs;

typedef struct {
    MemViewer *viewer;
    int is_closed;
} MemViewerClosedArgs;

static pthread_mutex_t g_backend_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_backend_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_backend_thread;
static pthread_once_t g_x11_threads_once = PTHREAD_ONCE_INIT;
static int g_backend_started = 0;
static int g_backend_ready = 0;
static int g_backend_failed = 0;
static int g_backend_refcount = 0;
static GMainLoop *g_backend_loop = NULL;

static gchar *mem_viewer_format_memory(const uint8_t *memory, size_t size);
static int mem_viewer_ensure_backend(void);
static void mem_viewer_release_backend(void);
static int mem_viewer_invoke(MemViewerGtkTask task, void *userdata);
static int mem_viewer_create_window_task(void *userdata);
static int mem_viewer_reload_view_task(void *userdata);
static int mem_viewer_destroy_view_task(void *userdata);
static int mem_viewer_copy_text_task(void *userdata);
static int mem_viewer_move_window_task(void *userdata);
static int mem_viewer_scroll_to_offset_task(void *userdata);
static int mem_viewer_set_byte_task(void *userdata);
static int mem_viewer_select_offset_task(void *userdata);
static int mem_viewer_get_selected_offset_task(void *userdata);
static int mem_viewer_set_auto_refresh_task(void *userdata);
static int mem_viewer_set_search_task(void *userdata);
static int mem_viewer_step_search_task(void *userdata);
static int mem_viewer_set_changed_only_task(void *userdata);
static int mem_viewer_get_visible_lines_task(void *userdata);
static int mem_viewer_get_change_fade_task(void *userdata);
static int mem_viewer_is_closed_task(void *userdata);
static void mem_viewer_select_offset(MemViewer *viewer, size_t offset, int scroll_to_byte);
static void mem_viewer_mark_changed_lines(MemViewer *viewer);
static size_t mem_viewer_estimate_visible_line_count(MemViewer *viewer);
static void mem_viewer_sync_around_line(MemViewer *viewer, size_t center_line);
static int mem_viewer_get_visible_line_range(MemViewer *viewer, size_t *first_line, size_t *last_line);
static void mem_viewer_sync_visible_lines(MemViewer *viewer, size_t first_line, size_t last_line);
static void mem_viewer_sync_buffer_from_memory(MemViewer *viewer);
static void mem_viewer_refresh_search(MemViewer *viewer, int scroll_to_current);
static void mem_viewer_apply_changed_line_filter(MemViewer *viewer);
static GtkTextTag *mem_viewer_get_change_tag(MemViewer *viewer, uint8_t fade);

static void mem_viewer_init_x11_threads(void)
{
    XInitThreads();
}

static gchar *mem_viewer_format_memory(const uint8_t *memory, size_t size)
{
    GString *text;
    size_t offset;

    text = g_string_sized_new(((size + MEM_VIEWER_BYTES_PER_ROW - 1U) / MEM_VIEWER_BYTES_PER_ROW) * 60U + 1U);
    for (offset = 0U; offset < size; offset += MEM_VIEWER_BYTES_PER_ROW) {
        size_t row_bytes;

        row_bytes = size - offset;
        if (row_bytes > MEM_VIEWER_BYTES_PER_ROW) {
            row_bytes = MEM_VIEWER_BYTES_PER_ROW;
        }

        g_string_append_printf(text, "%08zx:", offset);
        for (size_t i = 0U; i < MEM_VIEWER_BYTES_PER_ROW; ++i) {
            if (i < row_bytes) {
                g_string_append_printf(text, " %02X", memory[offset + i]);
            } else {
                g_string_append(text, "   ");
            }
        }
        g_string_append_c(text, '\n');
    }

    return g_string_free(text, FALSE);
}

static int mem_viewer_parse_size_value(const char *text, size_t *value)
{
    char *end;
    unsigned long long parsed;

    parsed = strtoull(text, &end, 16);
    if (text == end || *end != '\0') {
        return -1;
    }

    *value = (size_t)parsed;
    return 0;
}

static int mem_viewer_parse_byte_value(const char *text, uint8_t *value)
{
    char *end;
    unsigned long parsed;

    parsed = strtoul(text, &end, 16);
    if (text == end || *end != '\0' || parsed > 0xFFU) {
        return -1;
    }

    *value = (uint8_t)parsed;
    return 0;
}

static int mem_viewer_parse_search_value(const char *text, int decimal_mode, uint8_t *value)
{
    char *end;
    unsigned long parsed;

    parsed = strtoul(text, &end, decimal_mode ? 10 : 16);
    if (text == end || *end != '\0' || parsed > 0xFFU) {
        return -1;
    }

    *value = (uint8_t)parsed;
    return 0;
}

static void mem_viewer_format_hex8(uint8_t value, char out[2])
{
    static const char digits[] = "0123456789ABCDEF";

    out[0] = digits[(value >> 4) & 0x0F];
    out[1] = digits[value & 0x0F];
}

static GtkTextTag *mem_viewer_get_change_tag(MemViewer *viewer, uint8_t fade)
{
    GtkTextBuffer *buffer;
    char color[8];

    if (fade >= 255U) {
        return NULL;
    }
    if (viewer->change_tags[fade] != NULL) {
        return viewer->change_tags[fade];
    }

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view));
    snprintf(color, sizeof(color), "#%02XFF%02X", fade, fade);
    viewer->change_tags[fade] = gtk_text_buffer_create_tag(
        buffer,
        NULL,
        "foreground", color,
        NULL
    );
    return viewer->change_tags[fade];
}

static void mem_viewer_set_status(MemViewer *viewer, const char *status)
{
    gtk_label_set_text(GTK_LABEL(viewer->status_label), status);
}

static void mem_viewer_clear_search_matches(MemViewer *viewer)
{
    free(viewer->search_matches);
    viewer->search_matches = NULL;
    viewer->search_match_count = 0U;
    viewer->search_match_index = 0U;
}

static void mem_viewer_remove_search_tags(MemViewer *viewer)
{
    GtkTextBuffer *buffer;
    GtkTextIter start;
    GtkTextIter end;

    if (viewer->text_view == NULL || viewer->search_match_tag == NULL || viewer->search_current_tag == NULL) {
        return;
    }

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view));
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gtk_text_buffer_remove_tag(buffer, viewer->search_match_tag, &start, &end);
    gtk_text_buffer_remove_tag(buffer, viewer->search_current_tag, &start, &end);
}

static void mem_viewer_apply_search_tags(MemViewer *viewer)
{
    GtkTextBuffer *buffer;
    size_t i;

    if (viewer->text_view == NULL || viewer->search_match_tag == NULL || viewer->search_current_tag == NULL) {
        return;
    }

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view));
    mem_viewer_remove_search_tags(viewer);

    for (i = 0U; i < viewer->search_match_count; ++i) {
        GtkTextIter start;
        GtkTextIter end;
        int line;
        int line_offset;

        line = (int)(viewer->search_matches[i] / MEM_VIEWER_BYTES_PER_ROW);
        line_offset = 10 + (int)((viewer->search_matches[i] % MEM_VIEWER_BYTES_PER_ROW) * 3U);
        gtk_text_buffer_get_iter_at_line_offset(buffer, &start, line, line_offset);
        end = start;
        gtk_text_iter_forward_chars(&end, 2);
        gtk_text_buffer_apply_tag(buffer, viewer->search_match_tag, &start, &end);
        if (i == viewer->search_match_index) {
            gtk_text_buffer_apply_tag(buffer, viewer->search_current_tag, &start, &end);
        }
    }
}

static void mem_viewer_mark_all_lines_changed(MemViewer *viewer)
{
    if (viewer->changed_lines == NULL) {
        return;
    }

    memset(viewer->changed_lines, 1, viewer->line_count);
}

static void mem_viewer_apply_changed_line_filter(MemViewer *viewer)
{
    GtkTextBuffer *buffer;
    GtkTextIter start;
    GtkTextIter end;
    size_t line;

    if (viewer->text_view == NULL || viewer->hidden_line_tag == NULL) {
        return;
    }

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view));
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gtk_text_buffer_remove_tag(buffer, viewer->hidden_line_tag, &start, &end);

    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(viewer->changed_only_toggle))) {
        return;
    }

    for (line = 0U; line < viewer->line_count; ++line) {
        GtkTextIter line_start;
        GtkTextIter line_end;

        if (viewer->changed_lines[line]) {
            continue;
        }

        gtk_text_buffer_get_iter_at_line(buffer, &line_start, (gint)line);
        line_end = line_start;
        if (!gtk_text_iter_ends_line(&line_end)) {
            gtk_text_iter_forward_to_line_end(&line_end);
        }
        if (!gtk_text_iter_is_end(&line_end)) {
            gtk_text_iter_forward_char(&line_end);
        }
        gtk_text_buffer_apply_tag(buffer, viewer->hidden_line_tag, &line_start, &line_end);
    }
}

static int mem_viewer_row_byte_count(const MemViewer *viewer, int line)
{
    size_t base;
    size_t remaining;

    if (line < 0) {
        return 0;
    }

    base = (size_t)line * MEM_VIEWER_BYTES_PER_ROW;
    if (base >= viewer->size) {
        return 0;
    }

    remaining = viewer->size - base;
    if (remaining > MEM_VIEWER_BYTES_PER_ROW) {
        remaining = MEM_VIEWER_BYTES_PER_ROW;
    }

    return (int)remaining;
}

static int mem_viewer_offset_from_text_position(
    const MemViewer *viewer,
    int line,
    int column,
    size_t *offset_out
)
{
    int row_byte_count;
    int byte_index;
    int relative_column;

    row_byte_count = mem_viewer_row_byte_count(viewer, line);
    if (row_byte_count <= 0) {
        return -1;
    }

    if (column < 10) {
        byte_index = 0;
    } else {
        relative_column = column - 10;
        byte_index = relative_column / 3;
        if ((relative_column % 3) == 2 && byte_index + 1 < row_byte_count) {
            byte_index += 1;
        }
    }

    if (byte_index < 0) {
        byte_index = 0;
    }
    if (byte_index >= row_byte_count) {
        byte_index = row_byte_count - 1;
    }

    *offset_out = ((size_t)line * MEM_VIEWER_BYTES_PER_ROW) + (size_t)byte_index;
    return 0;
}

static int mem_viewer_offset_from_click(
    const MemViewer *viewer,
    GdkWindow *window,
    double x,
    double y,
    size_t *offset_out
)
{
    GtkTextIter iter;
    gint buffer_x;
    gint buffer_y;
    gint trailing;

    if (viewer->text_view == NULL) {
        return -1;
    }

    gtk_text_view_window_to_buffer_coords(
        GTK_TEXT_VIEW(viewer->text_view),
        gtk_text_view_get_window_type(GTK_TEXT_VIEW(viewer->text_view), window),
        (gint)x,
        (gint)y,
        &buffer_x,
        &buffer_y
    );
    gtk_text_view_get_iter_at_position(
        GTK_TEXT_VIEW(viewer->text_view),
        &iter,
        &trailing,
        buffer_x,
        buffer_y
    );

    return mem_viewer_offset_from_text_position(
        viewer,
        gtk_text_iter_get_line(&iter),
        gtk_text_iter_get_line_offset(&iter) + trailing,
        offset_out
    );
}

static void mem_viewer_mark_changed_lines(MemViewer *viewer)
{
    size_t line;

    if (viewer->changed_lines == NULL || viewer->displayed_memory == NULL) {
        return;
    }

    for (line = 0U; line < viewer->line_count; ++line) {
        size_t base;
        size_t row_bytes;

        base = line * MEM_VIEWER_BYTES_PER_ROW;
        row_bytes = viewer->size - base;
        if (row_bytes > MEM_VIEWER_BYTES_PER_ROW) {
            row_bytes = MEM_VIEWER_BYTES_PER_ROW;
        }
        viewer->changed_lines[line] = memcmp(
            viewer->displayed_memory + base,
            viewer->memory + base,
            row_bytes
        ) != 0;
    }
}

static int mem_viewer_get_visible_line_range(MemViewer *viewer, size_t *first_line, size_t *last_line)
{
    GtkTextIter start_iter;
    GtkTextIter end_iter;
    GdkRectangle visible_rect;
    int start_line;
    int end_line;

    if (viewer->text_view == NULL || viewer->line_count == 0U || !gtk_widget_get_realized(viewer->text_view)) {
        return -1;
    }

    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(viewer->text_view), &visible_rect);
    gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(viewer->text_view), &start_iter, visible_rect.y, NULL);
    gtk_text_view_get_line_at_y(
        GTK_TEXT_VIEW(viewer->text_view),
        &end_iter,
        visible_rect.y + MAX(visible_rect.height - 1, 0),
        NULL
    );

    start_line = gtk_text_iter_get_line(&start_iter);
    end_line = gtk_text_iter_get_line(&end_iter);
    if (start_line < 0) {
        start_line = 0;
    }
    if (end_line < start_line) {
        end_line = start_line;
    }
    if ((size_t)end_line >= viewer->line_count) {
        end_line = (int)(viewer->line_count - 1U);
    }

    *first_line = (size_t)start_line;
    *last_line = (size_t)end_line;
    return 0;
}

static size_t mem_viewer_estimate_visible_line_count(MemViewer *viewer)
{
    GtkTextBuffer *buffer;
    GtkTextIter first_iter;
    GtkTextIter second_iter;
    int first_y;
    int second_y;
    int line_height;
    int widget_height;

    if (viewer->text_view == NULL || viewer->line_count == 0U) {
        return 1U;
    }

    widget_height = gtk_widget_get_allocated_height(viewer->text_view);
    if (widget_height <= 0) {
        return 32U;
    }

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view));
    gtk_text_buffer_get_iter_at_line(buffer, &first_iter, 0);
    gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(viewer->text_view), &first_iter, &first_y, &line_height);
    if (viewer->line_count > 1U) {
        gtk_text_buffer_get_iter_at_line(buffer, &second_iter, 1);
        gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(viewer->text_view), &second_iter, &second_y, NULL);
        if (second_y > first_y) {
            line_height = second_y - first_y;
        }
    }

    if (line_height <= 0) {
        return 32U;
    }
    return (size_t)(widget_height / line_height) + 2U;
}

static void mem_viewer_sync_around_line(MemViewer *viewer, size_t center_line)
{
    size_t visible_lines;
    size_t half_window;
    size_t first_line;
    size_t last_line;

    if (viewer->line_count == 0U) {
        return;
    }

    visible_lines = mem_viewer_estimate_visible_line_count(viewer);
    half_window = visible_lines / 2U;
    first_line = center_line > half_window ? center_line - half_window : 0U;
    last_line = first_line + visible_lines;
    if (last_line >= viewer->line_count) {
        last_line = viewer->line_count - 1U;
    }

    mem_viewer_mark_changed_lines(viewer);
    mem_viewer_sync_visible_lines(viewer, first_line, last_line);
    mem_viewer_apply_changed_line_filter(viewer);
    mem_viewer_apply_search_tags(viewer);
}

static void mem_viewer_sync_visible_lines(MemViewer *viewer, size_t first_line, size_t last_line)
{
    GtkTextBuffer *buffer;
    size_t line;

    if (viewer->text_view == NULL || viewer->displayed_memory == NULL || first_line > last_line) {
        return;
    }

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view));
    for (line = first_line; line <= last_line; ++line) {
        size_t base;
        size_t row_bytes;
        size_t i;

        base = line * MEM_VIEWER_BYTES_PER_ROW;
        if (base >= viewer->size) {
            break;
        }

        row_bytes = viewer->size - base;
        if (row_bytes > MEM_VIEWER_BYTES_PER_ROW) {
            row_bytes = MEM_VIEWER_BYTES_PER_ROW;
        }

        for (i = 0U; i < row_bytes; ++i) {
            size_t offset;
            GtkTextIter start;
            GtkTextIter end;
            GtkTextTag *old_tag;
            GtkTextTag *new_tag;
            uint8_t old_fade;
            uint8_t new_fade;
            char byte_text[2];
            int line_offset;

            offset = base + i;
            line_offset = 10 + (int)(i * 3U);
            gtk_text_buffer_get_iter_at_line_offset(buffer, &start, (gint)line, line_offset);
            end = start;
            gtk_text_iter_forward_chars(&end, 2);

            old_fade = viewer->change_fade[offset];
            new_fade = old_fade;
            old_tag = mem_viewer_get_change_tag(viewer, old_fade);
            if (old_tag != NULL) {
                gtk_text_buffer_remove_tag(buffer, old_tag, &start, &end);
            }

            if (viewer->displayed_memory[offset] != viewer->memory[offset]) {
                mem_viewer_format_hex8(viewer->memory[offset], byte_text);
                gtk_text_buffer_delete(buffer, &start, &end);
                gtk_text_buffer_insert(buffer, &start, byte_text, 2);
                viewer->displayed_memory[offset] = viewer->memory[offset];
                new_fade = 0U;
            } else if (old_fade < 255U) {
                new_fade = (uint8_t)(old_fade + 1U);
            }

            viewer->change_fade[offset] = new_fade;
            new_tag = mem_viewer_get_change_tag(viewer, new_fade);
            if (new_tag != NULL) {
                gtk_text_buffer_get_iter_at_line_offset(buffer, &start, (gint)line, line_offset);
                end = start;
                gtk_text_iter_forward_chars(&end, 2);
                gtk_text_buffer_apply_tag(buffer, new_tag, &start, &end);
            }
        }
    }
}

static void mem_viewer_sync_buffer_from_memory(MemViewer *viewer)
{
    size_t first_line;
    size_t last_line;

    if (viewer->text_view == NULL || viewer->displayed_memory == NULL) {
        return;
    }

    mem_viewer_mark_changed_lines(viewer);
    if (mem_viewer_get_visible_line_range(viewer, &first_line, &last_line) == 0) {
        mem_viewer_sync_visible_lines(viewer, first_line, last_line);
    } else {
        mem_viewer_sync_visible_lines(viewer, 0U, viewer->line_count == 0U ? 0U : viewer->line_count - 1U);
    }
    mem_viewer_apply_changed_line_filter(viewer);
    mem_viewer_apply_search_tags(viewer);
}

static void mem_viewer_reload_buffer(MemViewer *viewer)
{
    GtkTextBuffer *buffer;
    gchar *formatted;
    char status[128];

    if (viewer->window == NULL || viewer->text_view == NULL || viewer->status_label == NULL) {
        return;
    }

    formatted = mem_viewer_format_memory(viewer->memory, viewer->size);
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view));
    gtk_text_buffer_set_text(buffer, formatted, -1);
    if (viewer->displayed_memory != NULL) {
        memcpy(viewer->displayed_memory, viewer->memory, viewer->size);
    }
    if (viewer->change_fade != NULL) {
        memset(viewer->change_fade, 255, viewer->size);
    }
    mem_viewer_mark_all_lines_changed(viewer);
    snprintf(status, sizeof(status), "Viewing %zu bytes", viewer->size);
    mem_viewer_set_status(viewer, status);
    mem_viewer_apply_changed_line_filter(viewer);
    mem_viewer_apply_search_tags(viewer);
    g_free(formatted);
}

static void mem_viewer_refresh_search(MemViewer *viewer, int scroll_to_current)
{
    const char *text;
    uint8_t value;
    size_t i;
    size_t match_count;
    size_t *matches;
    int decimal_mode;
    char status[128];

    text = gtk_entry_get_text(GTK_ENTRY(viewer->search_entry));
    decimal_mode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(viewer->search_decimal_toggle));
    mem_viewer_clear_search_matches(viewer);
    mem_viewer_remove_search_tags(viewer);

    if (text == NULL || *text == '\0') {
        mem_viewer_set_status(viewer, "Search cleared");
        return;
    }

    if (mem_viewer_parse_search_value(text, decimal_mode, &value) != 0) {
        mem_viewer_set_status(viewer, decimal_mode ? "Invalid decimal search value" : "Invalid hexadecimal search value");
        return;
    }

    match_count = 0U;
    for (i = 0U; i < viewer->size; ++i) {
        if (viewer->memory[i] == value) {
            match_count += 1U;
        }
    }

    if (match_count == 0U) {
        mem_viewer_set_status(viewer, "No matches found");
        return;
    }

    matches = (size_t *)malloc(match_count * sizeof(*matches));
    if (matches == NULL) {
        mem_viewer_set_status(viewer, "Search allocation failed");
        return;
    }

    match_count = 0U;
    for (i = 0U; i < viewer->size; ++i) {
        if (viewer->memory[i] == value) {
            matches[match_count++] = i;
        }
    }

    viewer->search_matches = matches;
    viewer->search_match_count = match_count;
    viewer->search_match_index = 0U;
    mem_viewer_apply_search_tags(viewer);
    if (scroll_to_current) {
        mem_viewer_select_offset(viewer, viewer->search_matches[0], 1);
    }
    snprintf(status, sizeof(status), "Found %zu matches", viewer->search_match_count);
    mem_viewer_set_status(viewer, status);
}

static gboolean mem_viewer_auto_refresh_tick(gpointer userdata)
{
    MemViewer *viewer;
    size_t offset;
    const char *offset_text;

    viewer = (MemViewer *)userdata;
    pthread_mutex_lock(&viewer->lock);
    if (viewer->closed || !viewer->auto_refresh_enabled || viewer->text_view == NULL) {
        viewer->auto_refresh_source_id = 0U;
        pthread_mutex_unlock(&viewer->lock);
        return G_SOURCE_REMOVE;
    }
    pthread_mutex_unlock(&viewer->lock);

    offset_text = gtk_entry_get_text(GTK_ENTRY(viewer->offset_entry));
    if (mem_viewer_parse_size_value(offset_text, &offset) != 0) {
        offset = 0U;
    }
    mem_viewer_sync_buffer_from_memory(viewer);
    mem_viewer_select_offset(viewer, offset < viewer->size ? offset : 0U, 0);
    return G_SOURCE_CONTINUE;
}

static void mem_viewer_set_auto_refresh_enabled(MemViewer *viewer, int enabled)
{
    viewer->auto_refresh_enabled = enabled != 0;
    if (viewer->auto_refresh_enabled) {
        if (viewer->auto_refresh_source_id == 0U) {
            viewer->auto_refresh_source_id = g_timeout_add(100U, mem_viewer_auto_refresh_tick, viewer);
        }
        mem_viewer_set_status(viewer, "Auto refresh enabled");
        return;
    }

    if (viewer->auto_refresh_source_id != 0U) {
        g_source_remove(viewer->auto_refresh_source_id);
        viewer->auto_refresh_source_id = 0U;
    }
    mem_viewer_set_status(viewer, "Auto refresh disabled");
}

static void mem_viewer_apply_single_byte(MemViewer *viewer, size_t offset, uint8_t value)
{
    char status[128];

    viewer->memory[offset] = value;
    mem_viewer_sync_buffer_from_memory(viewer);
    snprintf(status, sizeof(status), "Updated byte %zu to %02X", offset, value);
    mem_viewer_set_status(viewer, status);
}

static void mem_viewer_select_offset(MemViewer *viewer, size_t offset, int scroll_to_byte)
{
    GtkTextBuffer *buffer;
    GtkTextIter iter;
    int line;
    int line_offset;
    char offset_text[32];
    char value_text[8];

    if (offset >= viewer->size) {
        return;
    }

    line = (int)(offset / MEM_VIEWER_BYTES_PER_ROW);
    line_offset = 10 + (int)((offset % MEM_VIEWER_BYTES_PER_ROW) * 3U);
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view));
    gtk_text_buffer_get_iter_at_line_offset(buffer, &iter, line, line_offset);
    gtk_text_buffer_place_cursor(buffer, &iter);
    if (scroll_to_byte) {
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(viewer->text_view), &iter, 0.0, FALSE, 0.0, 0.0);
        mem_viewer_sync_around_line(viewer, (size_t)line);
    }

    snprintf(offset_text, sizeof(offset_text), "%zX", offset);
    snprintf(value_text, sizeof(value_text), "%02X", viewer->memory[offset]);
    gtk_entry_set_text(GTK_ENTRY(viewer->offset_entry), offset_text);
    gtk_entry_set_text(GTK_ENTRY(viewer->value_entry), value_text);
}

static void mem_viewer_on_reload_clicked(GtkButton *button, gpointer userdata)
{
    (void)button;
    mem_viewer_sync_buffer_from_memory((MemViewer *)userdata);
}

static void mem_viewer_on_auto_refresh_toggled(GtkToggleButton *button, gpointer userdata)
{
    mem_viewer_set_auto_refresh_enabled(
        (MemViewer *)userdata,
        gtk_toggle_button_get_active(button)
    );
}

static void mem_viewer_on_search_changed(GtkEditable *editable, gpointer userdata)
{
    (void)editable;
    mem_viewer_refresh_search((MemViewer *)userdata, 0);
}

static void mem_viewer_on_search_mode_toggled(GtkToggleButton *button, gpointer userdata)
{
    (void)button;
    mem_viewer_refresh_search((MemViewer *)userdata, 0);
}

static void mem_viewer_on_changed_only_toggled(GtkToggleButton *button, gpointer userdata)
{
    MemViewer *viewer;

    (void)button;
    viewer = (MemViewer *)userdata;
    mem_viewer_apply_changed_line_filter(viewer);
    mem_viewer_apply_search_tags(viewer);
}

static void mem_viewer_on_scroll_value_changed(GtkAdjustment *adjustment, gpointer userdata)
{
    MemViewer *viewer;

    (void)adjustment;
    viewer = (MemViewer *)userdata;
    if (viewer->closed || viewer->text_view == NULL) {
        return;
    }
    mem_viewer_sync_buffer_from_memory(viewer);
}

static void mem_viewer_step_search(MemViewer *viewer, int direction)
{
    char status[128];

    if (viewer->search_match_count == 0U) {
        mem_viewer_set_status(viewer, "No search matches");
        return;
    }

    if (direction > 0) {
        viewer->search_match_index = (viewer->search_match_index + 1U) % viewer->search_match_count;
    } else if (viewer->search_match_index == 0U) {
        viewer->search_match_index = viewer->search_match_count - 1U;
    } else {
        viewer->search_match_index -= 1U;
    }

    mem_viewer_apply_search_tags(viewer);
    mem_viewer_select_offset(viewer, viewer->search_matches[viewer->search_match_index], 1);
    snprintf(
        status,
        sizeof(status),
        "Match %zu of %zu",
        viewer->search_match_index + 1U,
        viewer->search_match_count
    );
    mem_viewer_set_status(viewer, status);
}

static void mem_viewer_on_search_next_clicked(GtkButton *button, gpointer userdata)
{
    (void)button;
    mem_viewer_step_search((MemViewer *)userdata, 1);
}

static void mem_viewer_on_search_prev_clicked(GtkButton *button, gpointer userdata)
{
    (void)button;
    mem_viewer_step_search((MemViewer *)userdata, -1);
}

static void mem_viewer_on_set_byte_clicked(GtkButton *button, gpointer userdata)
{
    const char *offset_text;
    const char *value_text;
    uint8_t value;
    size_t offset;
    MemViewer *viewer;

    (void)button;
    viewer = (MemViewer *)userdata;
    offset_text = gtk_entry_get_text(GTK_ENTRY(viewer->offset_entry));
    value_text = gtk_entry_get_text(GTK_ENTRY(viewer->value_entry));

    if (mem_viewer_parse_size_value(offset_text, &offset) != 0 || offset >= viewer->size) {
        mem_viewer_set_status(viewer, "Invalid hex byte offset");
        return;
    }
    if (mem_viewer_parse_byte_value(value_text, &value) != 0) {
        mem_viewer_set_status(viewer, "Invalid byte value, use hex like 7F");
        return;
    }

    mem_viewer_apply_single_byte(viewer, offset, value);
    mem_viewer_select_offset(viewer, offset, 0);
}

static gboolean mem_viewer_on_text_view_button_press(GtkWidget *widget, GdkEventButton *event, gpointer userdata)
{
    size_t offset;
    MemViewer *viewer;

    (void)widget;
    viewer = (MemViewer *)userdata;
    if (event->type != GDK_BUTTON_PRESS || event->button != 1) {
        return FALSE;
    }

    if (mem_viewer_offset_from_click(viewer, event->window, event->x, event->y, &offset) != 0) {
        return FALSE;
    }

    mem_viewer_select_offset(viewer, offset, 0);
    return TRUE;
}

static void mem_viewer_on_window_destroy(GtkWidget *widget, gpointer userdata)
{
    MemViewer *viewer;

    (void)widget;
    viewer = (MemViewer *)userdata;
    pthread_mutex_lock(&viewer->lock);
    viewer->closed = 1;
    if (viewer->auto_refresh_source_id != 0U) {
        g_source_remove(viewer->auto_refresh_source_id);
        viewer->auto_refresh_source_id = 0U;
    }
    viewer->auto_refresh_enabled = 0;
    viewer->window = NULL;
    viewer->scroller = NULL;
    viewer->text_view = NULL;
    viewer->status_label = NULL;
    viewer->offset_entry = NULL;
    viewer->value_entry = NULL;
    viewer->auto_refresh_toggle = NULL;
    viewer->search_entry = NULL;
    viewer->search_decimal_toggle = NULL;
    viewer->search_prev_button = NULL;
    viewer->search_next_button = NULL;
    viewer->changed_only_toggle = NULL;
    viewer->search_match_tag = NULL;
    viewer->search_current_tag = NULL;
    viewer->hidden_line_tag = NULL;
    mem_viewer_clear_search_matches(viewer);
    pthread_mutex_unlock(&viewer->lock);
}

static gboolean mem_viewer_sync_call_main(gpointer userdata)
{
    MemViewerSyncCall *call;

    call = (MemViewerSyncCall *)userdata;
    call->result = call->task(call->userdata);

    pthread_mutex_lock(&call->lock);
    call->completed = 1;
    pthread_cond_signal(&call->cond);
    pthread_mutex_unlock(&call->lock);
    return G_SOURCE_REMOVE;
}

static void *mem_viewer_backend_main(void *userdata)
{
    GMainLoop *loop;

    (void)userdata;

    if (!gtk_init_check(0, NULL)) {
        pthread_mutex_lock(&g_backend_lock);
        g_backend_failed = 1;
        pthread_cond_broadcast(&g_backend_cond);
        pthread_mutex_unlock(&g_backend_lock);
        return NULL;
    }

    loop = g_main_loop_new(NULL, FALSE);

    pthread_mutex_lock(&g_backend_lock);
    g_backend_loop = loop;
    g_backend_ready = 1;
    pthread_cond_broadcast(&g_backend_cond);
    pthread_mutex_unlock(&g_backend_lock);

    g_main_loop_run(loop);

    pthread_mutex_lock(&g_backend_lock);
    g_backend_loop = NULL;
    g_backend_ready = 0;
    pthread_cond_broadcast(&g_backend_cond);
    pthread_mutex_unlock(&g_backend_lock);

    g_main_loop_unref(loop);
    return NULL;
}

static int mem_viewer_ensure_backend(void)
{
    int rc;

    pthread_once(&g_x11_threads_once, mem_viewer_init_x11_threads);

    pthread_mutex_lock(&g_backend_lock);
    if (!g_backend_started) {
        g_backend_failed = 0;
        g_backend_ready = 0;
        rc = pthread_create(&g_backend_thread, NULL, mem_viewer_backend_main, NULL);
        if (rc != 0) {
            pthread_mutex_unlock(&g_backend_lock);
            return -1;
        }
        g_backend_started = 1;
    }

    while (g_backend_started && !g_backend_ready && !g_backend_failed) {
        pthread_cond_wait(&g_backend_cond, &g_backend_lock);
    }

    if (g_backend_failed) {
        pthread_join(g_backend_thread, NULL);
        g_backend_started = 0;
        g_backend_failed = 0;
        pthread_mutex_unlock(&g_backend_lock);
        return -1;
    }

    g_backend_refcount += 1;
    pthread_mutex_unlock(&g_backend_lock);
    return 0;
}

static gboolean mem_viewer_quit_backend(gpointer userdata)
{
    (void)userdata;
    if (g_backend_loop != NULL) {
        g_main_loop_quit(g_backend_loop);
    }
    return G_SOURCE_REMOVE;
}

static void mem_viewer_release_backend(void)
{
    int should_join;
    pthread_t thread;

    should_join = 0;
    pthread_mutex_lock(&g_backend_lock);
    if (g_backend_refcount > 0) {
        g_backend_refcount -= 1;
    }
    if (g_backend_refcount == 0 && g_backend_started) {
        thread = g_backend_thread;
        should_join = 1;
    }
    pthread_mutex_unlock(&g_backend_lock);

    if (!should_join) {
        return;
    }

    if (g_backend_ready) {
        g_main_context_invoke(NULL, mem_viewer_quit_backend, NULL);
    }
    pthread_join(thread, NULL);

    pthread_mutex_lock(&g_backend_lock);
    g_backend_started = 0;
    g_backend_ready = 0;
    g_backend_failed = 0;
    g_backend_loop = NULL;
    pthread_mutex_unlock(&g_backend_lock);
}

static int mem_viewer_invoke(MemViewerGtkTask task, void *userdata)
{
    MemViewerSyncCall call;

    pthread_mutex_lock(&g_backend_lock);
    if (!g_backend_ready || g_backend_loop == NULL) {
        pthread_mutex_unlock(&g_backend_lock);
        return -1;
    }
    pthread_mutex_unlock(&g_backend_lock);

    memset(&call, 0, sizeof(call));
    call.task = task;
    call.userdata = userdata;
    pthread_mutex_init(&call.lock, NULL);
    pthread_cond_init(&call.cond, NULL);

    pthread_mutex_lock(&call.lock);
    g_main_context_invoke(NULL, mem_viewer_sync_call_main, &call);
    while (!call.completed) {
        pthread_cond_wait(&call.cond, &call.lock);
    }
    pthread_mutex_unlock(&call.lock);

    pthread_cond_destroy(&call.cond);
    pthread_mutex_destroy(&call.lock);
    return call.result;
}

static int mem_viewer_create_window_task(void *userdata)
{
    GtkWidget *root_box;
    GtkWidget *left_box;
    GtkWidget *search_box;
    GtkWidget *controls;
    GtkWidget *reload_button;
    GtkWidget *set_byte_button;
    GtkWidget *auto_refresh_button;
    GtkWidget *offset_label;
    GtkWidget *value_label;
    GtkWidget *search_label;
    GtkWidget *changed_only_button;
    GtkCssProvider *css_provider;
    GtkAdjustment *vadjustment;
    MemViewer *viewer;

    viewer = (MemViewer *)userdata;

    viewer->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    viewer->scroller = gtk_scrolled_window_new(NULL, NULL);
    viewer->text_view = gtk_text_view_new();
    viewer->status_label = gtk_label_new("");
    viewer->offset_entry = gtk_entry_new();
    viewer->value_entry = gtk_entry_new();
    viewer->auto_refresh_toggle = gtk_toggle_button_new_with_label("Auto Refresh");
    viewer->search_entry = gtk_entry_new();
    viewer->search_decimal_toggle = gtk_check_button_new_with_label("Decimal");
    viewer->search_prev_button = gtk_button_new_with_label("Previous");
    viewer->search_next_button = gtk_button_new_with_label("Next");
    viewer->changed_only_toggle = gtk_check_button_new_with_label("Changed Lines Only");
    if (viewer->window == NULL || viewer->scroller == NULL || viewer->text_view == NULL ||
        viewer->status_label == NULL || viewer->offset_entry == NULL || viewer->value_entry == NULL ||
        viewer->auto_refresh_toggle == NULL || viewer->search_entry == NULL ||
        viewer->search_decimal_toggle == NULL || viewer->search_prev_button == NULL ||
        viewer->search_next_button == NULL || viewer->changed_only_toggle == NULL) {
        return -1;
    }

    gtk_window_set_title(GTK_WINDOW(viewer->window), "Memory Viewer");
    gtk_window_set_default_size(GTK_WINDOW(viewer->window), 760, 480);

    root_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    search_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    reload_button = gtk_button_new_with_label("Reload");
    set_byte_button = gtk_button_new_with_label("Set Byte");
    auto_refresh_button = viewer->auto_refresh_toggle;
    offset_label = gtk_label_new("Offset");
    value_label = gtk_label_new("Value");
    search_label = gtk_label_new("Search Value");
    viewer->search_match_tag = gtk_text_buffer_create_tag(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view)),
        NULL,
        "background", "#FFF0A8",
        NULL
    );
    viewer->search_current_tag = gtk_text_buffer_create_tag(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view)),
        NULL,
        "background", "#FFB347",
        "weight", PANGO_WEIGHT_BOLD,
        NULL
    );
    viewer->hidden_line_tag = gtk_text_buffer_create_tag(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view)),
        NULL,
        "invisible", TRUE,
        NULL
    );
    changed_only_button = viewer->changed_only_toggle;

    gtk_container_set_border_width(GTK_CONTAINER(root_box), 8);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(viewer->text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(viewer->text_view), GTK_WRAP_NONE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(viewer->text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(viewer->text_view), TRUE);
    css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(
        css_provider,
        "textview, textview text { background-color: #101010; color: #FFFFFF; }",
        -1,
        NULL
    );
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(viewer->text_view),
        GTK_STYLE_PROVIDER(css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(css_provider);
    gtk_entry_set_width_chars(GTK_ENTRY(viewer->offset_entry), 10);
    gtk_entry_set_width_chars(GTK_ENTRY(viewer->value_entry), 4);
    gtk_entry_set_max_length(GTK_ENTRY(viewer->value_entry), 2);
    gtk_entry_set_text(GTK_ENTRY(viewer->offset_entry), "0");
    gtk_entry_set_text(GTK_ENTRY(viewer->value_entry), "00");
    gtk_container_add(GTK_CONTAINER(viewer->scroller), viewer->text_view);
    vadjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(viewer->scroller));
    gtk_box_pack_start(GTK_BOX(controls), reload_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), offset_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), viewer->offset_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), value_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), viewer->value_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), set_byte_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), auto_refresh_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_box), controls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_box), viewer->scroller, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(left_box), viewer->status_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_box), search_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_box), viewer->search_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_box), viewer->search_decimal_toggle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_box), viewer->search_prev_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_box), viewer->search_next_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_box), changed_only_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root_box), left_box, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root_box), search_box, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(viewer->window), root_box);

    g_signal_connect(viewer->window, "destroy", G_CALLBACK(mem_viewer_on_window_destroy), viewer);
    g_signal_connect(reload_button, "clicked", G_CALLBACK(mem_viewer_on_reload_clicked), viewer);
    g_signal_connect(set_byte_button, "clicked", G_CALLBACK(mem_viewer_on_set_byte_clicked), viewer);
    g_signal_connect(auto_refresh_button, "toggled", G_CALLBACK(mem_viewer_on_auto_refresh_toggled), viewer);
    g_signal_connect(viewer->search_entry, "changed", G_CALLBACK(mem_viewer_on_search_changed), viewer);
    g_signal_connect(viewer->search_decimal_toggle, "toggled", G_CALLBACK(mem_viewer_on_search_mode_toggled), viewer);
    g_signal_connect(viewer->search_prev_button, "clicked", G_CALLBACK(mem_viewer_on_search_prev_clicked), viewer);
    g_signal_connect(viewer->search_next_button, "clicked", G_CALLBACK(mem_viewer_on_search_next_clicked), viewer);
    g_signal_connect(changed_only_button, "toggled", G_CALLBACK(mem_viewer_on_changed_only_toggled), viewer);
    g_signal_connect(viewer->text_view, "button-press-event", G_CALLBACK(mem_viewer_on_text_view_button_press), viewer);
    g_signal_connect(vadjustment, "value-changed", G_CALLBACK(mem_viewer_on_scroll_value_changed), viewer);

    mem_viewer_reload_buffer(viewer);
    mem_viewer_select_offset(viewer, 0U, 0);
    gtk_widget_show_all(viewer->window);
    return 0;
}

static int mem_viewer_reload_view_task(void *userdata)
{
    MemViewer *viewer;

    viewer = (MemViewer *)userdata;
    pthread_mutex_lock(&viewer->lock);
    if (viewer->closed || viewer->window == NULL) {
        pthread_mutex_unlock(&viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&viewer->lock);

    mem_viewer_sync_buffer_from_memory(viewer);
    return 0;
}

static int mem_viewer_destroy_view_task(void *userdata)
{
    MemViewer *viewer;

    viewer = (MemViewer *)userdata;
    pthread_mutex_lock(&viewer->lock);
    if (viewer->window == NULL) {
        viewer->closed = 1;
        pthread_mutex_unlock(&viewer->lock);
        return 0;
    }
    pthread_mutex_unlock(&viewer->lock);

    gtk_widget_destroy(viewer->window);
    return 0;
}

static int mem_viewer_copy_text_task(void *userdata)
{
    GtkTextBuffer *buffer;
    GtkTextIter start;
    GtkTextIter end;
    gchar *text;
    MemViewerCopyTextArgs *args;

    args = (MemViewerCopyTextArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->viewer->text_view == NULL) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(args->viewer->text_view));
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    args->required_size = strlen(text) + 1U;
    if (args->buffer != NULL && args->buffer_size > 0U) {
        g_strlcpy(args->buffer, text, args->buffer_size);
    }
    g_free(text);
    return 0;
}

static int mem_viewer_move_window_task(void *userdata)
{
    MemViewerMoveWindowArgs *args;

    args = (MemViewerMoveWindowArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->viewer->window == NULL) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    gtk_window_move(GTK_WINDOW(args->viewer->window), args->x, args->y);
    return 0;
}

static int mem_viewer_scroll_to_offset_task(void *userdata)
{
    GtkTextBuffer *buffer;
    GtkTextIter iter;
    MemViewerScrollArgs *args;
    size_t line;

    args = (MemViewerScrollArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->viewer->text_view == NULL) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    line = args->offset / MEM_VIEWER_BYTES_PER_ROW;
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(args->viewer->text_view));
    gtk_text_buffer_get_iter_at_line(buffer, &iter, (gint)line);
    gtk_text_buffer_place_cursor(buffer, &iter);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(args->viewer->text_view), &iter, 0.0, FALSE, 0.0, 0.0);
    mem_viewer_sync_around_line(args->viewer, line);
    mem_viewer_set_status(args->viewer, "Scrolled to requested memory position");
    return 0;
}

static int mem_viewer_set_byte_task(void *userdata)
{
    MemViewerSetByteArgs *args;

    args = (MemViewerSetByteArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->viewer->text_view == NULL || args->offset >= args->viewer->size) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    mem_viewer_apply_single_byte(args->viewer, args->offset, args->value);
    return 0;
}

static int mem_viewer_select_offset_task(void *userdata)
{
    MemViewerSelectOffsetArgs *args;

    args = (MemViewerSelectOffsetArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->viewer->text_view == NULL || args->offset >= args->viewer->size) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    mem_viewer_select_offset(args->viewer, args->offset, 0);
    return 0;
}

static int mem_viewer_get_selected_offset_task(void *userdata)
{
    const char *text;
    MemViewerGetOffsetArgs *args;

    args = (MemViewerGetOffsetArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->viewer->offset_entry == NULL) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    text = gtk_entry_get_text(GTK_ENTRY(args->viewer->offset_entry));
    if (mem_viewer_parse_size_value(text, &args->offset) != 0) {
        return -1;
    }

    return 0;
}

static int mem_viewer_set_auto_refresh_task(void *userdata)
{
    MemViewerAutoRefreshArgs *args;

    args = (MemViewerAutoRefreshArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->viewer->auto_refresh_toggle == NULL) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(args->viewer->auto_refresh_toggle), args->enabled != 0);
    return 0;
}

static int mem_viewer_set_search_task(void *userdata)
{
    MemViewerSearchArgs *args;

    args = (MemViewerSearchArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->viewer->search_entry == NULL || args->viewer->search_decimal_toggle == NULL) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(args->viewer->search_decimal_toggle), args->decimal_mode != 0);
    gtk_entry_set_text(GTK_ENTRY(args->viewer->search_entry), args->text);
    mem_viewer_refresh_search(args->viewer, 1);
    return 0;
}

static int mem_viewer_step_search_task(void *userdata)
{
    MemViewerSearchStepArgs *args;

    args = (MemViewerSearchStepArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    mem_viewer_step_search(args->viewer, args->direction);
    return 0;
}

static int mem_viewer_set_changed_only_task(void *userdata)
{
    MemViewerChangedOnlyArgs *args;

    args = (MemViewerChangedOnlyArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->viewer->changed_only_toggle == NULL) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(args->viewer->changed_only_toggle), args->enabled != 0);
    return 0;
}

static int mem_viewer_get_visible_lines_task(void *userdata)
{
    GtkTextBuffer *buffer;
    GtkTextIter iter;
    MemViewerVisibleLinesArgs *args;
    size_t line;

    args = (MemViewerVisibleLinesArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->viewer->text_view == NULL) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(args->viewer->text_view));
    args->visible_line_count = 0;
    for (line = 0U; line < args->viewer->line_count; ++line) {
        gtk_text_buffer_get_iter_at_line(buffer, &iter, (gint)line);
        if (!gtk_text_iter_has_tag(&iter, args->viewer->hidden_line_tag)) {
            args->visible_line_count += 1;
        }
    }

    return 0;
}

static int mem_viewer_get_change_fade_task(void *userdata)
{
    MemViewerFadeArgs *args;

    args = (MemViewerFadeArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->offset >= args->viewer->size || args->viewer->change_fade == NULL) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    args->fade_value = args->viewer->change_fade[args->offset];
    pthread_mutex_unlock(&args->viewer->lock);
    return 0;
}

static int mem_viewer_is_closed_task(void *userdata)
{
    MemViewerClosedArgs *args;

    args = (MemViewerClosedArgs *)userdata;
    args->is_closed = args->viewer->closed;
    return 0;
}

MemViewer *mem_viewer_open(const void *memory, size_t size)
{
    MemViewer *viewer;

    if (memory == NULL || size == 0U) {
        return NULL;
    }

    viewer = (MemViewer *)calloc(1U, sizeof(*viewer));
    if (viewer == NULL) {
        return NULL;
    }

    viewer->memory = (uint8_t *)memory;
    viewer->size = size;
    viewer->line_count = (size + MEM_VIEWER_BYTES_PER_ROW - 1U) / MEM_VIEWER_BYTES_PER_ROW;
    viewer->displayed_memory = (uint8_t *)malloc(size);
    viewer->changed_lines = (uint8_t *)calloc(viewer->line_count == 0U ? 1U : viewer->line_count, 1U);
    viewer->change_fade = (uint8_t *)malloc(size);
    if (viewer->displayed_memory == NULL || viewer->changed_lines == NULL || viewer->change_fade == NULL) {
        free(viewer->change_fade);
        free(viewer->changed_lines);
        free(viewer);
        return NULL;
    }
    if (pthread_mutex_init(&viewer->lock, NULL) != 0) {
        free(viewer->change_fade);
        free(viewer->displayed_memory);
        free(viewer->changed_lines);
        free(viewer);
        return NULL;
    }
    viewer->lock_initialized = 1;

    if (mem_viewer_ensure_backend() != 0) {
        mem_viewer_destroy(viewer);
        return NULL;
    }

    if (mem_viewer_invoke(mem_viewer_create_window_task, viewer) != 0) {
        mem_viewer_destroy(viewer);
        return NULL;
    }

    return viewer;
}

int mem_viewer_update(MemViewer *viewer)
{
    if (viewer == NULL) {
        return -1;
    }
    return mem_viewer_invoke(mem_viewer_reload_view_task, viewer);
}

void mem_viewer_destroy(MemViewer *viewer)
{
    if (viewer == NULL) {
        return;
    }

    if (viewer->lock_initialized) {
        if (g_backend_started && g_backend_ready) {
            mem_viewer_invoke(mem_viewer_destroy_view_task, viewer);
        }
        pthread_mutex_destroy(&viewer->lock);
    }
    if (g_backend_started) {
        mem_viewer_release_backend();
    }
    mem_viewer_clear_search_matches(viewer);
    free(viewer->change_fade);
    free(viewer->changed_lines);
    free(viewer->displayed_memory);
    free(viewer);
}

size_t mem_viewer_debug_copy_text(MemViewer *viewer, char *buffer, size_t buffer_size)
{
    MemViewerCopyTextArgs args;

    if (viewer == NULL) {
        return 0U;
    }

    memset(&args, 0, sizeof(args));
    args.viewer = viewer;
    args.buffer = buffer;
    args.buffer_size = buffer_size;
    if (mem_viewer_invoke(mem_viewer_copy_text_task, &args) != 0) {
        return 0U;
    }
    return args.required_size;
}

int mem_viewer_debug_set_window_position(MemViewer *viewer, int x, int y)
{
    MemViewerMoveWindowArgs args;

    if (viewer == NULL) {
        return -1;
    }

    args.viewer = viewer;
    args.x = x;
    args.y = y;
    return mem_viewer_invoke(mem_viewer_move_window_task, &args);
}

int mem_viewer_debug_scroll_to_offset(MemViewer *viewer, size_t offset)
{
    MemViewerScrollArgs args;

    if (viewer == NULL || offset >= viewer->size) {
        return -1;
    }

    args.viewer = viewer;
    args.offset = offset;
    return mem_viewer_invoke(mem_viewer_scroll_to_offset_task, &args);
}

int mem_viewer_debug_set_byte(MemViewer *viewer, size_t offset, uint8_t value)
{
    MemViewerSetByteArgs args;

    if (viewer == NULL) {
        return -1;
    }

    args.viewer = viewer;
    args.offset = offset;
    args.value = value;
    return mem_viewer_invoke(mem_viewer_set_byte_task, &args);
}

int mem_viewer_debug_set_auto_refresh(MemViewer *viewer, int enabled)
{
    MemViewerAutoRefreshArgs args;

    if (viewer == NULL) {
        return -1;
    }

    args.viewer = viewer;
    args.enabled = enabled;
    return mem_viewer_invoke(mem_viewer_set_auto_refresh_task, &args);
}

int mem_viewer_debug_set_search(MemViewer *viewer, const char *text, int decimal_mode)
{
    MemViewerSearchArgs args;

    if (viewer == NULL || text == NULL) {
        return -1;
    }

    args.viewer = viewer;
    args.text = text;
    args.decimal_mode = decimal_mode;
    return mem_viewer_invoke(mem_viewer_set_search_task, &args);
}

int mem_viewer_debug_search_next(MemViewer *viewer)
{
    MemViewerSearchStepArgs args;

    if (viewer == NULL) {
        return -1;
    }

    args.viewer = viewer;
    args.direction = 1;
    return mem_viewer_invoke(mem_viewer_step_search_task, &args);
}

int mem_viewer_debug_search_previous(MemViewer *viewer)
{
    MemViewerSearchStepArgs args;

    if (viewer == NULL) {
        return -1;
    }

    args.viewer = viewer;
    args.direction = -1;
    return mem_viewer_invoke(mem_viewer_step_search_task, &args);
}

int mem_viewer_debug_set_changed_only(MemViewer *viewer, int enabled)
{
    MemViewerChangedOnlyArgs args;

    if (viewer == NULL) {
        return -1;
    }

    args.viewer = viewer;
    args.enabled = enabled;
    return mem_viewer_invoke(mem_viewer_set_changed_only_task, &args);
}

int mem_viewer_debug_get_visible_line_count(MemViewer *viewer)
{
    MemViewerVisibleLinesArgs args;

    if (viewer == NULL) {
        return -1;
    }

    args.viewer = viewer;
    args.visible_line_count = -1;
    if (mem_viewer_invoke(mem_viewer_get_visible_lines_task, &args) != 0) {
        return -1;
    }
    return args.visible_line_count;
}

int mem_viewer_debug_get_change_fade(MemViewer *viewer, size_t offset)
{
    MemViewerFadeArgs args;

    if (viewer == NULL) {
        return -1;
    }

    args.viewer = viewer;
    args.offset = offset;
    args.fade_value = -1;
    if (mem_viewer_invoke(mem_viewer_get_change_fade_task, &args) != 0) {
        return -1;
    }
    return args.fade_value;
}

int mem_viewer_debug_is_closed(MemViewer *viewer)
{
    MemViewerClosedArgs args;

    if (viewer == NULL) {
        return 1;
    }

    args.viewer = viewer;
    args.is_closed = 1;
    if (mem_viewer_invoke(mem_viewer_is_closed_task, &args) != 0) {
        return 1;
    }
    return args.is_closed;
}

int mem_viewer_debug_select_offset(MemViewer *viewer, size_t offset)
{
    MemViewerSelectOffsetArgs args;

    if (viewer == NULL) {
        return -1;
    }

    args.viewer = viewer;
    args.offset = offset;
    return mem_viewer_invoke(mem_viewer_select_offset_task, &args);
}

size_t mem_viewer_debug_get_selected_offset(MemViewer *viewer)
{
    MemViewerGetOffsetArgs args;

    if (viewer == NULL) {
        return (size_t)-1;
    }

    args.viewer = viewer;
    args.offset = (size_t)-1;
    if (mem_viewer_invoke(mem_viewer_get_selected_offset_task, &args) != 0) {
        return (size_t)-1;
    }

    return args.offset;
}

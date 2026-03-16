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
    size_t size;
    pthread_mutex_t lock;
    int lock_initialized;
    int closed;
    GtkWidget *window;
    GtkWidget *scroller;
    GtkWidget *text_view;
    GtkWidget *status_label;
    GtkWidget *offset_entry;
    GtkWidget *value_entry;
};

typedef struct {
    MemViewer *viewer;
    const char *text;
} MemViewerSetTextArgs;

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
static int mem_viewer_parse_memory(const char *text, uint8_t *memory, size_t size, gchar **error_message);
static int mem_viewer_ensure_backend(void);
static void mem_viewer_release_backend(void);
static int mem_viewer_invoke(MemViewerGtkTask task, void *userdata);
static int mem_viewer_create_window_task(void *userdata);
static int mem_viewer_reload_view_task(void *userdata);
static int mem_viewer_destroy_view_task(void *userdata);
static int mem_viewer_set_text_task(void *userdata);
static int mem_viewer_copy_text_task(void *userdata);
static int mem_viewer_apply_task(void *userdata);
static int mem_viewer_move_window_task(void *userdata);
static int mem_viewer_scroll_to_offset_task(void *userdata);
static int mem_viewer_set_byte_task(void *userdata);

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

static int mem_viewer_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int mem_viewer_parse_memory(const char *text, uint8_t *memory, size_t size, gchar **error_message)
{
    const char *cursor;
    size_t written;

    cursor = text;
    written = 0U;
    while (*cursor != '\0') {
        const char *line_end;
        const char *colon;

        line_end = strchr(cursor, '\n');
        if (line_end == NULL) {
            line_end = cursor + strlen(cursor);
        }
        if (line_end == cursor) {
            cursor = *line_end == '\0' ? line_end : line_end + 1;
            continue;
        }

        colon = memchr(cursor, ':', (size_t)(line_end - cursor));
        if (colon == NULL) {
            *error_message = g_strdup("Each line must contain an address followed by ':'.");
            return -1;
        }

        cursor = colon + 1;
        while (cursor < line_end) {
            int high;
            int low;

            while (cursor < line_end && g_ascii_isspace((guchar)*cursor)) {
                ++cursor;
            }
            if (cursor >= line_end) {
                break;
            }
            if ((line_end - cursor) < 2) {
                *error_message = g_strdup("Found an incomplete hex byte.");
                return -1;
            }

            high = mem_viewer_hex_value(cursor[0]);
            low = mem_viewer_hex_value(cursor[1]);
            if (high < 0 || low < 0) {
                *error_message = g_strdup("Found a non-hexadecimal byte value.");
                return -1;
            }
            if (written >= size) {
                *error_message = g_strdup("The editor contains more bytes than the mapped memory region.");
                return -1;
            }

            memory[written] = (uint8_t)((high << 4) | low);
            written += 1U;
            cursor += 2;

            while (cursor < line_end && g_ascii_isspace((guchar)*cursor)) {
                ++cursor;
            }
        }

        cursor = *line_end == '\0' ? line_end : line_end + 1;
    }

    if (written != size) {
        *error_message = g_strdup_printf(
            "The editor describes %zu bytes, but the mapped region contains %zu bytes.",
            written,
            size
        );
        return -1;
    }

    return 0;
}

static int mem_viewer_parse_size_value(const char *text, size_t *value)
{
    char *end;
    unsigned long long parsed;

    parsed = strtoull(text, &end, 0);
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

static void mem_viewer_set_status(MemViewer *viewer, const char *status)
{
    gtk_label_set_text(GTK_LABEL(viewer->status_label), status);
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
    snprintf(status, sizeof(status), "Viewing %zu bytes", viewer->size);
    mem_viewer_set_status(viewer, status);
    g_free(formatted);
}

static void mem_viewer_apply_single_byte(MemViewer *viewer, size_t offset, uint8_t value)
{
    GtkTextBuffer *buffer;
    GtkTextIter iter;
    char status[128];

    viewer->memory[offset] = value;
    mem_viewer_reload_buffer(viewer);

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view));
    gtk_text_buffer_get_iter_at_offset(buffer, &iter, 0);
    gtk_text_buffer_place_cursor(buffer, &iter);
    snprintf(status, sizeof(status), "Updated byte %zu to %02X", offset, value);
    mem_viewer_set_status(viewer, status);
}

static void mem_viewer_on_apply_clicked(GtkButton *button, gpointer userdata)
{
    GtkTextBuffer *buffer;
    GtkTextIter start;
    GtkTextIter end;
    gchar *text;
    gchar *error_message;
    MemViewer *viewer;

    (void)button;
    viewer = (MemViewer *)userdata;
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(viewer->text_view));
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    error_message = NULL;
    if (mem_viewer_parse_memory(text, viewer->memory, viewer->size, &error_message) != 0) {
        mem_viewer_set_status(viewer, error_message);
        g_free(error_message);
        g_free(text);
        return;
    }

    mem_viewer_set_status(viewer, "Applied changes to memory");
    g_free(text);
}

static void mem_viewer_on_reload_clicked(GtkButton *button, gpointer userdata)
{
    (void)button;
    mem_viewer_reload_buffer((MemViewer *)userdata);
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
        mem_viewer_set_status(viewer, "Invalid byte offset");
        return;
    }
    if (mem_viewer_parse_byte_value(value_text, &value) != 0) {
        mem_viewer_set_status(viewer, "Invalid byte value, use hex like 7F");
        return;
    }

    mem_viewer_apply_single_byte(viewer, offset, value);
}

static void mem_viewer_on_window_destroy(GtkWidget *widget, gpointer userdata)
{
    MemViewer *viewer;

    (void)widget;
    viewer = (MemViewer *)userdata;
    pthread_mutex_lock(&viewer->lock);
    viewer->closed = 1;
    viewer->window = NULL;
    viewer->scroller = NULL;
    viewer->text_view = NULL;
    viewer->status_label = NULL;
    viewer->offset_entry = NULL;
    viewer->value_entry = NULL;
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
    GtkWidget *box;
    GtkWidget *controls;
    GtkWidget *reload_button;
    GtkWidget *apply_button;
    GtkWidget *set_byte_button;
    GtkWidget *offset_label;
    GtkWidget *value_label;
    MemViewer *viewer;

    viewer = (MemViewer *)userdata;

    viewer->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    viewer->scroller = gtk_scrolled_window_new(NULL, NULL);
    viewer->text_view = gtk_text_view_new();
    viewer->status_label = gtk_label_new("");
    viewer->offset_entry = gtk_entry_new();
    viewer->value_entry = gtk_entry_new();
    if (viewer->window == NULL || viewer->scroller == NULL || viewer->text_view == NULL ||
        viewer->status_label == NULL || viewer->offset_entry == NULL || viewer->value_entry == NULL) {
        return -1;
    }

    gtk_window_set_title(GTK_WINDOW(viewer->window), "Memory Viewer");
    gtk_window_set_default_size(GTK_WINDOW(viewer->window), 760, 480);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    reload_button = gtk_button_new_with_label("Reload");
    apply_button = gtk_button_new_with_label("Apply");
    set_byte_button = gtk_button_new_with_label("Set Byte");
    offset_label = gtk_label_new("Offset");
    value_label = gtk_label_new("Value");

    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(viewer->text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(viewer->text_view), GTK_WRAP_NONE);
    gtk_entry_set_width_chars(GTK_ENTRY(viewer->offset_entry), 10);
    gtk_entry_set_width_chars(GTK_ENTRY(viewer->value_entry), 4);
    gtk_entry_set_max_length(GTK_ENTRY(viewer->value_entry), 2);
    gtk_entry_set_text(GTK_ENTRY(viewer->offset_entry), "0");
    gtk_entry_set_text(GTK_ENTRY(viewer->value_entry), "00");
    gtk_container_add(GTK_CONTAINER(viewer->scroller), viewer->text_view);
    gtk_box_pack_start(GTK_BOX(controls), reload_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), apply_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), offset_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), viewer->offset_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), value_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), viewer->value_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), set_byte_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), controls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), viewer->scroller, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), viewer->status_label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(viewer->window), box);

    g_signal_connect(viewer->window, "destroy", G_CALLBACK(mem_viewer_on_window_destroy), viewer);
    g_signal_connect(reload_button, "clicked", G_CALLBACK(mem_viewer_on_reload_clicked), viewer);
    g_signal_connect(apply_button, "clicked", G_CALLBACK(mem_viewer_on_apply_clicked), viewer);
    g_signal_connect(set_byte_button, "clicked", G_CALLBACK(mem_viewer_on_set_byte_clicked), viewer);

    mem_viewer_reload_buffer(viewer);
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

    mem_viewer_reload_buffer(viewer);
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

static int mem_viewer_set_text_task(void *userdata)
{
    GtkTextBuffer *buffer;
    MemViewerSetTextArgs *args;

    args = (MemViewerSetTextArgs *)userdata;
    pthread_mutex_lock(&args->viewer->lock);
    if (args->viewer->closed || args->viewer->text_view == NULL) {
        pthread_mutex_unlock(&args->viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&args->viewer->lock);

    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(args->viewer->text_view));
    gtk_text_buffer_set_text(buffer, args->text, -1);
    mem_viewer_set_status(args->viewer, "Test edited buffer contents");
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

static int mem_viewer_apply_task(void *userdata)
{
    MemViewer *viewer;

    viewer = (MemViewer *)userdata;
    pthread_mutex_lock(&viewer->lock);
    if (viewer->closed || viewer->text_view == NULL) {
        pthread_mutex_unlock(&viewer->lock);
        return -1;
    }
    pthread_mutex_unlock(&viewer->lock);

    mem_viewer_on_apply_clicked(NULL, viewer);
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
    if (pthread_mutex_init(&viewer->lock, NULL) != 0) {
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

int mem_viewer_debug_set_text(MemViewer *viewer, const char *text)
{
    MemViewerSetTextArgs args;

    if (viewer == NULL || text == NULL) {
        return -1;
    }

    args.viewer = viewer;
    args.text = text;
    return mem_viewer_invoke(mem_viewer_set_text_task, &args);
}

int mem_viewer_debug_apply(MemViewer *viewer)
{
    if (viewer == NULL) {
        return -1;
    }

    return mem_viewer_invoke(mem_viewer_apply_task, viewer);
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

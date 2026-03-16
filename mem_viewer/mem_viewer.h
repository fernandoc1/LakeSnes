#ifndef MEM_VIEWER_H
#define MEM_VIEWER_H

#include <stddef.h>

typedef struct MemViewer MemViewer;

MemViewer *mem_viewer_open(const void *memory, size_t size);
int mem_viewer_update_memory(MemViewer *viewer, const void *memory, size_t size);
void mem_viewer_destroy(MemViewer *viewer);

#endif

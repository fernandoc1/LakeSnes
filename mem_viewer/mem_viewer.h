#ifndef MEM_VIEWER_H
#define MEM_VIEWER_H

#include <stddef.h>

/*
This project is a memory viewer and editor implemented as a shared library that can be instantiated anywhere within an application.
Once provided with a memory address and the size of the memory region, the library opens a window displaying the contents of that memory block.
The user can inspect and modify the values directly through the interface.
The viewer runs in a separate thread from the main application, ensuring that the primary program execution remains unaffected.
The graphical interface is implemented using GTK, which serves as the rendering backend for the viewer window.
*/

typedef struct MemViewer MemViewer;

#ifdef __cplusplus
extern "C" {
#endif

MemViewer *mem_viewer_open(const void *memory, size_t size);
int mem_viewer_update(MemViewer *viewer);
void mem_viewer_destroy(MemViewer *viewer);

#ifdef __cplusplus
}
#endif

#endif

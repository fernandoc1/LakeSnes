#ifndef TRACE_RECORDER_H
#define TRACE_RECORDER_H

#include <stdbool.h>
#include <stddef.h>

#include "cpu.h"
#include "snes.h"

typedef struct TraceRecorder TraceRecorder;

TraceRecorder* traceRecorder_init(Snes* snes);
void traceRecorder_free(TraceRecorder* recorder);

bool traceRecorder_begin(TraceRecorder* recorder);
void traceRecorder_end(TraceRecorder* recorder);
void traceRecorder_clear(TraceRecorder* recorder);

bool traceRecorder_isRecording(const TraceRecorder* recorder);
int traceRecorder_getRecordCount(const TraceRecorder* recorder);
const CpuInstructionInfo* traceRecorder_getRecord(const TraceRecorder* recorder, int index);

bool traceRecorder_restoreInitialState(TraceRecorder* recorder);
bool traceRecorder_saveToFile(const TraceRecorder* recorder, const char* path);
bool traceRecorder_loadFromFile(TraceRecorder* recorder, const char* path);
bool traceRecorder_dumpDisassembly(const TraceRecorder* recorder, const char* path);
void traceRecorder_setRuntimeGraphEnabled(TraceRecorder* recorder, bool enabled);
void traceRecorder_clearRuntimeGraph(TraceRecorder* recorder);
bool traceRecorder_dumpRuntimeGraph(const TraceRecorder* recorder, const char* path);
void traceRecorder_setRuntimeNotesEnabled(TraceRecorder* recorder, bool enabled);
void traceRecorder_clearRuntimeNotes(TraceRecorder* recorder);
bool traceRecorder_dumpRuntimeNotes(const TraceRecorder* recorder, const char* path);
void traceRecorder_formatRecord(const CpuInstructionInfo* info, char* line, size_t lineSize);

#endif

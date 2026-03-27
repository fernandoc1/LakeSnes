#include <stdio.h>

#include "trace_recorder.h"

int main(int argc, char** argv) {
  if(argc != 2) {
    fprintf(stderr, "Usage: %s <trace.ltrc>\n", argc > 0 ? argv[0] : "ltrc_dump");
    return 1;
  }

  TraceRecorder* recorder = traceRecorder_init(NULL);
  if(recorder == NULL) {
    fprintf(stderr, "Failed to initialize trace recorder\n");
    return 1;
  }

  if(!traceRecorder_loadFromFile(recorder, argv[1])) {
    fprintf(stderr, "Failed to load trace file: %s\n", argv[1]);
    traceRecorder_free(recorder);
    return 1;
  }

  char line[96];
  int count = traceRecorder_getRecordCount(recorder);
  for(int i = 0; i < count; ++i) {
    const CpuInstructionInfo* info = traceRecorder_getRecord(recorder, i);
    traceRecorder_formatRecord(info, line, sizeof(line));
    puts(line);
  }

  traceRecorder_free(recorder);
  return 0;
}

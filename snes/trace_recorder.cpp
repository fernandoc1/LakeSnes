#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "trace_recorder.h"

struct TraceRecorder {
  Snes* snes;
  bool recording;
  std::vector<CpuInstructionInfo> records;
  uint8_t* initialStateData;
  int initialStateSize;
};

static const uint32_t traceMagic = 0x4352544c; // 'LTRC'
static const uint32_t traceVersion = 1;

static void traceRecorderHook(void* userData, const Cpu* cpu, const CpuInstructionInfo* info) {
  (void) cpu;
  TraceRecorder* recorder = static_cast<TraceRecorder*>(userData);
  recorder->records.push_back(*info);
}

static bool traceRecorderWriteU32(FILE* file, uint32_t value) {
  return fwrite(&value, sizeof(value), 1, file) == 1;
}

static bool traceRecorderReadU32(FILE* file, uint32_t* value) {
  return fread(value, sizeof(*value), 1, file) == 1;
}

static bool traceRecorderCaptureInitialState(TraceRecorder* recorder) {
  int size = snes_saveState(recorder->snes, NULL);
  if(size <= 0) {
    return false;
  }

  uint8_t* data = static_cast<uint8_t*>(malloc(size));
  if(data == NULL) {
    return false;
  }

  if(snes_saveState(recorder->snes, data) != size) {
    free(data);
    return false;
  }

  free(recorder->initialStateData);
  recorder->initialStateData = data;
  recorder->initialStateSize = size;
  return true;
}

TraceRecorder* traceRecorder_init(Snes* snes) {
  TraceRecorder* recorder = new TraceRecorder();
  recorder->snes = snes;
  recorder->recording = false;
  recorder->initialStateData = NULL;
  recorder->initialStateSize = 0;
  return recorder;
}

void traceRecorder_free(TraceRecorder* recorder) {
  if(recorder == NULL) {
    return;
  }
  traceRecorder_end(recorder);
  free(recorder->initialStateData);
  delete recorder;
}

bool traceRecorder_begin(TraceRecorder* recorder) {
  if(recorder == NULL) {
    return false;
  }

  traceRecorder_end(recorder);
  recorder->records.clear();
  if(!traceRecorderCaptureInitialState(recorder)) {
    return false;
  }

  cpu_setInstructionHook(recorder->snes->cpu, traceRecorderHook, recorder);
  recorder->recording = true;
  return true;
}

void traceRecorder_end(TraceRecorder* recorder) {
  if(recorder == NULL) {
    return;
  }
  if(recorder->recording) {
    cpu_setInstructionHook(recorder->snes->cpu, NULL, NULL);
    recorder->recording = false;
  }
}

void traceRecorder_clear(TraceRecorder* recorder) {
  if(recorder == NULL) {
    return;
  }
  traceRecorder_end(recorder);
  recorder->records.clear();
  free(recorder->initialStateData);
  recorder->initialStateData = NULL;
  recorder->initialStateSize = 0;
}

bool traceRecorder_isRecording(const TraceRecorder* recorder) {
  return recorder != NULL && recorder->recording;
}

int traceRecorder_getRecordCount(const TraceRecorder* recorder) {
  if(recorder == NULL) {
    return 0;
  }
  return static_cast<int>(recorder->records.size());
}

const CpuInstructionInfo* traceRecorder_getRecord(const TraceRecorder* recorder, int index) {
  if(recorder == NULL || index < 0 || index >= static_cast<int>(recorder->records.size())) {
    return NULL;
  }
  return &recorder->records[static_cast<size_t>(index)];
}

bool traceRecorder_restoreInitialState(TraceRecorder* recorder) {
  if(recorder == NULL || recorder->initialStateData == NULL || recorder->initialStateSize <= 0) {
    return false;
  }
  return snes_loadState(recorder->snes, recorder->initialStateData, recorder->initialStateSize);
}

bool traceRecorder_saveToFile(const TraceRecorder* recorder, const char* path) {
  if(recorder == NULL || path == NULL || recorder->initialStateData == NULL) {
    return false;
  }

  FILE* file = fopen(path, "wb");
  if(file == NULL) {
    return false;
  }

  bool ok = true;
  ok = ok && traceRecorderWriteU32(file, traceMagic);
  ok = ok && traceRecorderWriteU32(file, traceVersion);
  ok = ok && traceRecorderWriteU32(file, static_cast<uint32_t>(recorder->initialStateSize));
  ok = ok && traceRecorderWriteU32(file, static_cast<uint32_t>(recorder->records.size()));
  ok = ok && fwrite(recorder->initialStateData, recorder->initialStateSize, 1, file) == 1;

  for(size_t i = 0; ok && i < recorder->records.size(); ++i) {
    const CpuInstructionInfo& info = recorder->records[i];
    ok = ok && traceRecorderWriteU32(file, info.address);
    ok = ok && fwrite(&info.opcode, sizeof(info.opcode), 1, file) == 1;
    ok = ok && fwrite(&info.size, sizeof(info.size), 1, file) == 1;
    ok = ok && fwrite(info.bytes, sizeof(info.bytes), 1, file) == 1;
    ok = ok && fwrite(info.mnemonic, sizeof(info.mnemonic), 1, file) == 1;
    ok = ok && fwrite(info.operands, sizeof(info.operands), 1, file) == 1;
    ok = ok && fwrite(info.formatted, sizeof(info.formatted), 1, file) == 1;
  }

  fclose(file);
  return ok;
}

bool traceRecorder_loadFromFile(TraceRecorder* recorder, const char* path) {
  if(recorder == NULL || path == NULL) {
    return false;
  }

  FILE* file = fopen(path, "rb");
  if(file == NULL) {
    return false;
  }

  traceRecorder_clear(recorder);

  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t stateSize = 0;
  uint32_t recordCount = 0;
  bool ok = true;

  ok = ok && traceRecorderReadU32(file, &magic);
  ok = ok && traceRecorderReadU32(file, &version);
  ok = ok && traceRecorderReadU32(file, &stateSize);
  ok = ok && traceRecorderReadU32(file, &recordCount);
  ok = ok && magic == traceMagic;
  ok = ok && version == traceVersion;

  if(ok && stateSize > 0) {
    recorder->initialStateData = static_cast<uint8_t*>(malloc(stateSize));
    ok = recorder->initialStateData != NULL;
    if(ok) {
      recorder->initialStateSize = static_cast<int>(stateSize);
      ok = fread(recorder->initialStateData, stateSize, 1, file) == 1;
    }
  }

  if(ok) {
    recorder->records.resize(recordCount);
    for(uint32_t i = 0; ok && i < recordCount; ++i) {
      CpuInstructionInfo& info = recorder->records[i];
      memset(&info, 0, sizeof(info));
      ok = ok && traceRecorderReadU32(file, &info.address);
      ok = ok && fread(&info.opcode, sizeof(info.opcode), 1, file) == 1;
      ok = ok && fread(&info.size, sizeof(info.size), 1, file) == 1;
      ok = ok && fread(info.bytes, sizeof(info.bytes), 1, file) == 1;
      ok = ok && fread(info.mnemonic, sizeof(info.mnemonic), 1, file) == 1;
      ok = ok && fread(info.operands, sizeof(info.operands), 1, file) == 1;
      ok = ok && fread(info.formatted, sizeof(info.formatted), 1, file) == 1;
    }
  }

  fclose(file);

  if(!ok) {
    traceRecorder_clear(recorder);
    return false;
  }

  return true;
}

void traceRecorder_formatRecord(const CpuInstructionInfo* info, char* line, size_t lineSize) {
  if(line == NULL || lineSize == 0) {
    return;
  }
  if(info == NULL) {
    snprintf(line, lineSize, "<null>");
    return;
  }

  if(info->operands[0] != '\0') {
    snprintf(
      line,
      lineSize,
      "%06x  %-4s %s",
      info->address & 0xffffff,
      info->mnemonic,
      info->operands
    );
  } else {
    snprintf(
      line,
      lineSize,
      "%06x  %s",
      info->address & 0xffffff,
      info->mnemonic
    );
  }
}

bool traceRecorder_dumpDisassembly(const TraceRecorder* recorder, const char* path) {
  if(recorder == NULL || path == NULL) {
    return false;
  }

  FILE* file = fopen(path, "w");
  if(file == NULL) {
    return false;
  }

  char line[96];
  for(size_t i = 0; i < recorder->records.size(); ++i) {
    traceRecorder_formatRecord(&recorder->records[i], line, sizeof(line));
    if(fprintf(file, "%s\n", line) < 0) {
      fclose(file);
      return false;
    }
  }

  fclose(file);
  return true;
}

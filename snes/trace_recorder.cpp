#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "trace_recorder.h"

struct RuntimeGraphNode {
  CpuInstructionInfo info;
  bool hasFileOffset;
  uint32_t fileOffset;
  uint64_t executionCount;
};

struct RuntimeGraphEdge {
  size_t from;
  size_t to;
  uint64_t executionCount;
};

struct TraceRecorder {
  Snes* snes;
  bool recording;
  bool hookInstalled;
  bool runtimeGraphEnabled;
  std::vector<CpuInstructionInfo> records;
  uint8_t* initialStateData;
  int initialStateSize;
  std::vector<RuntimeGraphNode> runtimeNodes;
  std::vector<RuntimeGraphEdge> runtimeEdges;
  std::unordered_map<std::string, size_t> runtimeNodeIds;
  std::unordered_map<uint64_t, size_t> runtimeEdgeIds;
  bool hasPreviousRuntimeNode;
  size_t previousRuntimeNode;
};

static const uint32_t traceMagic = 0x4352544c; // 'LTRC'
static const uint32_t traceVersion = 1;

static void traceRecorderHook(void* userData, const Cpu* cpu, const CpuInstructionInfo* info);
void traceRecorder_clearRuntimeGraph(TraceRecorder* recorder);

static bool traceRecorderIsRomAddress(const Snes* snes, uint32_t address) {
  if(snes == NULL || snes->cart == NULL) {
    return false;
  }

  const uint8_t bank = (address >> 16) & 0xff;
  const uint16_t adr = address & 0xffff;
  const uint8_t bankMasked = bank & 0x7f;

  switch(snes->cart->type) {
    case 1:
    case 2:
    case 3:
      return adr >= 0x8000 || bankMasked >= 0x40;
    default:
      return false;
  }
}

static bool traceRecorderGetFileOffset(const Snes* snes, uint32_t address, uint32_t* fileOffset) {
  if(snes == NULL || snes->cart == NULL || fileOffset == NULL) {
    return false;
  }

  uint32_t romOffset = 0;
  const uint8_t bank = (address >> 16) & 0xff;
  const uint16_t adr = address & 0xffff;

  switch(snes->cart->type) {
    case 1: {
      const uint8_t bankMasked = bank & 0x7f;
      if(!(adr >= 0x8000 || bankMasked >= 0x40)) {
        return false;
      }
      romOffset = ((bankMasked << 15) | (adr & 0x7fff)) & (snes->cart->romSize - 1);
      break;
    }
    case 2: {
      const uint8_t bankMasked = bank & 0x7f;
      if(!(adr >= 0x8000 || bankMasked >= 0x40)) {
        return false;
      }
      romOffset = (((bankMasked & 0x3f) << 16) | adr) & (snes->cart->romSize - 1);
      break;
    }
    case 3: {
      const bool secondHalf = bank < 0x80;
      const uint8_t bankMasked = bank & 0x7f;
      if(!(adr >= 0x8000 || bankMasked >= 0x40)) {
        return false;
      }
      romOffset = (((bankMasked & 0x3f) << 16) | (secondHalf ? 0x400000 : 0) | adr) & (snes->cart->romSize - 1);
      break;
    }
    default:
      return false;
  }

  *fileOffset = romOffset + snes->romFileHeaderSize;
  return true;
}

static void traceRecorderWriteDotEscaped(FILE* file, const char* text) {
  if(file == NULL || text == NULL) {
    return;
  }
  for(const char* p = text; *p != '\0'; ++p) {
    switch(*p) {
      case '\\': fputs("\\\\", file); break;
      case '"': fputs("\\\"", file); break;
      case '\n': fputs("\\n", file); break;
      case '\r': fputs("\\r", file); break;
      case '\t': fputs("\\t", file); break;
      default: fputc(*p, file); break;
    }
  }
}

static void traceRecorderRefreshHook(TraceRecorder* recorder) {
  if(recorder == NULL || recorder->snes == NULL || recorder->snes->cpu == NULL) {
    return;
  }

  const bool wantHook = recorder->recording || recorder->runtimeGraphEnabled;
  if(wantHook == recorder->hookInstalled) {
    return;
  }

  cpu_setInstructionHook(recorder->snes->cpu, wantHook ? traceRecorderHook : NULL, wantHook ? recorder : NULL);
  recorder->hookInstalled = wantHook;
}

static std::string traceRecorderRuntimeNodeKey(const CpuInstructionInfo* info) {
  char buffer[160];
  snprintf(
    buffer,
    sizeof(buffer),
    "%06x:%02x:%u:%02x%02x%02x%02x:%s",
    info->address & 0xffffff,
    info->opcode,
    info->size,
    info->bytes[0],
    info->bytes[1],
    info->bytes[2],
    info->bytes[3],
    info->formatted
  );
  return std::string(buffer);
}

static void traceRecorderRecordRuntimeInstruction(TraceRecorder* recorder, const CpuInstructionInfo* info) {
  if(recorder == NULL || info == NULL || !traceRecorderIsRomAddress(recorder->snes, info->address)) {
    recorder->hasPreviousRuntimeNode = false;
    return;
  }

  const std::string key = traceRecorderRuntimeNodeKey(info);
  size_t nodeId = 0;
  const auto existingNode = recorder->runtimeNodeIds.find(key);
  if(existingNode != recorder->runtimeNodeIds.end()) {
    nodeId = existingNode->second;
  } else {
    RuntimeGraphNode node = {};
    node.info = *info;
    node.hasFileOffset = traceRecorderGetFileOffset(recorder->snes, info->address, &node.fileOffset);
    node.executionCount = 0;
    nodeId = recorder->runtimeNodes.size();
    recorder->runtimeNodes.push_back(node);
    recorder->runtimeNodeIds.emplace(key, nodeId);
  }

  recorder->runtimeNodes[nodeId].executionCount++;

  if(recorder->hasPreviousRuntimeNode) {
    const uint64_t edgeKey = ((uint64_t)recorder->previousRuntimeNode << 32) | (uint64_t)nodeId;
    const auto existingEdge = recorder->runtimeEdgeIds.find(edgeKey);
    if(existingEdge != recorder->runtimeEdgeIds.end()) {
      recorder->runtimeEdges[existingEdge->second].executionCount++;
    } else {
      RuntimeGraphEdge edge = {};
      edge.from = recorder->previousRuntimeNode;
      edge.to = nodeId;
      edge.executionCount = 1;
      recorder->runtimeEdgeIds.emplace(edgeKey, recorder->runtimeEdges.size());
      recorder->runtimeEdges.push_back(edge);
    }
  }

  recorder->previousRuntimeNode = nodeId;
  recorder->hasPreviousRuntimeNode = true;
}

static void traceRecorderHook(void* userData, const Cpu* cpu, const CpuInstructionInfo* info) {
  (void) cpu;
  TraceRecorder* recorder = static_cast<TraceRecorder*>(userData);
  if(recorder->recording) {
    recorder->records.push_back(*info);
  }
  if(recorder->runtimeGraphEnabled) {
    traceRecorderRecordRuntimeInstruction(recorder, info);
  }
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
  recorder->hookInstalled = false;
  recorder->runtimeGraphEnabled = false;
  recorder->initialStateData = NULL;
  recorder->initialStateSize = 0;
  recorder->hasPreviousRuntimeNode = false;
  recorder->previousRuntimeNode = 0;
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

  recorder->recording = true;
  traceRecorderRefreshHook(recorder);
  return true;
}

void traceRecorder_end(TraceRecorder* recorder) {
  if(recorder == NULL) {
    return;
  }
  recorder->recording = false;
  traceRecorderRefreshHook(recorder);
}

void traceRecorder_clear(TraceRecorder* recorder) {
  if(recorder == NULL) {
    return;
  }
  traceRecorder_end(recorder);
  recorder->records.clear();
  traceRecorder_clearRuntimeGraph(recorder);
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

void traceRecorder_setRuntimeGraphEnabled(TraceRecorder* recorder, bool enabled) {
  if(recorder == NULL) {
    return;
  }
  recorder->runtimeGraphEnabled = enabled;
  if(enabled) {
    recorder->hasPreviousRuntimeNode = false;
  }
  traceRecorderRefreshHook(recorder);
}

void traceRecorder_clearRuntimeGraph(TraceRecorder* recorder) {
  if(recorder == NULL) {
    return;
  }
  recorder->runtimeNodes.clear();
  recorder->runtimeEdges.clear();
  recorder->runtimeNodeIds.clear();
  recorder->runtimeEdgeIds.clear();
  recorder->hasPreviousRuntimeNode = false;
  recorder->previousRuntimeNode = 0;
}

bool traceRecorder_dumpRuntimeGraph(const TraceRecorder* recorder, const char* path) {
  if(recorder == NULL || path == NULL) {
    return false;
  }

  FILE* file = fopen(path, "w");
  if(file == NULL) {
    return false;
  }

  fprintf(file, "digraph runtime_cfg {\n");
  fprintf(file, "  // nodes: %zu\n", recorder->runtimeNodes.size());
  fprintf(file, "  // edges: %zu\n", recorder->runtimeEdges.size());
  fprintf(file, "  node [shape=box];\n");

  for(size_t i = 0; i < recorder->runtimeNodes.size(); ++i) {
    const RuntimeGraphNode& node = recorder->runtimeNodes[i];
    fprintf(file, "  n%zu [label=\"", i);
    if(node.hasFileOffset) {
      fprintf(file, "%06x [file@%06x]\\n", node.info.address & 0xffffff, node.fileOffset);
    } else {
      fprintf(file, "%06x\\n", node.info.address & 0xffffff);
    }
    traceRecorderWriteDotEscaped(file, node.info.formatted);
    fprintf(file, "\\ncount=%llu\"];\n", (unsigned long long)node.executionCount);
  }

  for(size_t i = 0; i < recorder->runtimeEdges.size(); ++i) {
    const RuntimeGraphEdge& edge = recorder->runtimeEdges[i];
    fprintf(
      file,
      "  n%zu -> n%zu [label=\"%llu\"];\n",
      edge.from,
      edge.to,
      (unsigned long long)edge.executionCount
    );
  }

  fprintf(file, "}\n");
  fclose(file);
  return true;
}

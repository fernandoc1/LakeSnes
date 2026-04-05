#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

struct RuntimeMemoryAccess {
  uint32_t fileOffset;
  uint64_t readCount;
  uint64_t writeCount;
};

struct TraceRecorder {
  Snes* snes;
  bool recording;
  bool hookInstalled;
  bool runtimeGraphEnabled;
  bool runtimeNotesEnabled;
  std::vector<CpuInstructionInfo> records;
  uint8_t* initialStateData;
  int initialStateSize;
  std::vector<RuntimeGraphNode> runtimeNodes;
  std::vector<RuntimeGraphEdge> runtimeEdges;
  std::unordered_map<std::string, size_t> runtimeNodeIds;
  std::unordered_map<uint64_t, size_t> runtimeEdgeIds;
  std::unordered_map<uint32_t, RuntimeMemoryAccess> runtimeMemoryAccesses;
  bool hasPreviousRuntimeNode;
  size_t previousRuntimeNode;
};

static const uint32_t traceMagic = 0x4352544c; // 'LTRC'
static const uint32_t traceVersion = 1;

static void traceRecorderHook(void* userData, const Cpu* cpu, const CpuInstructionInfo* info);
static void traceRecorderAccessHook(void* userData, uint32_t adr, uint8_t val, bool write);
void traceRecorder_clearRuntimeGraph(TraceRecorder* recorder);
void traceRecorder_clearRuntimeNotes(TraceRecorder* recorder);

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

static void traceRecorderWriteJsonEscaped(FILE* file, const char* text) {
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

static void traceRecorderMnemonicColor(const char* mnemonic, char* color, size_t colorSize) {
  uint32_t hash = 2166136261u;
  if(mnemonic != NULL) {
    for(size_t i = 0; mnemonic[i] != '\0'; ++i) {
      hash ^= (uint8_t)mnemonic[i];
      hash *= 16777619u;
    }
  }

  const double hue = (double)(hash % 360u);
  const double saturation = 0.55 + (double)((hash >> 9) & 0x1f) / 100.0;
  const double value = 0.72 + (double)((hash >> 17) & 0x0f) / 50.0;
  const double s = saturation > 0.88 ? 0.88 : saturation;
  const double v = value > 0.96 ? 0.96 : value;
  const double c = v * s;
  const double hPrime = hue / 60.0;
  const double x = c * (1.0 - fabs(fmod(hPrime, 2.0) - 1.0));
  double r1 = 0.0;
  double g1 = 0.0;
  double b1 = 0.0;

  if(hPrime < 1.0) {
    r1 = c; g1 = x; b1 = 0.0;
  } else if(hPrime < 2.0) {
    r1 = x; g1 = c; b1 = 0.0;
  } else if(hPrime < 3.0) {
    r1 = 0.0; g1 = c; b1 = x;
  } else if(hPrime < 4.0) {
    r1 = 0.0; g1 = x; b1 = c;
  } else if(hPrime < 5.0) {
    r1 = x; g1 = 0.0; b1 = c;
  } else {
    r1 = c; g1 = 0.0; b1 = x;
  }

  const double m = v - c;
  const unsigned r = (unsigned)((r1 + m) * 255.0);
  const unsigned g = (unsigned)((g1 + m) * 255.0);
  const unsigned b = (unsigned)((b1 + m) * 255.0);
  snprintf(color, colorSize, "#%02x%02x%02x", r & 0xffu, g & 0xffu, b & 0xffu);
}

static bool traceRecorderIsControlFlowOpcode(uint8_t opcode) {
  switch(opcode) {
    case 0x10: // bpl
    case 0x20: // jsr
    case 0x22: // jsl
    case 0x30: // bmi
    case 0x4c: // jmp
    case 0x50: // bvc
    case 0x5c: // jml
    case 0x70: // bvs
    case 0x80: // bra
    case 0x82: // brl
    case 0x90: // bcc
    case 0xb0: // bcs
    case 0xd0: // bne
    case 0xf0: // beq
    case 0xfc: // jsr (abs,x)
      return true;
    default:
      return false;
  }
}

static void traceRecorderRefreshHook(TraceRecorder* recorder) {
  if(recorder == NULL || recorder->snes == NULL || recorder->snes->cpu == NULL) {
    return;
  }

  const bool wantHook = recorder->recording || recorder->runtimeGraphEnabled || recorder->runtimeNotesEnabled;
  if(wantHook == recorder->hookInstalled) {
    return;
  }

  cpu_setInstructionHook(recorder->snes->cpu, wantHook ? traceRecorderHook : NULL, wantHook ? recorder : NULL);
  snes_setAccessHook(recorder->snes, wantHook ? traceRecorderAccessHook : NULL, wantHook ? recorder : NULL);
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

  if((recorder->runtimeGraphEnabled || recorder->runtimeNotesEnabled) && recorder->hasPreviousRuntimeNode) {
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
  if(recorder->runtimeGraphEnabled || recorder->runtimeNotesEnabled) {
    traceRecorderRecordRuntimeInstruction(recorder, info);
  }
}

static void traceRecorderAccessHook(void* userData, uint32_t adr, uint8_t val, bool write) {
  (void) val;
  TraceRecorder* recorder = static_cast<TraceRecorder*>(userData);
  if(recorder == NULL || !recorder->runtimeNotesEnabled) {
    return;
  }

  uint32_t fileOffset = 0;
  if(!traceRecorderIsRomAddress(recorder->snes, adr) || !traceRecorderGetFileOffset(recorder->snes, adr, &fileOffset)) {
    return;
  }

  RuntimeMemoryAccess& access = recorder->runtimeMemoryAccesses[fileOffset];
  access.fileOffset = fileOffset;
  if(write) {
    access.writeCount++;
  } else {
    access.readCount++;
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
  recorder->runtimeNotesEnabled = false;
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
  traceRecorder_clearRuntimeNotes(recorder);
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

void traceRecorder_setRuntimeNotesEnabled(TraceRecorder* recorder, bool enabled) {
  if(recorder == NULL) {
    return;
  }
  recorder->runtimeNotesEnabled = enabled;
  traceRecorderRefreshHook(recorder);
}

void traceRecorder_clearRuntimeNotes(TraceRecorder* recorder) {
  if(recorder == NULL) {
    return;
  }
  recorder->runtimeMemoryAccesses.clear();
}

bool traceRecorder_dumpRuntimeNotes(const TraceRecorder* recorder, const char* path) {
  if(recorder == NULL || path == NULL) {
    return false;
  }

  FILE* file = fopen(path, "w");
  if(file == NULL) {
    return false;
  }

  std::unordered_map<uint32_t, bool> executedOffsets;
  for(size_t i = 0; i < recorder->runtimeNodes.size(); ++i) {
    const RuntimeGraphNode& node = recorder->runtimeNodes[i];
    if(!node.hasFileOffset || node.info.size == 0) {
      continue;
    }
    for(uint8_t j = 0; j < node.info.size; ++j) {
      executedOffsets[node.fileOffset + j] = true;
    }
  }

  fprintf(file, "{\n  \"annotations\": [\n");
  bool first = true;

  for(size_t i = 0; i < recorder->runtimeNodes.size(); ++i) {
    const RuntimeGraphNode& node = recorder->runtimeNodes[i];
    if(!node.hasFileOffset || node.info.size == 0) {
      continue;
    }

    if(!first) {
      fprintf(file, ",\n");
    }
    first = false;

    std::string note = "executed ";
    {
      char prefix[192];
      snprintf(
        prefix,
        sizeof(prefix),
        "%llu x: %06x: %s",
        (unsigned long long)node.executionCount,
        node.info.address & 0xffffff,
        node.info.formatted
      );
      note += prefix;
    }
    char color[16];
    traceRecorderMnemonicColor(node.info.mnemonic, color, sizeof(color));

    if(traceRecorderIsControlFlowOpcode(node.info.opcode)) {
      bool firstTarget = true;
      for(size_t edgeIndex = 0; edgeIndex < recorder->runtimeEdges.size(); ++edgeIndex) {
        const RuntimeGraphEdge& edge = recorder->runtimeEdges[edgeIndex];
        if(edge.from != i || edge.to >= recorder->runtimeNodes.size()) {
          continue;
        }
        const RuntimeGraphNode& target = recorder->runtimeNodes[edge.to];
        if(!target.hasFileOffset) {
          continue;
        }

        char targetLabel[160];
        snprintf(
          targetLabel,
          sizeof(targetLabel),
          "%06x: %s (%llu x)",
          target.info.address & 0xffffff,
          target.info.formatted,
          (unsigned long long)edge.executionCount
        );
        if(firstTarget) {
          note += " -> ";
          firstTarget = false;
        } else {
          note += ", ";
        }

        char link[256];
        snprintf(link, sizeof(link), "[[0x%x|%s]]", target.fileOffset, targetLabel);
        note += link;
      }
    }

    fprintf(file, "    {\n");
    fprintf(file, "      \"positions\": [");
    for(uint8_t j = 0; j < node.info.size; ++j) {
      if(j != 0) {
        fprintf(file, ", ");
      }
      fprintf(file, "\"0x%x\"", (unsigned)(node.fileOffset + j));
    }
    fprintf(file, "],\n");
    fprintf(file, "      \"note\": \"");
    traceRecorderWriteJsonEscaped(file, note.c_str());
    fprintf(file, "\",\n");
    fprintf(file, "      \"color\": \"%s\"\n", color);
    fprintf(file, "    }");
  }

  for(const auto& entry : recorder->runtimeMemoryAccesses) {
    const RuntimeMemoryAccess& access = entry.second;
    if(executedOffsets.find(access.fileOffset) != executedOffsets.end()) {
      continue;
    }
    if(access.readCount == 0 && access.writeCount == 0) {
      continue;
    }

    if(!first) {
      fprintf(file, ",\n");
    }
    first = false;

    char note[192];
    const char* color = access.writeCount > 0
      ? (access.readCount > 0 ? "#d48cff" : "#ff7f50")
      : "#f0c94a";
    snprintf(
      note,
      sizeof(note),
      "rom data access @ file@%06x: reads=%llu writes=%llu",
      access.fileOffset,
      (unsigned long long)access.readCount,
      (unsigned long long)access.writeCount
    );

    fprintf(file, "    {\n");
    fprintf(file, "      \"positions\": [\"0x%x\"],\n", access.fileOffset);
    fprintf(file, "      \"note\": \"");
    traceRecorderWriteJsonEscaped(file, note);
    fprintf(file, "\",\n");
    fprintf(file, "      \"color\": \"%s\"\n", color);
    fprintf(file, "    }");
  }

  fprintf(file, "\n  ]\n}\n");
  fclose(file);
  return true;
}

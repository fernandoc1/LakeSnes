#include <stdio.h>
#include <stdint.h>
#include <math.h>

#include <string>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rom_disasm.h"
#include "cpu.h"

static const size_t kMaxCallStackDepth = 32;
static const size_t kContextKeyDepth = 6;
static const size_t kMaxContextsPerAddress = 64;
static const size_t kNodeLimitReached = (size_t)-1;
static const size_t kContextLimitReached = (size_t)-2;

struct RomAnalysisState {
  uint32_t address;
  bool e;
  bool mf;
  bool xf;
  bool c;
  bool cKnown;
  std::vector<uint64_t> returnStack;
  std::vector<uint32_t> callTargetStack;
};

struct RomAnalysisNode {
  RomAnalysisState state;
  CpuInstructionInfo info;
  bool hasFileOffset;
  uint32_t fileOffset;
  bool synthetic;
  std::string syntheticLabel;
};

struct RomAnalysisEdge {
  size_t from;
  size_t to;
  const char* label;
};

struct RomFunctionInfo {
  uint32_t entry;
  bool recursive;
  int recursionGroup;
};

static void rom_disasm_format(const CpuInstructionInfo* info, FILE* out) {
  if(info->operands[0] != '\0') {
    fprintf(out, "%06x  %-4s %s\n", info->address & 0xffffff, info->mnemonic, info->operands);
  } else {
    fprintf(out, "%06x  %s\n", info->address & 0xffffff, info->mnemonic);
  }
}

static std::string rom_disasm_stateKey(const RomAnalysisState& state) {
  char header[64];
  snprintf(
    header,
    sizeof(header),
    "%06x:%d:%d:%d:%d:%d",
    state.address & 0xffffff,
    state.e ? 1 : 0,
    state.mf ? 1 : 0,
    state.xf ? 1 : 0,
    state.c ? 1 : 0,
    state.cKnown ? 1 : 0
  );

  std::string key = header;
  char entry[32];
  snprintf(entry, sizeof(entry), ":rd%zu:cd%zu", state.returnStack.size(), state.callTargetStack.size());
  key += entry;

  const size_t returnStart = state.returnStack.size() > kContextKeyDepth ? state.returnStack.size() - kContextKeyDepth : 0;
  for(size_t i = returnStart; i < state.returnStack.size(); ++i) {
    const uint32_t returnAddress = state.returnStack[i] & 0xffffff;
    const uint32_t callAddress = (state.returnStack[i] >> 24) & 0xffffff;
    snprintf(entry, sizeof(entry), ":%06x>%06x", callAddress, returnAddress);
    key += entry;
  }
  const size_t callStart = state.callTargetStack.size() > kContextKeyDepth ? state.callTargetStack.size() - kContextKeyDepth : 0;
  for(size_t i = callStart; i < state.callTargetStack.size(); ++i) {
    snprintf(entry, sizeof(entry), ":@%06x", state.callTargetStack[i] & 0xffffff);
    key += entry;
  }
  return key;
}

static uint64_t rom_disasm_makeReturnFrame(uint32_t callAddress, uint32_t returnAddress) {
  return ((uint64_t)(callAddress & 0xffffff) << 24) | (uint64_t)(returnAddress & 0xffffff);
}

static uint32_t rom_disasm_getReturnAddress(uint64_t frame) {
  return frame & 0xffffff;
}

static uint32_t rom_disasm_getCallAddress(uint64_t frame) {
  return (frame >> 24) & 0xffffff;
}

static bool rom_disasm_callStackContains(const RomAnalysisState& state, uint32_t address) {
  for(size_t i = 0; i < state.callTargetStack.size(); ++i) {
    if((state.callTargetStack[i] & 0xffffff) == (address & 0xffffff)) {
      return true;
    }
  }
  return false;
}

static bool rom_disasm_isRomAddress(const Snes* snes, uint32_t address) {
  if(snes == NULL || snes->cart == NULL) {
    return false;
  }

  const uint8_t bank = (address >> 16) & 0xff;
  const uint16_t adr = address & 0xffff;
  const uint8_t bankMasked = bank & 0x7f;

  switch(snes->cart->type) {
    case 1:
      return adr >= 0x8000 || bankMasked >= 0x40;
    case 2:
    case 3:
      return adr >= 0x8000 || bankMasked >= 0x40;
    default:
      return false;
  }
}

static bool rom_disasm_getFileOffset(const Snes* snes, uint32_t address, uint32_t* fileOffset) {
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

static void rom_disasm_readInstruction(
  Snes* snes,
  const RomAnalysisState& state,
  CpuInstructionInfo* info
) {
  uint8_t bytes[4] = {};
  for(size_t i = 0; i < sizeof(bytes); ++i) {
    bytes[i] = snes_read(snes, (state.address + i) & 0xffffff);
  }

  uint8_t size = cpu_getInstructionSize(bytes[0], state.mf, state.xf);
  cpu_disassembleInstruction(state.address, state.mf, state.xf, bytes, size, info);
}

static RomAnalysisState rom_disasm_advanceState(
  const RomAnalysisState& state,
  const CpuInstructionInfo& info
) {
  RomAnalysisState next = state;
  next.address = (state.address & 0xff0000) | ((state.address + info.size) & 0xffff);

  switch(info.opcode) {
    case 0x18:
      next.c = false;
      next.cKnown = true;
      break;
    case 0x38:
      next.c = true;
      next.cKnown = true;
      break;
    case 0xc2:
      if(info.size > 1) {
        next.mf = next.mf && ((info.bytes[1] & 0x20) == 0);
        next.xf = next.xf && ((info.bytes[1] & 0x10) == 0);
        if(next.e) {
          next.mf = true;
          next.xf = true;
        }
      }
      break;
    case 0xe2:
      if(info.size > 1) {
        if(info.bytes[1] & 0x20) next.mf = true;
        if(info.bytes[1] & 0x10) next.xf = true;
      }
      break;
    case 0xfb:
      if(next.cKnown) {
        const bool newCarry = next.e;
        next.e = next.c;
        next.c = newCarry;
        if(next.e) {
          next.mf = true;
          next.xf = true;
        }
      }
      break;
    default:
      break;
  }

  return next;
}

static uint32_t rom_disasm_relativeTarget(const CpuInstructionInfo& info) {
  const uint16_t pc = info.address & 0xffff;
  return (info.address & 0xff0000) | ((pc + 2 + (int8_t)info.bytes[1]) & 0xffff);
}

static uint32_t rom_disasm_relativeLongTarget(const CpuInstructionInfo& info) {
  const uint16_t pc = info.address & 0xffff;
  const int16_t offset = (int16_t)((info.bytes[2] << 8) | info.bytes[1]);
  return (info.address & 0xff0000) | ((pc + 3 + offset) & 0xffff);
}

static uint32_t rom_disasm_absoluteTarget(const CpuInstructionInfo& info) {
  const uint16_t target = (info.bytes[2] << 8) | info.bytes[1];
  return (info.address & 0xff0000) | target;
}

static uint32_t rom_disasm_longTarget(const CpuInstructionInfo& info) {
  return ((uint32_t)info.bytes[3] << 16) | ((uint32_t)info.bytes[2] << 8) | info.bytes[1];
}

static uint32_t rom_disasm_currentFunctionEntry(const RomAnalysisState& state) {
  if(!state.callTargetStack.empty()) {
    return state.callTargetStack.back() & 0xffffff;
  }
  return state.address & 0xffffff;
}

static void rom_disasm_collectFunctionInfos(
  const std::unordered_map<uint32_t, std::unordered_set<uint32_t> >& functionCalls,
  std::unordered_map<uint32_t, RomFunctionInfo>* functionInfos
) {
  std::vector<uint32_t> functions;
  std::unordered_map<uint32_t, int> functionIndex;
  for(std::unordered_map<uint32_t, std::unordered_set<uint32_t> >::const_iterator it = functionCalls.begin(); it != functionCalls.end(); ++it) {
    functionIndex[it->first] = (int)functions.size();
    functions.push_back(it->first);
  }

  std::vector<int> index(functions.size(), -1);
  std::vector<int> lowlink(functions.size(), -1);
  std::vector<bool> onStack(functions.size(), false);
  std::vector<int> stack;
  int nextIndex = 0;
  int nextGroup = 1;

  std::function<void(int)> strongConnect = [&](int v) {
    index[v] = nextIndex;
    lowlink[v] = nextIndex;
    nextIndex++;
    stack.push_back(v);
    onStack[v] = true;

    std::unordered_map<uint32_t, std::unordered_set<uint32_t> >::const_iterator edgeIt = functionCalls.find(functions[v]);
    if(edgeIt != functionCalls.end()) {
      for(std::unordered_set<uint32_t>::const_iterator toIt = edgeIt->second.begin(); toIt != edgeIt->second.end(); ++toIt) {
        std::unordered_map<uint32_t, int>::const_iterator targetIndexIt = functionIndex.find(*toIt);
        if(targetIndexIt == functionIndex.end()) {
          continue;
        }
        const int w = targetIndexIt->second;
        if(index[w] == -1) {
          strongConnect(w);
          if(lowlink[w] < lowlink[v]) {
            lowlink[v] = lowlink[w];
          }
        } else if(onStack[w] && index[w] < lowlink[v]) {
          lowlink[v] = index[w];
        }
      }
    }

    if(lowlink[v] == index[v]) {
      std::vector<int> component;
      while(!stack.empty()) {
        const int w = stack.back();
        stack.pop_back();
        onStack[w] = false;
        component.push_back(w);
        if(w == v) {
          break;
        }
      }

      bool recursiveComponent = component.size() > 1;
      if(!recursiveComponent && !component.empty()) {
        std::unordered_map<uint32_t, std::unordered_set<uint32_t> >::const_iterator selfIt = functionCalls.find(functions[component[0]]);
        recursiveComponent = selfIt != functionCalls.end() && selfIt->second.find(functions[component[0]]) != selfIt->second.end();
      }

      const int groupId = recursiveComponent && component.size() > 1 ? nextGroup++ : 0;
      for(size_t i = 0; i < component.size(); ++i) {
        RomFunctionInfo info = {};
        info.entry = functions[component[i]];
        info.recursive = recursiveComponent;
        info.recursionGroup = groupId;
        (*functionInfos)[info.entry] = info;
      }
    }
  };

  for(size_t i = 0; i < functions.size(); ++i) {
    if(index[i] == -1) {
      strongConnect((int)i);
    }
  }
}

static void rom_disasm_enqueueSyntheticSuccessor(
  size_t from,
  const char* edgeLabel,
  const std::string& nodeLabel,
  std::vector<RomAnalysisNode>* nodes,
  std::unordered_map<std::string, size_t>* syntheticLookup,
  std::vector<RomAnalysisEdge>* edges,
  std::unordered_set<std::string>* edgeSet
);

static size_t rom_disasm_getOrCreateNode(
  Snes* snes,
  const RomAnalysisState& state,
  int instructionLimit,
  std::vector<RomAnalysisNode>* nodes,
  std::unordered_map<std::string, size_t>* nodeLookup,
  std::vector<size_t>* worklist,
  std::unordered_map<uint32_t, size_t>* addressContextCounts
) {
  const std::string key = rom_disasm_stateKey(state);
  std::unordered_map<std::string, size_t>::const_iterator it = nodeLookup->find(key);
  if(it != nodeLookup->end()) {
    return it->second;
  }
  if(instructionLimit > 0 && (int)nodes->size() >= instructionLimit) {
    return kNodeLimitReached;
  }
  const uint32_t addressKey = state.address & 0xffffff;
  std::unordered_map<uint32_t, size_t>::const_iterator contextIt = addressContextCounts->find(addressKey);
  if(contextIt != addressContextCounts->end() && contextIt->second >= kMaxContextsPerAddress) {
    return kContextLimitReached;
  }

  RomAnalysisNode node = {};
  node.state = state;
  rom_disasm_readInstruction(snes, state, &node.info);
  node.hasFileOffset = rom_disasm_getFileOffset(snes, state.address, &node.fileOffset);
  node.synthetic = false;
  const size_t index = nodes->size();
  nodes->push_back(node);
  (*nodeLookup)[key] = index;
  worklist->push_back(index);
  (*addressContextCounts)[addressKey] += 1;
  return index;
}

static size_t rom_disasm_getOrCreateSyntheticNode(
  const std::string& label,
  std::vector<RomAnalysisNode>* nodes,
  std::unordered_map<std::string, size_t>* syntheticLookup
) {
  std::unordered_map<std::string, size_t>::const_iterator it = syntheticLookup->find(label);
  if(it != syntheticLookup->end()) {
    return it->second;
  }

  RomAnalysisNode node = {};
  node.synthetic = true;
  node.syntheticLabel = label;
  const size_t index = nodes->size();
  nodes->push_back(node);
  (*syntheticLookup)[label] = index;
  return index;
}

static void rom_disasm_addEdge(
  size_t from,
  size_t to,
  const char* label,
  std::vector<RomAnalysisEdge>* edges,
  std::unordered_set<std::string>* edgeSet
) {
  if(to == kNodeLimitReached || to == kContextLimitReached) {
    return;
  }

  const std::string key = std::to_string(from) + ":" + std::to_string(to) + ":" + label;
  if(!edgeSet->insert(key).second) {
    return;
  }

  RomAnalysisEdge edge = {from, to, label};
  edges->push_back(edge);
}

static void rom_disasm_enqueueSuccessor(
  Snes* snes,
  const RomAnalysisState& state,
  size_t from,
  const char* label,
  int instructionLimit,
  std::vector<RomAnalysisNode>* nodes,
  std::unordered_map<std::string, size_t>* nodeLookup,
  std::vector<size_t>* worklist,
  std::unordered_map<uint32_t, size_t>* addressContextCounts,
  std::unordered_map<std::string, size_t>* syntheticLookup,
  std::vector<RomAnalysisEdge>* edges,
  std::unordered_set<std::string>* edgeSet,
  size_t* contextLimitCount
) {
  if(!rom_disasm_isRomAddress(snes, state.address)) {
    return;
  }

  const size_t to = rom_disasm_getOrCreateNode(snes, state, instructionLimit, nodes, nodeLookup, worklist, addressContextCounts);
  if(to == kContextLimitReached) {
    if(contextLimitCount != NULL) {
      *contextLimitCount += 1;
    }
    char syntheticLabel[96];
    snprintf(syntheticLabel, sizeof(syntheticLabel), "context limit\\n%06x", state.address & 0xffffff);
    rom_disasm_enqueueSyntheticSuccessor(from, "context-limit", syntheticLabel, nodes, syntheticLookup, edges, edgeSet);
    return;
  }
  rom_disasm_addEdge(from, to, label, edges, edgeSet);
}

static void rom_disasm_enqueueSyntheticSuccessor(
  size_t from,
  const char* edgeLabel,
  const std::string& nodeLabel,
  std::vector<RomAnalysisNode>* nodes,
  std::unordered_map<std::string, size_t>* syntheticLookup,
  std::vector<RomAnalysisEdge>* edges,
  std::unordered_set<std::string>* edgeSet
) {
  const size_t to = rom_disasm_getOrCreateSyntheticNode(nodeLabel, nodes, syntheticLookup);
  rom_disasm_addEdge(from, to, edgeLabel, edges, edgeSet);
}

static void rom_disasm_enqueueCallsiteEdge(
  uint32_t address,
  size_t from,
  const char* label,
  const std::string& fallbackLabel,
  std::vector<RomAnalysisNode>* nodes,
  std::unordered_map<std::string, size_t>* syntheticLookup,
  std::vector<RomAnalysisEdge>* edges,
  std::unordered_set<std::string>* edgeSet
) {
  for(size_t i = 0; i < nodes->size(); ++i) {
    if(!(*nodes)[i].synthetic && (((*nodes)[i].info.address & 0xffffff) == (address & 0xffffff))) {
      rom_disasm_addEdge(from, i, label, edges, edgeSet);
      return;
    }
  }

  rom_disasm_enqueueSyntheticSuccessor(from, label, fallbackLabel, nodes, syntheticLookup, edges, edgeSet);
}

static void rom_disasm_escapeDot(FILE* out, const char* text) {
  for(const char* p = text; *p != '\0'; ++p) {
    switch(*p) {
      case '\\':
      case '"':
        fputc('\\', out);
        fputc(*p, out);
        break;
      case '\n':
        fputs("\\n", out);
        break;
      default:
        fputc(*p, out);
        break;
    }
  }
}

static void rom_disasm_writeFunctionLabel(FILE* out, uint32_t entry, const RomFunctionInfo* info) {
  fprintf(out, "%06x", entry & 0xffffff);
  if(info != NULL && info->recursive) {
    if(info->recursionGroup > 0) {
      fprintf(out, "\\nmutually-recursive group %d", info->recursionGroup);
    } else {
      fprintf(out, "\\nrecursive");
    }
  }
}

static void rom_disasm_writeJsonEscaped(FILE* out, const char* text) {
  for(const char* p = text; *p != '\0'; ++p) {
    switch(*p) {
      case '\\':
      case '"':
        fputc('\\', out);
        fputc(*p, out);
        break;
      case '\n':
        fputs("\\n", out);
        break;
      case '\r':
        fputs("\\r", out);
        break;
      case '\t':
        fputs("\\t", out);
        break;
      default:
        fputc(*p, out);
        break;
    }
  }
}

static void rom_disasm_mnemonicColor(const char* mnemonic, char* color, size_t colorSize) {
  uint32_t hash = 2166136261u;
  if(mnemonic != NULL) {
    for(size_t i = 0; mnemonic[i] != '\0'; ++i) {
      hash ^= (uint8_t)mnemonic[i];
      hash *= 16777619u;
    }
  }

  // Spread mnemonics around the full hue circle while keeping saturation/value readable.
  const double hue = (double)(hash % 360u);
  const double saturation = 0.55 + (double)((hash >> 9) & 0x1f) / 100.0;   // 0.55 - 0.86
  const double value = 0.72 + (double)((hash >> 17) & 0x0f) / 50.0;        // 0.72 - 1.02
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

static void rom_disasm_writeNotesJson(FILE* out, const std::vector<RomAnalysisNode>& nodes) {
  if(out == NULL) {
    return;
  }

  fprintf(out, "{\n  \"annotations\": [\n");
  bool first = true;
  for(size_t i = 0; i < nodes.size(); ++i) {
    if(nodes[i].synthetic || !nodes[i].hasFileOffset || nodes[i].info.size == 0) {
      continue;
    }
    if(!first) {
      fprintf(out, ",\n");
    }
    first = false;
    fprintf(out, "    {\n");
    fprintf(out, "      \"positions\": [");
    for(uint8_t j = 0; j < nodes[i].info.size; ++j) {
      if(j != 0) {
        fprintf(out, ", ");
      }
      fprintf(out, "\"0x%x\"", (unsigned)(nodes[i].fileOffset + j));
    }
    fprintf(out, "],\n");
    fprintf(out, "      \"note\": \"");
    char note[128];
    char color[16];
    snprintf(note, sizeof(note), "%06x: %s", nodes[i].info.address & 0xffffff, nodes[i].info.formatted);
    rom_disasm_mnemonicColor(nodes[i].info.mnemonic, color, sizeof(color));
    rom_disasm_writeJsonEscaped(out, note);
    fprintf(out, "\",\n");
    fprintf(out, "      \"color\": \"%s\"\n", color);
    fprintf(out, "    }");
  }
  fprintf(out, "\n  ]\n}\n");
}

static void rom_disasm_reportProgress(
  const RomDisassemblyControl* control,
  size_t nodes,
  size_t edges,
  size_t unresolvedIndirectJumpCount,
  size_t unresolvedIndirectCallCount,
  size_t unresolvedReturnCount,
  size_t recursiveCallCount,
  size_t mutualRecursiveCallCount,
  size_t maxCallDepthCount,
  size_t contextLimitCount,
  size_t uniqueInstructionAddresses,
  size_t discoveredFunctions,
  size_t reachableRomBytes,
  double romCoveragePercent,
  size_t processedNodes,
  size_t queuedNodes,
  int instructionLimit,
  bool hitNodeLimit,
  bool stopRequested,
  bool completed
) {
  if(control == NULL || control->statusCallback == NULL) {
    return;
  }

  RomDisassemblyProgress progress = {};
  progress.nodes = nodes;
  progress.edges = edges;
  progress.unresolvedIndirectJumps = unresolvedIndirectJumpCount;
  progress.unresolvedIndirectCalls = unresolvedIndirectCallCount;
  progress.unresolvedReturns = unresolvedReturnCount;
  progress.recursiveCallsCutOff = recursiveCallCount;
  progress.mutualRecursiveCallsCutOff = mutualRecursiveCallCount;
  progress.maxCallDepthCutOff = maxCallDepthCount;
  progress.contextLimitCutOff = contextLimitCount;
  progress.uniqueInstructionAddresses = uniqueInstructionAddresses;
  progress.discoveredFunctions = discoveredFunctions;
  progress.reachableRomBytes = reachableRomBytes;
  progress.romCoveragePercent = romCoveragePercent;
  progress.processedNodes = processedNodes;
  progress.queuedNodes = queuedNodes;
  progress.instructionLimit = instructionLimit;
  progress.hitNodeLimit = hitNodeLimit;
  progress.stopRequested = stopRequested;
  progress.completed = completed;
  control->statusCallback(control->userData, &progress);
}

bool rom_disassemble(Snes* snes, FILE* out, int instructionLimit) {
  if(snes == NULL || out == NULL || instructionLimit <= 0) {
    return false;
  }

  uint16_t pc = snes_read(snes, 0xfffc) | (snes_read(snes, 0xfffd) << 8);
  uint8_t bank = 0x00;
  bool e = true;
  bool mf = true;
  bool xf = true;
  bool c = false;
  bool cKnown = true;

  for(int i = 0; i < instructionLimit; ++i) {
    uint32_t address = (bank << 16) | pc;
    uint8_t bytes[4] = {
      snes_read(snes, address),
      snes_read(snes, (address + 1) & 0xffffff),
      snes_read(snes, (address + 2) & 0xffffff),
      snes_read(snes, (address + 3) & 0xffffff)
    };
    uint8_t size = cpu_getInstructionSize(bytes[0], mf, xf);
    CpuInstructionInfo info = {};
    cpu_disassembleInstruction(address, mf, xf, bytes, size, &info);
    rom_disasm_format(&info, out);

    switch(bytes[0]) {
      case 0x18:
        c = false;
        cKnown = true;
        break;
      case 0x38:
        c = true;
        cKnown = true;
        break;
      case 0xc2:
        if(size > 1) {
          mf = mf && ((bytes[1] & 0x20) == 0);
          xf = xf && ((bytes[1] & 0x10) == 0);
          if(e) {
            mf = true;
            xf = true;
          }
        }
        break;
      case 0xe2:
        if(size > 1) {
          if(bytes[1] & 0x20) mf = true;
          if(bytes[1] & 0x10) xf = true;
        }
        break;
      case 0xfb:
        if(cKnown) {
          bool newCarry = e;
          e = c;
          c = newCarry;
          if(e) {
            mf = true;
            xf = true;
          }
        }
        break;
      default:
        break;
    }

    pc = static_cast<uint16_t>(pc + size);
  }

  return true;
}

bool rom_disassemble_cfg_with_outputs(Snes* snes, FILE* out, FILE* notesOut, int instructionLimit, const RomDisassemblyControl* control) {
  if(snes == NULL || out == NULL || instructionLimit < 0) {
    return false;
  }

  RomAnalysisState initialState = {};
  initialState.address = snes_read(snes, 0xfffc) | (snes_read(snes, 0xfffd) << 8);
  initialState.e = true;
  initialState.mf = true;
  initialState.xf = true;
  initialState.c = false;
  initialState.cKnown = true;
  initialState.callTargetStack.push_back(initialState.address);

  std::vector<RomAnalysisNode> nodes;
  std::unordered_map<std::string, size_t> nodeLookup;
  std::vector<size_t> worklist;
  std::unordered_map<uint32_t, size_t> addressContextCounts;
  std::vector<RomAnalysisEdge> edges;
  std::unordered_set<std::string> edgeSet;
  std::unordered_map<std::string, size_t> syntheticLookup;
  std::unordered_map<uint32_t, std::unordered_set<uint32_t> > functionCalls;
  std::unordered_set<uint32_t> uniqueInstructionAddresses;
  size_t unresolvedIndirectJumpCount = 0;
  size_t unresolvedIndirectCallCount = 0;
  size_t unresolvedReturnCount = 0;
  size_t recursiveCallCount = 0;
  size_t mutualRecursiveCallCount = 0;
  size_t maxCallDepthCount = 0;
  size_t contextLimitCount = 0;
  bool hitNodeLimit = false;
  bool stopRequested = false;
  size_t lastStatusNodeCount = 0;
  size_t processedNodeCount = 0;

  if(!rom_disasm_isRomAddress(snes, initialState.address)) {
    return false;
  }
  if(rom_disasm_getOrCreateNode(snes, initialState, instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts) == kNodeLimitReached) {
    return false;
  }

  for(size_t cursor = 0; cursor < worklist.size(); ++cursor) {
    if(control != NULL && control->stopRequested != NULL && *control->stopRequested != 0) {
      stopRequested = true;
      rom_disasm_reportProgress(
        control,
        nodes.size(),
        edges.size(),
        unresolvedIndirectJumpCount,
        unresolvedIndirectCallCount,
        unresolvedReturnCount,
        recursiveCallCount,
        mutualRecursiveCallCount,
        maxCallDepthCount,
        contextLimitCount,
        uniqueInstructionAddresses.size(),
        functionCalls.size(),
        0,
        0.0,
        processedNodeCount,
        worklist.size(),
        instructionLimit,
        hitNodeLimit,
        true,
        false
      );
      break;
    }

    const size_t nodeIndex = worklist[cursor];
    const RomAnalysisNode& node = nodes[nodeIndex];
    const RomAnalysisState nextState = rom_disasm_advanceState(node.state, node.info);
    const uint32_t currentFunctionEntry = rom_disasm_currentFunctionEntry(node.state);
    uniqueInstructionAddresses.insert(node.info.address & 0xffffff);

    switch(node.info.opcode) {
      case 0x10:
      case 0x30:
      case 0x50:
      case 0x70:
      case 0x90:
      case 0xb0:
      case 0xd0:
      case 0xf0: {
        RomAnalysisState branchState = nextState;
        branchState.address = rom_disasm_relativeTarget(node.info);
        rom_disasm_enqueueSuccessor(snes, branchState, nodeIndex, "branch", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        rom_disasm_enqueueSuccessor(snes, nextState, nodeIndex, "fallthrough", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        break;
      }
      case 0x80: {
        RomAnalysisState branchState = nextState;
        branchState.address = rom_disasm_relativeTarget(node.info);
        rom_disasm_enqueueSuccessor(snes, branchState, nodeIndex, "jump", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        break;
      }
      case 0x82: {
        RomAnalysisState branchState = nextState;
        branchState.address = rom_disasm_relativeLongTarget(node.info);
        rom_disasm_enqueueSuccessor(snes, branchState, nodeIndex, "jump", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        break;
      }
      case 0x20: {
        RomAnalysisState callState = nextState;
        callState.address = rom_disasm_absoluteTarget(node.info);
        functionCalls[currentFunctionEntry].insert(callState.address & 0xffffff);
        const bool directRecursion =
          !node.state.callTargetStack.empty() &&
          ((node.state.callTargetStack.back() & 0xffffff) == (callState.address & 0xffffff));
        if(callState.callTargetStack.size() >= kMaxCallStackDepth) {
          maxCallDepthCount++;
          rom_disasm_enqueueSyntheticSuccessor(nodeIndex, "call-depth-limit", std::string("max call depth reached\\n") + node.info.formatted, &nodes, &syntheticLookup, &edges, &edgeSet);
        } else if(rom_disasm_callStackContains(node.state, callState.address)) {
          if(directRecursion) {
            recursiveCallCount++;
            rom_disasm_enqueueSyntheticSuccessor(nodeIndex, "recursive-call", std::string("recursive call\\n") + node.info.formatted, &nodes, &syntheticLookup, &edges, &edgeSet);
          } else {
            mutualRecursiveCallCount++;
            rom_disasm_enqueueSyntheticSuccessor(nodeIndex, "mutual-recursion", std::string("mutual recursion\\n") + node.info.formatted, &nodes, &syntheticLookup, &edges, &edgeSet);
          }
        } else {
          callState.returnStack.push_back(rom_disasm_makeReturnFrame(node.info.address, nextState.address));
          callState.callTargetStack.push_back(callState.address);
          rom_disasm_enqueueSuccessor(snes, callState, nodeIndex, "call", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        }
        rom_disasm_enqueueSuccessor(snes, nextState, nodeIndex, "fallthrough", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        break;
      }
      case 0x22: {
        RomAnalysisState callState = nextState;
        callState.address = rom_disasm_longTarget(node.info);
        functionCalls[currentFunctionEntry].insert(callState.address & 0xffffff);
        const bool directRecursion =
          !node.state.callTargetStack.empty() &&
          ((node.state.callTargetStack.back() & 0xffffff) == (callState.address & 0xffffff));
        if(callState.callTargetStack.size() >= kMaxCallStackDepth) {
          maxCallDepthCount++;
          rom_disasm_enqueueSyntheticSuccessor(nodeIndex, "call-depth-limit", std::string("max call depth reached\\n") + node.info.formatted, &nodes, &syntheticLookup, &edges, &edgeSet);
        } else if(rom_disasm_callStackContains(node.state, callState.address)) {
          if(directRecursion) {
            recursiveCallCount++;
            rom_disasm_enqueueSyntheticSuccessor(nodeIndex, "recursive-call", std::string("recursive call\\n") + node.info.formatted, &nodes, &syntheticLookup, &edges, &edgeSet);
          } else {
            mutualRecursiveCallCount++;
            rom_disasm_enqueueSyntheticSuccessor(nodeIndex, "mutual-recursion", std::string("mutual recursion\\n") + node.info.formatted, &nodes, &syntheticLookup, &edges, &edgeSet);
          }
        } else {
          callState.returnStack.push_back(rom_disasm_makeReturnFrame(node.info.address, nextState.address));
          callState.callTargetStack.push_back(callState.address);
          rom_disasm_enqueueSuccessor(snes, callState, nodeIndex, "call", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        }
        rom_disasm_enqueueSuccessor(snes, nextState, nodeIndex, "fallthrough", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        break;
      }
      case 0x4c: {
        RomAnalysisState jumpState = nextState;
        jumpState.address = rom_disasm_absoluteTarget(node.info);
        rom_disasm_enqueueSuccessor(snes, jumpState, nodeIndex, "jump", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        break;
      }
      case 0x5c: {
        RomAnalysisState jumpState = nextState;
        jumpState.address = rom_disasm_longTarget(node.info);
        rom_disasm_enqueueSuccessor(snes, jumpState, nodeIndex, "jump", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        break;
      }
      case 0xfc:
        unresolvedIndirectCallCount++;
        rom_disasm_enqueueSyntheticSuccessor(
          nodeIndex,
          "indirect-call",
          std::string("unresolved indirect call\\n") + node.info.formatted,
          &nodes,
          &syntheticLookup,
          &edges,
          &edgeSet
        );
        rom_disasm_enqueueSuccessor(snes, nextState, nodeIndex, "fallthrough", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        break;
      case 0x6c:
      case 0x7c:
      case 0xdc:
        unresolvedIndirectJumpCount++;
        rom_disasm_enqueueSyntheticSuccessor(
          nodeIndex,
          "indirect-jump",
          std::string("unresolved indirect jump\\n") + node.info.formatted,
          &nodes,
          &syntheticLookup,
          &edges,
          &edgeSet
        );
        break;
      case 0x00:
      case 0x02:
      case 0x40:
      case 0x60:
      case 0x6b:
      case 0xcb:
      case 0xdb:
        if((node.info.opcode == 0x60 || node.info.opcode == 0x6b) && !node.state.returnStack.empty()) {
          RomAnalysisState returnState = node.state;
          const uint64_t frame = returnState.returnStack.back();
          const uint32_t returnAddress = rom_disasm_getReturnAddress(frame);
          const uint32_t callAddress = rom_disasm_getCallAddress(frame);
          returnState.address = returnAddress;
          returnState.returnStack.pop_back();
          if(!returnState.callTargetStack.empty()) {
            returnState.callTargetStack.pop_back();
          }
          rom_disasm_enqueueSuccessor(snes, returnState, nodeIndex, "return", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
          rom_disasm_enqueueCallsiteEdge(
            callAddress,
            nodeIndex,
            "return-callsite",
            std::string("callsite ") + node.info.formatted,
            &nodes,
            &syntheticLookup,
            &edges,
            &edgeSet
          );
        } else if(node.info.opcode == 0x60 || node.info.opcode == 0x6b) {
          unresolvedReturnCount++;
          rom_disasm_enqueueSyntheticSuccessor(
            nodeIndex,
            "return",
            std::string("unresolved return\\n") + node.info.formatted,
            &nodes,
            &syntheticLookup,
            &edges,
            &edgeSet
          );
        }
        break;
      default:
        rom_disasm_enqueueSuccessor(snes, nextState, nodeIndex, "fallthrough", instructionLimit, &nodes, &nodeLookup, &worklist, &addressContextCounts, &syntheticLookup, &edges, &edgeSet, &contextLimitCount);
        break;
    }

    if(instructionLimit > 0 && nodes.size() >= (size_t)instructionLimit) {
      hitNodeLimit = true;
    }
    processedNodeCount = cursor + 1;

    const bool statusRequested =
      control != NULL &&
      control->statusRequested != NULL &&
      *control->statusRequested != 0;
    const bool intervalReached =
      control != NULL &&
      control->statusCallback != NULL &&
      control->statusInterval > 0 &&
      nodes.size() >= lastStatusNodeCount + control->statusInterval;

    if(statusRequested || intervalReached) {
      rom_disasm_reportProgress(
        control,
        nodes.size(),
        edges.size(),
        unresolvedIndirectJumpCount,
        unresolvedIndirectCallCount,
        unresolvedReturnCount,
        recursiveCallCount,
        mutualRecursiveCallCount,
        maxCallDepthCount,
        contextLimitCount,
        uniqueInstructionAddresses.size(),
        functionCalls.size(),
        0,
        0.0,
        processedNodeCount,
        worklist.size(),
        instructionLimit,
        hitNodeLimit,
        false,
        false
      );
      lastStatusNodeCount = nodes.size();
      if(control != NULL && control->statusRequested != NULL) {
        *control->statusRequested = 0;
      }
    }
  }

  std::unordered_map<uint32_t, RomFunctionInfo> functionInfos;
  rom_disasm_collectFunctionInfos(functionCalls, &functionInfos);
  size_t recursiveFunctionCount = 0;
  size_t mutualRecursiveFunctionCount = 0;
  size_t recursionGroupCount = 0;
  std::unordered_set<uint32_t> reachableRomOffsets;
  std::unordered_set<int> seenGroups;
  for(std::unordered_map<uint32_t, RomFunctionInfo>::const_iterator it = functionInfos.begin(); it != functionInfos.end(); ++it) {
    if(it->second.recursive) {
      recursiveFunctionCount++;
      if(it->second.recursionGroup > 0) {
        mutualRecursiveFunctionCount++;
        seenGroups.insert(it->second.recursionGroup);
      }
    }
  }
  recursionGroupCount = seenGroups.size();
  for(size_t i = 0; i < nodes.size(); ++i) {
    if(!nodes[i].synthetic && nodes[i].hasFileOffset) {
      for(uint8_t j = 0; j < nodes[i].info.size; ++j) {
        reachableRomOffsets.insert((nodes[i].fileOffset + j) & 0xffffff);
      }
    }
  }
  const size_t reachableRomBytes = reachableRomOffsets.size();
  const double romCoveragePercent =
    (snes->cart != NULL && snes->cart->romSize > 0)
      ? (100.0 * reachableRomBytes) / (double)snes->cart->romSize
      : 0.0;

  rom_disasm_reportProgress(
    control,
    nodes.size(),
    edges.size(),
    unresolvedIndirectJumpCount,
    unresolvedIndirectCallCount,
    unresolvedReturnCount,
    recursiveCallCount,
    mutualRecursiveCallCount,
    maxCallDepthCount,
    contextLimitCount,
    uniqueInstructionAddresses.size(),
    functionInfos.size(),
    reachableRomBytes,
    romCoveragePercent,
    processedNodeCount,
    worklist.size(),
    instructionLimit,
    hitNodeLimit,
    stopRequested,
    true
  );

  fprintf(out, "digraph rom_cfg {\n");
  fprintf(out, "  // nodes: %zu\n", nodes.size());
  fprintf(out, "  // edges: %zu\n", edges.size());
  fprintf(out, "  // unresolved indirect jumps: %zu\n", unresolvedIndirectJumpCount);
  fprintf(out, "  // unresolved indirect calls: %zu\n", unresolvedIndirectCallCount);
  fprintf(out, "  // unresolved returns: %zu\n", unresolvedReturnCount);
  fprintf(out, "  // recursive functions: %zu\n", recursiveFunctionCount);
  fprintf(out, "  // mutually recursive functions: %zu\n", mutualRecursiveFunctionCount);
  fprintf(out, "  // mutually recursive groups: %zu\n", recursionGroupCount);
  fprintf(out, "  // recursive calls cut off: %zu\n", recursiveCallCount);
  fprintf(out, "  // mutually recursive calls cut off: %zu\n", mutualRecursiveCallCount);
  fprintf(out, "  // max call depth cut off: %zu\n", maxCallDepthCount);
  fprintf(out, "  // context limit cut off: %zu\n", contextLimitCount);
  fprintf(out, "  // unique instruction addresses: %zu\n", uniqueInstructionAddresses.size());
  fprintf(out, "  // discovered functions: %zu\n", functionInfos.size());
  fprintf(out, "  // reachable ROM bytes: %zu\n", reachableRomBytes);
  fprintf(out, "  // ROM coverage: %.4f%%\n", romCoveragePercent);
  if(instructionLimit == 0) {
    fprintf(out, "  // limit: none\n");
  } else {
    fprintf(out, "  // limit: %d\n", instructionLimit);
  }
  if(stopRequested) {
    fprintf(out, "  // traversal: stopped by signal\n");
  } else {
    fprintf(out, "  // traversal: %s\n", hitNodeLimit ? "stopped at limit" : "worklist exhausted");
  }
  fprintf(out, "  node [shape=box];\n");

  for(size_t i = 0; i < nodes.size(); ++i) {
    if(nodes[i].synthetic) {
      fprintf(out, "  n%zu [label=\"", i);
      rom_disasm_escapeDot(out, nodes[i].syntheticLabel.c_str());
      fprintf(out, "\", style=dashed];\n");
    } else {
      fprintf(out, "  n%zu [label=\"%06x", i, nodes[i].info.address & 0xffffff);
      if(nodes[i].hasFileOffset) {
        fprintf(out, " [file@%06x]", nodes[i].fileOffset & 0xffffff);
      }
      fprintf(out, "\\n");
      rom_disasm_escapeDot(out, nodes[i].info.formatted);
      const uint32_t functionEntry = rom_disasm_currentFunctionEntry(nodes[i].state);
      std::unordered_map<uint32_t, RomFunctionInfo>::const_iterator functionIt = functionInfos.find(functionEntry);
      if(functionIt != functionInfos.end() && functionIt->second.recursive) {
        if(functionIt->second.recursionGroup > 0) {
          fprintf(out, "\\nfunc %06x mutually-recursive group %d", functionEntry, functionIt->second.recursionGroup);
        } else {
          fprintf(out, "\\nfunc %06x recursive", functionEntry);
        }
      }
      fprintf(out, "\"];\n");
    }
  }

  for(size_t i = 0; i < edges.size(); ++i) {
    fprintf(out, "  n%zu -> n%zu [label=\"%s\"];\n", edges[i].from, edges[i].to, edges[i].label);
  }

  fprintf(out, "  subgraph cluster_function_call_graph {\n");
  fprintf(out, "    label=\"Function Call Graph\";\n");
  fprintf(out, "    color=gray50;\n");
  fprintf(out, "    style=dashed;\n");

  for(std::unordered_map<uint32_t, RomFunctionInfo>::const_iterator it = functionInfos.begin(); it != functionInfos.end(); ++it) {
    fprintf(out, "    f_%06x [shape=ellipse, label=\"", it->first & 0xffffff);
    rom_disasm_writeFunctionLabel(out, it->first, &it->second);
    if(it->second.recursionGroup > 0) {
      fprintf(out, "\", color=firebrick, fontcolor=firebrick];\n");
    } else if(it->second.recursive) {
      fprintf(out, "\", color=darkorange3, fontcolor=darkorange3];\n");
    } else {
      fprintf(out, "\"];\n");
    }
  }

  for(std::unordered_map<uint32_t, std::unordered_set<uint32_t> >::const_iterator it = functionCalls.begin(); it != functionCalls.end(); ++it) {
    for(std::unordered_set<uint32_t>::const_iterator targetIt = it->second.begin(); targetIt != it->second.end(); ++targetIt) {
      const uint32_t fromEntry = it->first & 0xffffff;
      const uint32_t toEntry = *targetIt & 0xffffff;
      std::unordered_map<uint32_t, RomFunctionInfo>::const_iterator fromInfoIt = functionInfos.find(fromEntry);
      std::unordered_map<uint32_t, RomFunctionInfo>::const_iterator toInfoIt = functionInfos.find(toEntry);
      const RomFunctionInfo* fromInfo = fromInfoIt != functionInfos.end() ? &fromInfoIt->second : NULL;
      const RomFunctionInfo* toInfo = toInfoIt != functionInfos.end() ? &toInfoIt->second : NULL;
      const bool sameRecursiveGroup =
        fromInfo != NULL &&
        toInfo != NULL &&
        fromInfo->recursionGroup > 0 &&
        fromInfo->recursionGroup == toInfo->recursionGroup;
      if(sameRecursiveGroup) {
        fprintf(out, "    f_%06x -> f_%06x [color=firebrick, penwidth=2];\n", fromEntry, toEntry);
      } else {
        fprintf(out, "    f_%06x -> f_%06x;\n", fromEntry, toEntry);
      }
    }
  }

  fprintf(out, "  }\n");

  fprintf(out, "}\n");
  rom_disasm_writeNotesJson(notesOut, nodes);
  return true;
}

bool rom_disassemble_cfg(Snes* snes, FILE* out, int instructionLimit) {
  return rom_disassemble_cfg_with_outputs(snes, out, NULL, instructionLimit, NULL);
}

bool rom_disassemble_cfg_with_control(Snes* snes, FILE* out, int instructionLimit, const RomDisassemblyControl* control) {
  return rom_disassemble_cfg_with_outputs(snes, out, NULL, instructionLimit, control);
}

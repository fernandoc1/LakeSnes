#include <stdio.h>
#include <stdint.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rom_disasm.h"
#include "cpu.h"

struct RomAnalysisState {
  uint32_t address;
  bool e;
  bool mf;
  bool xf;
  bool c;
  bool cKnown;
  std::vector<uint64_t> returnStack;
};

struct RomAnalysisNode {
  RomAnalysisState state;
  CpuInstructionInfo info;
  bool synthetic;
  std::string syntheticLabel;
};

struct RomAnalysisEdge {
  size_t from;
  size_t to;
  const char* label;
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
  for(size_t i = 0; i < state.returnStack.size(); ++i) {
    const uint32_t returnAddress = state.returnStack[i] & 0xffffff;
    const uint32_t callAddress = (state.returnStack[i] >> 24) & 0xffffff;
    snprintf(entry, sizeof(entry), ":%06x>%06x", callAddress, returnAddress);
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

static size_t rom_disasm_getOrCreateNode(
  Snes* snes,
  const RomAnalysisState& state,
  int instructionLimit,
  std::vector<RomAnalysisNode>* nodes,
  std::unordered_map<std::string, size_t>* nodeLookup,
  std::vector<size_t>* worklist
) {
  const std::string key = rom_disasm_stateKey(state);
  std::unordered_map<std::string, size_t>::const_iterator it = nodeLookup->find(key);
  if(it != nodeLookup->end()) {
    return it->second;
  }
  if((int)nodes->size() >= instructionLimit) {
    return (size_t)-1;
  }

  RomAnalysisNode node = {};
  node.state = state;
  rom_disasm_readInstruction(snes, state, &node.info);
  node.synthetic = false;
  const size_t index = nodes->size();
  nodes->push_back(node);
  (*nodeLookup)[key] = index;
  worklist->push_back(index);
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
  if(to == (size_t)-1) {
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
  std::vector<RomAnalysisEdge>* edges,
  std::unordered_set<std::string>* edgeSet
) {
  if(!rom_disasm_isRomAddress(snes, state.address)) {
    return;
  }

  const size_t to = rom_disasm_getOrCreateNode(snes, state, instructionLimit, nodes, nodeLookup, worklist);
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

bool rom_disassemble_cfg(Snes* snes, FILE* out, int instructionLimit) {
  if(snes == NULL || out == NULL || instructionLimit <= 0) {
    return false;
  }

  RomAnalysisState initialState = {};
  initialState.address = snes_read(snes, 0xfffc) | (snes_read(snes, 0xfffd) << 8);
  initialState.e = true;
  initialState.mf = true;
  initialState.xf = true;
  initialState.c = false;
  initialState.cKnown = true;

  std::vector<RomAnalysisNode> nodes;
  std::unordered_map<std::string, size_t> nodeLookup;
  std::vector<size_t> worklist;
  std::vector<RomAnalysisEdge> edges;
  std::unordered_set<std::string> edgeSet;
  std::unordered_map<std::string, size_t> syntheticLookup;

  if(!rom_disasm_isRomAddress(snes, initialState.address)) {
    return false;
  }
  if(rom_disasm_getOrCreateNode(snes, initialState, instructionLimit, &nodes, &nodeLookup, &worklist) == (size_t)-1) {
    return false;
  }

  for(size_t cursor = 0; cursor < worklist.size(); ++cursor) {
    const size_t nodeIndex = worklist[cursor];
    const RomAnalysisNode& node = nodes[nodeIndex];
    const RomAnalysisState nextState = rom_disasm_advanceState(node.state, node.info);

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
        rom_disasm_enqueueSuccessor(snes, branchState, nodeIndex, "branch", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        rom_disasm_enqueueSuccessor(snes, nextState, nodeIndex, "fallthrough", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        break;
      }
      case 0x80: {
        RomAnalysisState branchState = nextState;
        branchState.address = rom_disasm_relativeTarget(node.info);
        rom_disasm_enqueueSuccessor(snes, branchState, nodeIndex, "jump", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        break;
      }
      case 0x82: {
        RomAnalysisState branchState = nextState;
        branchState.address = rom_disasm_relativeLongTarget(node.info);
        rom_disasm_enqueueSuccessor(snes, branchState, nodeIndex, "jump", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        break;
      }
      case 0x20: {
        RomAnalysisState callState = nextState;
        callState.address = rom_disasm_absoluteTarget(node.info);
        callState.returnStack.push_back(rom_disasm_makeReturnFrame(node.info.address, nextState.address));
        rom_disasm_enqueueSuccessor(snes, callState, nodeIndex, "call", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        rom_disasm_enqueueSuccessor(snes, nextState, nodeIndex, "fallthrough", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        break;
      }
      case 0x22: {
        RomAnalysisState callState = nextState;
        callState.address = rom_disasm_longTarget(node.info);
        callState.returnStack.push_back(rom_disasm_makeReturnFrame(node.info.address, nextState.address));
        rom_disasm_enqueueSuccessor(snes, callState, nodeIndex, "call", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        rom_disasm_enqueueSuccessor(snes, nextState, nodeIndex, "fallthrough", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        break;
      }
      case 0x4c: {
        RomAnalysisState jumpState = nextState;
        jumpState.address = rom_disasm_absoluteTarget(node.info);
        rom_disasm_enqueueSuccessor(snes, jumpState, nodeIndex, "jump", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        break;
      }
      case 0x5c: {
        RomAnalysisState jumpState = nextState;
        jumpState.address = rom_disasm_longTarget(node.info);
        rom_disasm_enqueueSuccessor(snes, jumpState, nodeIndex, "jump", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        break;
      }
      case 0xfc:
        rom_disasm_enqueueSyntheticSuccessor(
          nodeIndex,
          "indirect-call",
          std::string("unresolved indirect call\\n") + node.info.formatted,
          &nodes,
          &syntheticLookup,
          &edges,
          &edgeSet
        );
        rom_disasm_enqueueSuccessor(snes, nextState, nodeIndex, "fallthrough", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        break;
      case 0x6c:
      case 0x7c:
      case 0xdc:
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
          rom_disasm_enqueueSuccessor(snes, returnState, nodeIndex, "return", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
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
        rom_disasm_enqueueSuccessor(snes, nextState, nodeIndex, "fallthrough", instructionLimit, &nodes, &nodeLookup, &worklist, &edges, &edgeSet);
        break;
    }
  }

  fprintf(out, "digraph rom_cfg {\n");
  fprintf(out, "  node [shape=box];\n");

  for(size_t i = 0; i < nodes.size(); ++i) {
    if(nodes[i].synthetic) {
      fprintf(out, "  n%zu [label=\"", i);
      rom_disasm_escapeDot(out, nodes[i].syntheticLabel.c_str());
      fprintf(out, "\", style=dashed];\n");
    } else {
      fprintf(out, "  n%zu [label=\"%06x\\n", i, nodes[i].info.address & 0xffffff);
      rom_disasm_escapeDot(out, nodes[i].info.formatted);
      fprintf(out, "\"];\n");
    }
  }

  for(size_t i = 0; i < edges.size(); ++i) {
    fprintf(out, "  n%zu -> n%zu [label=\"%s\"];\n", edges[i].from, edges[i].to, edges[i].label);
  }

  fprintf(out, "}\n");
  return true;
}

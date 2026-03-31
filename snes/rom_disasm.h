#ifndef ROM_DISASM_H
#define ROM_DISASM_H

#include <stdio.h>
#include <stdbool.h>
#include <signal.h>

#include "snes.h"

typedef struct RomDisassemblyProgress {
  size_t nodes;
  size_t edges;
  size_t unresolvedIndirectJumps;
  size_t unresolvedIndirectCalls;
  size_t unresolvedReturns;
  size_t recursiveCallsCutOff;
  size_t mutualRecursiveCallsCutOff;
  size_t maxCallDepthCutOff;
  size_t contextLimitCutOff;
  size_t processedNodes;
  size_t queuedNodes;
  int instructionLimit;
  bool hitNodeLimit;
  bool stopRequested;
  bool completed;
} RomDisassemblyProgress;

typedef void (*RomDisassemblyStatusCallback)(void* userData, const RomDisassemblyProgress* progress);

typedef struct RomDisassemblyControl {
  volatile sig_atomic_t* stopRequested;
  volatile sig_atomic_t* statusRequested;
  RomDisassemblyStatusCallback statusCallback;
  void* userData;
  size_t statusInterval;
} RomDisassemblyControl;

bool rom_disassemble(Snes* snes, FILE* out, int instructionLimit);
bool rom_disassemble_cfg(Snes* snes, FILE* out, int instructionLimit);
bool rom_disassemble_cfg_with_control(Snes* snes, FILE* out, int instructionLimit, const RomDisassemblyControl* control);

#endif

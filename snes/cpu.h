
#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>

#include "statehandler.h"

#include "mem_viewer.h"

typedef struct Snes Snes;
class Cpu;

typedef uint8_t (*CpuReadHandler)(Snes* snes, uint32_t adr);
typedef void (*CpuWriteHandler)(Snes* snes, uint32_t adr, uint8_t val);
typedef void (*CpuIdleHandler)(Snes* snes, bool waiting);

#define LAKESNES_COPROCESSOR_HOOK_SYMBOL "lakesnes_cop_execute"

struct CpuInstructionInfo {
  uint32_t address;
  uint8_t opcode;
  uint8_t size;
  uint8_t bytes[4];
  char mnemonic[16];
  char operands[32];
  char formatted[48];
};

typedef void (*CpuInstructionHook)(void* userData, const Cpu* cpu, const CpuInstructionInfo* info);
typedef bool (*CpuCoprocessorHook)(void* userData, Snes* snes, Cpu* cpu, uint8_t address, const uint8_t* data, uint16_t size);

struct ProgramCounter {
private:
  uint16_t previousValue; // for debugging
  uint16_t value;
public:
    ProgramCounter(uint16_t v = 0) : previousValue(v), value(v) {}
    ProgramCounter& operator=(uint16_t v) {
        previousValue = value;
        value = v;
        return *this;
    }

    operator uint16_t() const {
        return value;
    }

    ProgramCounter& operator++() {
        previousValue = value;
        ++value;
        return *this;
    }

    ProgramCounter operator++(int) {
        ProgramCounter tmp = *this;
        previousValue = value;
        ++value;
        return tmp;
    }

    ProgramCounter& operator--() {
        previousValue = value;
        --value;
        return *this;
    }

    ProgramCounter& operator+=(uint16_t v) {
        previousValue = value;
        value += v;
        return *this;
    }

    ProgramCounter& operator-=(uint16_t v) {
        previousValue = value;
        value -= v;
        return *this;
    }

    uint16_t raw() const { return value; }
    uint16_t previous() const { return previousValue; }
};

class Cpu {
public:
  Snes* snes;
  CpuReadHandler read;
  CpuWriteHandler write;
  CpuIdleHandler idle;
  // registers
  uint16_t a;
  uint16_t x;
  uint16_t y;
  uint16_t sp;
  struct ProgramCounter pc;
  uint16_t dp; // direct page (D)
  uint8_t k; // program bank (PB)
  uint8_t db; // data bank (B)
  // flags
  bool c;
  bool z;
  bool v;
  bool n;
  bool i;
  bool d;
  bool xf;
  bool mf;
  bool e;
  // power state (WAI/STP)
  bool waiting;
  bool stopped;
  // interrupts
  bool irqWanted;
  bool nmiWanted;
  bool intWanted;
  bool resetWanted;
  // Coprocessor state
  uint8_t cop_mem[256];
  uint8_t cop_addr;
  uint16_t cop_size;

  MemViewer* copViewer;
  MemViewer* memViewer;

  //Execution Map
  uint8_t executionMap[0x10000];
  MemViewer* executionMapViewer;

  Cpu(Snes* snes, CpuReadHandler read, CpuWriteHandler write, CpuIdleHandler idle);
  ~Cpu();

  void reset(bool hard);
  void handleState(StateHandler* sh);
  void runOpcode();
  void nmi();
  void setIrq(bool state);
  void setInstructionHook(CpuInstructionHook hook, void* userData);
  void setCoprocessorHook(CpuCoprocessorHook hook, void* userData);
  bool runCoprocessorHook();
  void appendInstructionByte(uint8_t value);

private:
  CpuInstructionHook instructionHook;
  void* instructionHookUserData;
  CpuCoprocessorHook coprocessorHook;
  void* coprocessorHookUserData;
  uint32_t tracedInstructionAddress;
  bool tracedInstructionMf;
  bool tracedInstructionXf;
  uint8_t tracedInstructionBytes[4];
  uint8_t tracedInstructionSize;

  void beginInstructionTrace();
  void emitInstructionTrace();
};

Cpu* cpu_init(Snes* snes, CpuReadHandler read, CpuWriteHandler write, CpuIdleHandler idle);
void cpu_free(Cpu* cpu);
void cpu_reset(Cpu* cpu, bool hard);
void cpu_handleState(Cpu* cpu, StateHandler* sh);
void cpu_runOpcode(Cpu* cpu);
void cpu_nmi(Cpu* cpu);
void cpu_setIrq(Cpu* cpu, bool state);
void cpu_setInstructionHook(Cpu* cpu, CpuInstructionHook hook, void* userData);
void cpu_setCoprocessorHook(Cpu* cpu, CpuCoprocessorHook hook, void* userData);
uint8_t cpu_getInstructionSize(uint8_t opcode, bool mf, bool xf);
void cpu_disassembleInstruction(uint32_t address, bool mf, bool xf, const uint8_t* bytes, uint8_t size, CpuInstructionInfo* info);

#endif

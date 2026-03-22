
#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdbool.h>

#include <iostream>

#include "statehandler.h"

#include "mem_viewer.h"

typedef uint8_t (*CpuReadHandler)(void* mem, uint32_t adr);
typedef void (*CpuWriteHandler)(void* mem, uint32_t adr, uint8_t val);
typedef void (*CpuIdleHandler)(void* mem, bool waiting);

typedef struct Cpu Cpu;

struct ProgramCounter {
private:
    uint16_t value;
public:
    ProgramCounter(uint16_t v = 0) : value(v) {}
    ProgramCounter& operator=(uint16_t v) {
        value = v;
        return *this;
    }

    operator uint16_t() const {
        return value;
    }

    ProgramCounter& operator++() {
        ++value;
        return *this;
    }

    ProgramCounter operator++(int) {
        ProgramCounter tmp = *this;
        ++value;
        return tmp;
    }

    ProgramCounter& operator--() {
        --value;
        return *this;
    }

    ProgramCounter& operator+=(uint16_t v) {
        value += v;
        return *this;
    }

    ProgramCounter& operator-=(uint16_t v) {
        value -= v;
        return *this;
    }
    
    uint16_t raw() const { return value; }
};

struct Cpu {
  // reference to memory handler, pointers to read/write/idle handlers
  void* mem;
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

  MemViewer* copViewer;
  MemViewer* memViewer;
};

Cpu* cpu_init(void* mem, CpuReadHandler read, CpuWriteHandler write, CpuIdleHandler idle);
void cpu_free(Cpu* cpu);
void cpu_reset(Cpu* cpu, bool hard);
void cpu_handleState(Cpu* cpu, StateHandler* sh);
void cpu_runOpcode(Cpu* cpu);
void cpu_nmi(Cpu* cpu);
void cpu_setIrq(Cpu* cpu, bool state);

#endif

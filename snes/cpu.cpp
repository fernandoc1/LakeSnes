
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "statehandler.h"

static void cpu_decodeInstruction(
  uint32_t address,
  bool mf,
  bool xf,
  const uint8_t* bytes,
  uint8_t size,
  CpuInstructionInfo* info
);
static uint8_t cpu_read(Cpu* cpu, uint32_t adr);
static void cpu_write(Cpu* cpu, uint32_t adr, uint8_t val);
static void cpu_idle(Cpu* cpu);
static void cpu_idleWait(Cpu* cpu);
static void cpu_checkInt(Cpu* cpu);
static uint8_t cpu_readOpcode(Cpu* cpu);
static uint16_t cpu_readOpcodeWord(Cpu* cpu, bool intCheck);
static uint8_t cpu_getFlags(Cpu* cpu);
static void cpu_setFlags(Cpu* cpu, uint8_t value);
static void cpu_setZN(Cpu* cpu, uint16_t value, bool byte);
static void cpu_doBranch(Cpu* cpu, bool check);
static uint8_t cpu_pullByte(Cpu* cpu);
static void cpu_pushByte(Cpu* cpu, uint8_t value);
static uint16_t cpu_pullWord(Cpu* cpu, bool intCheck);
static void cpu_pushWord(Cpu* cpu, uint16_t value, bool intCheck);
static uint16_t cpu_readWord(Cpu* cpu, uint32_t adrl, uint32_t adrh, bool intCheck);
static void cpu_writeWord(Cpu* cpu, uint32_t adrl, uint32_t adrh, uint16_t value, bool reversed, bool intCheck);
static void cpu_doInterrupt(Cpu* cpu);
static void cpu_doOpcode(Cpu* cpu, uint8_t opcode);

// addressing modes and opcode functions not declared, only used after defintions

Cpu::Cpu(Snes* snes, CpuReadHandler read, CpuWriteHandler write, CpuIdleHandler idle)
    : snes(snes),
      read(read),
      write(write),
      idle(idle),
      a(0),
      x(0),
      y(0),
      sp(0),
      pc(0),
      dp(0),
      k(0),
      db(0),
      c(false),
      z(false),
      v(false),
      n(false),
      i(false),
      d(false),
      xf(false),
      mf(false),
      e(false),
      waiting(false),
      stopped(false),
      irqWanted(false),
      nmiWanted(false),
      intWanted(false),
      resetWanted(false),
      cop_addr(0),
      cop_size(0),
      copViewer(nullptr),
      memViewer(nullptr),
      executionMapViewer(nullptr),
      instructionHook(nullptr),
      instructionHookUserData(nullptr),
      coprocessorHook(nullptr),
      coprocessorHookUserData(nullptr),
      tracedInstructionAddress(0),
      tracedInstructionMf(false),
      tracedInstructionXf(false),
      tracedInstructionSize(0) {
  memset(cop_mem, 0, sizeof(cop_mem));
  memset(executionMap, 0, sizeof(executionMap));
  memset(tracedInstructionBytes, 0, sizeof(tracedInstructionBytes));
  //cpu->copViewer = mem_viewer_open(cpu->cop_mem, sizeof(cpu->cop_mem));
  memViewer = mem_viewer_open(snes, 0x10000); // Assuming 64KB memory space
  //executionMapViewer = mem_viewer_open(executionMap, sizeof(executionMap));
}

Cpu::~Cpu() {
  //mem_viewer_destroy(cpu->copViewer);
  mem_viewer_destroy(memViewer);
  //mem_viewer_destroy(executionMapViewer);
}

Cpu* cpu_init(Snes* snes, CpuReadHandler read, CpuWriteHandler write, CpuIdleHandler idle) {
  return new Cpu(snes, read, write, idle);
}

void cpu_free(Cpu* cpu) {
  delete cpu;
}

void Cpu::reset(bool hard) {
  if(hard) {
    a = 0;
    x = 0;
    y = 0;
    sp = 0;
    pc = 0;
    dp = 0;
    k = 0;
    db = 0;
    c = false;
    z = false;
    v = false;
    n = false;
    i = false;
    d = false;
    xf = false;
    mf = false;
    e = false;
    irqWanted = false;
  }
  waiting = false;
  stopped = false;
  nmiWanted = false;
  intWanted = false;
  resetWanted = true;
  cop_addr = 0;
  cop_size = 0;
}

void cpu_reset(Cpu* cpu, bool hard) {
  cpu->reset(hard);
}

void Cpu::handleState(StateHandler* sh) {
  sh_handleBools(sh,
    &c, &z, &v, &n, &i, &d, &xf, &mf, &e, &waiting, &stopped,
    &irqWanted, &nmiWanted, &intWanted, &resetWanted, NULL
  );
  sh_handleBytes(sh, &k, &db, NULL);
  uint16_t pcValue = pc.raw();
  sh_handleWords(sh, &a, &x, &y, &sp, &pcValue, &dp, NULL);
  pc = pcValue;
}

void cpu_handleState(Cpu* cpu, StateHandler* sh) {
  cpu->handleState(sh);
}

void Cpu::runOpcode() {
  if(resetWanted) {
    resetWanted = false;
    // reset: brk/interrupt without writes
    cpu_read(this, (k << 16) | pc);
    cpu_idle(this);
    cpu_read(this, 0x100 | (sp-- & 0xff));
    cpu_read(this, 0x100 | (sp-- & 0xff));
    cpu_read(this, 0x100 | (sp-- & 0xff));
    sp = (sp & 0xff) | 0x100;
    e = true;
    i = true;
    d = false;
    cpu_setFlags(this, cpu_getFlags(this)); // updates x and m flags, clears upper half of x and y if needed
    k = 0;
    pc = cpu_readWord(this, 0xfffc, 0xfffd, false);
    return;
  }
  if(stopped) {
    cpu_idleWait(this);
    return;
  }
  if(waiting) {
    if(irqWanted || nmiWanted) {
      waiting = false;
      cpu_idle(this);
      cpu_checkInt(this);
      cpu_idle(this);
      return;
    } else {
      cpu_idleWait(this);
      return;
    }
  }
  // not stopped or waiting, execute a opcode or go to interrupt
  if(intWanted) {
    cpu_read(this, (k << 16) | pc);
    cpu_doInterrupt(this);
  } else {
    beginInstructionTrace();
    uint8_t opcode = cpu_readOpcode(this);
    cpu_doOpcode(this, opcode);
    emitInstructionTrace();
  }
}

void cpu_runOpcode(Cpu* cpu) {
  cpu->runOpcode();
}

void Cpu::nmi() {
  nmiWanted = true;
}

void cpu_nmi(Cpu* cpu) {
  cpu->nmi();
}

void Cpu::setIrq(bool state) {
  irqWanted = state;
}

void cpu_setIrq(Cpu* cpu, bool state) {
  cpu->setIrq(state);
}

void Cpu::setInstructionHook(CpuInstructionHook hook, void* userData) {
  instructionHook = hook;
  instructionHookUserData = userData;
}

void cpu_setInstructionHook(Cpu* cpu, CpuInstructionHook hook, void* userData) {
  cpu->setInstructionHook(hook, userData);
}

void Cpu::setCoprocessorHook(CpuCoprocessorHook hook, void* userData) {
  coprocessorHook = hook;
  coprocessorHookUserData = userData;
}

void cpu_setCoprocessorHook(Cpu* cpu, CpuCoprocessorHook hook, void* userData) {
  cpu->setCoprocessorHook(hook, userData);
}

bool Cpu::runCoprocessorHook() {
  if(coprocessorHook == nullptr) {
    return false;
  }
  return coprocessorHook(
    coprocessorHookUserData,
    snes,
    cop_addr,
    &cop_mem[cop_addr],
    cop_size
  );
}

static uint8_t cpu_read(Cpu* cpu, uint32_t adr) {
  return cpu->read(cpu->snes, adr);
}

static void cpu_write(Cpu* cpu, uint32_t adr, uint8_t val) {
  cpu->write(cpu->snes, adr, val);
}

static void cpu_idle(Cpu* cpu) {
  cpu->idle(cpu->snes, false);
}

static void cpu_idleWait(Cpu* cpu) {
  cpu->idle(cpu->snes, true);
}

static void cpu_checkInt(Cpu* cpu) {
  cpu->intWanted = cpu->nmiWanted || (cpu->irqWanted && !cpu->i);
}

static uint8_t cpu_readOpcode(Cpu* cpu) {
  uint8_t val = cpu_read(cpu, (cpu->k << 16) | cpu->pc);
  cpu->executionMap[cpu->pc] = val;
  cpu->appendInstructionByte(val);
  cpu->pc++;
  return val;
}

static uint16_t cpu_readOpcodeWord(Cpu* cpu, bool intCheck) {
  uint8_t low = cpu_readOpcode(cpu);
  if(intCheck) cpu_checkInt(cpu);
  return low | (cpu_readOpcode(cpu) << 8);
}

static uint8_t cpu_getFlags(Cpu* cpu) {
  uint8_t val = cpu->n << 7;
  val |= cpu->v << 6;
  val |= cpu->mf << 5;
  val |= cpu->xf << 4;
  val |= cpu->d << 3;
  val |= cpu->i << 2;
  val |= cpu->z << 1;
  val |= cpu->c;
  return val;
}

static void cpu_setFlags(Cpu* cpu, uint8_t val) {
  cpu->n = val & 0x80;
  cpu->v = val & 0x40;
  cpu->mf = val & 0x20;
  cpu->xf = val & 0x10;
  cpu->d = val & 8;
  cpu->i = val & 4;
  cpu->z = val & 2;
  cpu->c = val & 1;
  if(cpu->e) {
    cpu->mf = true;
    cpu->xf = true;
    cpu->sp = (cpu->sp & 0xff) | 0x100;
  }
  if(cpu->xf) {
    cpu->x &= 0xff;
    cpu->y &= 0xff;
  }
}

static void cpu_setZN(Cpu* cpu, uint16_t value, bool byte) {
  if(byte) {
    cpu->z = (value & 0xff) == 0;
    cpu->n = value & 0x80;
  } else {
    cpu->z = value == 0;
    cpu->n = value & 0x8000;
  }
}

static void cpu_doBranch(Cpu* cpu, bool check) {
  if(!check) cpu_checkInt(cpu);
  uint8_t value = cpu_readOpcode(cpu);
  if(check) {
    cpu_checkInt(cpu);
    cpu_idle(cpu); // taken branch: 1 extra cycle
    cpu->pc += (int8_t) value;
  }
}

static uint8_t cpu_pullByte(Cpu* cpu) {
  cpu->sp++;
  if(cpu->e) cpu->sp = (cpu->sp & 0xff) | 0x100;
  return cpu_read(cpu, cpu->sp);
}

static void cpu_pushByte(Cpu* cpu, uint8_t value) {
  cpu_write(cpu, cpu->sp, value);
  cpu->sp--;
  if(cpu->e) cpu->sp = (cpu->sp & 0xff) | 0x100;
}

static uint16_t cpu_pullWord(Cpu* cpu, bool intCheck) {
  uint8_t value = cpu_pullByte(cpu);
  if(intCheck) cpu_checkInt(cpu);
  return value | (cpu_pullByte(cpu) << 8);
}

static void cpu_pushWord(Cpu* cpu, uint16_t value, bool intCheck) {
  cpu_pushByte(cpu, value >> 8);
  if(intCheck) cpu_checkInt(cpu);
  cpu_pushByte(cpu, value & 0xff);
}

static uint16_t cpu_readWord(Cpu* cpu, uint32_t adrl, uint32_t adrh, bool intCheck) {
  uint8_t value = cpu_read(cpu, adrl);
  if(intCheck) cpu_checkInt(cpu);
  return value | (cpu_read(cpu, adrh) << 8);
}

static void cpu_writeWord(Cpu* cpu, uint32_t adrl, uint32_t adrh, uint16_t value, bool reversed, bool intCheck) {
  if(reversed) {
    cpu_write(cpu, adrh, value >> 8);
    if(intCheck) cpu_checkInt(cpu);
    cpu_write(cpu, adrl, value & 0xff);
  } else {
    cpu_write(cpu, adrl, value & 0xff);
    if(intCheck) cpu_checkInt(cpu);
    cpu_write(cpu, adrh, value >> 8);
  }
}

static void cpu_doInterrupt(Cpu* cpu) {
  cpu_idle(cpu);
  cpu_pushByte(cpu, cpu->k);
  cpu_pushWord(cpu, cpu->pc, false);
  cpu_pushByte(cpu, cpu_getFlags(cpu));
  cpu->i = true;
  cpu->d = false;
  cpu->k = 0;
  cpu->intWanted = false;
  if(cpu->nmiWanted) {
    cpu->nmiWanted = false;
    cpu->pc = cpu_readWord(cpu, 0xffea, 0xffeb, false);
  } else { // irq
    cpu->pc = cpu_readWord(cpu, 0xffee, 0xffef, false);
  }
}

// addressing modes

static void cpu_adrImp(Cpu* cpu) {
  // only for 2-cycle implied opcodes
  cpu_checkInt(cpu);
  if(cpu->intWanted) {
    // if interrupt detected in 2-cycle implied/accumulator opcode,
    // idle cycle turns into read from pc
    cpu_read(cpu, (cpu->k << 16) | cpu->pc);
  } else {
    cpu_idle(cpu);
  }
}

static uint32_t cpu_adrImm(Cpu* cpu, uint32_t* low, bool xFlag) {
  if((xFlag && cpu->xf) || (!xFlag && cpu->mf)) {
    *low = (cpu->k << 16) | cpu->pc++;
    return 0;
  } else {
    *low = (cpu->k << 16) | cpu->pc++;
    return (cpu->k << 16) | cpu->pc++;
  }
}

static uint32_t cpu_adrDp(Cpu* cpu, uint32_t* low) {
  uint8_t adr = cpu_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu_idle(cpu); // dpr not 0: 1 extra cycle
  *low = (cpu->dp + adr) & 0xffff;
  return (cpu->dp + adr + 1) & 0xffff;
}

static uint32_t cpu_adrDpx(Cpu* cpu, uint32_t* low) {
  uint8_t adr = cpu_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu_idle(cpu); // dpr not 0: 1 extra cycle
  cpu_idle(cpu);
  *low = (cpu->dp + adr + cpu->x) & 0xffff;
  return (cpu->dp + adr + cpu->x + 1) & 0xffff;
}

static uint32_t cpu_adrDpy(Cpu* cpu, uint32_t* low) {
  uint8_t adr = cpu_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu_idle(cpu); // dpr not 0: 1 extra cycle
  cpu_idle(cpu);
  *low = (cpu->dp + adr + cpu->y) & 0xffff;
  return (cpu->dp + adr + cpu->y + 1) & 0xffff;
}

static uint32_t cpu_adrIdp(Cpu* cpu, uint32_t* low) {
  uint8_t adr = cpu_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu_idle(cpu); // dpr not 0: 1 extra cycle
  uint16_t pointer = cpu_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff, false);
  *low = (cpu->db << 16) + pointer;
  return ((cpu->db << 16) + pointer + 1) & 0xffffff;
}

static uint32_t cpu_adrIdx(Cpu* cpu, uint32_t* low) {
  uint8_t adr = cpu_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu_idle(cpu); // dpr not 0: 1 extra cycle
  cpu_idle(cpu);
  uint16_t pointer = cpu_readWord(cpu, (cpu->dp + adr + cpu->x) & 0xffff, (cpu->dp + adr + cpu->x + 1) & 0xffff, false);
  *low = (cpu->db << 16) + pointer;
  return ((cpu->db << 16) + pointer + 1) & 0xffffff;
}

static uint32_t cpu_adrIdy(Cpu* cpu, uint32_t* low, bool write) {
  uint8_t adr = cpu_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu_idle(cpu); // dpr not 0: 1 extra cycle
  uint16_t pointer = cpu_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff, false);
  // writing opcode or x = 0 or page crossed: 1 extra cycle
  if(write || !cpu->xf || ((pointer >> 8) != ((pointer + cpu->y) >> 8))) cpu_idle(cpu);
  *low = ((cpu->db << 16) + pointer + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + pointer + cpu->y + 1) & 0xffffff;
}

static uint32_t cpu_adrIdl(Cpu* cpu, uint32_t* low) {
  uint8_t adr = cpu_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu_idle(cpu); // dpr not 0: 1 extra cycle
  uint32_t pointer = cpu_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff, false);
  pointer |= cpu_read(cpu, (cpu->dp + adr + 2) & 0xffff) << 16;
  *low = pointer;
  return (pointer + 1) & 0xffffff;
}

static uint32_t cpu_adrIly(Cpu* cpu, uint32_t* low) {
  uint8_t adr = cpu_readOpcode(cpu);
  if(cpu->dp & 0xff) cpu_idle(cpu); // dpr not 0: 1 extra cycle
  uint32_t pointer = cpu_readWord(cpu, (cpu->dp + adr) & 0xffff, (cpu->dp + adr + 1) & 0xffff, false);
  pointer |= cpu_read(cpu, (cpu->dp + adr + 2) & 0xffff) << 16;
  *low = (pointer + cpu->y) & 0xffffff;
  return (pointer + cpu->y + 1) & 0xffffff;
}

static uint32_t cpu_adrSr(Cpu* cpu, uint32_t* low) {
  uint8_t adr = cpu_readOpcode(cpu);
  cpu_idle(cpu);
  *low = (cpu->sp + adr) & 0xffff;
  return (cpu->sp + adr + 1) & 0xffff;
}

static uint32_t cpu_adrIsy(Cpu* cpu, uint32_t* low) {
  uint8_t adr = cpu_readOpcode(cpu);
  cpu_idle(cpu);
  uint16_t pointer = cpu_readWord(cpu, (cpu->sp + adr) & 0xffff, (cpu->sp + adr + 1) & 0xffff, false);
  cpu_idle(cpu);
  *low = ((cpu->db << 16) + pointer + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + pointer + cpu->y + 1) & 0xffffff;
}

static uint32_t cpu_adrAbs(Cpu* cpu, uint32_t* low) {
  uint16_t adr = cpu_readOpcodeWord(cpu, false);
  *low = (cpu->db << 16) + adr;
  return ((cpu->db << 16) + adr + 1) & 0xffffff;
}

static uint32_t cpu_adrAbx(Cpu* cpu, uint32_t* low, bool write) {
  uint16_t adr = cpu_readOpcodeWord(cpu, false);
  // writing opcode or x = 0 or page crossed: 1 extra cycle
  if(write || !cpu->xf || ((adr >> 8) != ((adr + cpu->x) >> 8))) cpu_idle(cpu);
  *low = ((cpu->db << 16) + adr + cpu->x) & 0xffffff;
  return ((cpu->db << 16) + adr + cpu->x + 1) & 0xffffff;
}

static uint32_t cpu_adrAby(Cpu* cpu, uint32_t* low, bool write) {
  uint16_t adr = cpu_readOpcodeWord(cpu, false);
  // writing opcode or x = 0 or page crossed: 1 extra cycle
  if(write || !cpu->xf || ((adr >> 8) != ((adr + cpu->y) >> 8))) cpu_idle(cpu);
  *low = ((cpu->db << 16) + adr + cpu->y) & 0xffffff;
  return ((cpu->db << 16) + adr + cpu->y + 1) & 0xffffff;
}

static uint32_t cpu_adrAbl(Cpu* cpu, uint32_t* low) {
  uint32_t adr = cpu_readOpcodeWord(cpu, false);
  adr |= cpu_readOpcode(cpu) << 16;
  *low = adr;
  return (adr + 1) & 0xffffff;
}

static uint32_t cpu_adrAlx(Cpu* cpu, uint32_t* low) {
  uint32_t adr = cpu_readOpcodeWord(cpu, false);
  adr |= cpu_readOpcode(cpu) << 16;
  *low = (adr + cpu->x) & 0xffffff;
  return (adr + cpu->x + 1) & 0xffffff;
}

// opcode functions

static void cpu_and(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    cpu_checkInt(cpu);
    uint8_t value = cpu_read(cpu, low);
    cpu->a = (cpu->a & 0xff00) | ((cpu->a & value) & 0xff);
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, true);
    cpu->a &= value;
  }
  cpu_setZN(cpu, cpu->a, cpu->mf);
}

static void cpu_ora(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    cpu_checkInt(cpu);
    uint8_t value = cpu_read(cpu, low);
    cpu->a = (cpu->a & 0xff00) | ((cpu->a | value) & 0xff);
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, true);
    cpu->a |= value;
  }
  cpu_setZN(cpu, cpu->a, cpu->mf);
}

static void cpu_eor(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    cpu_checkInt(cpu);
    uint8_t value = cpu_read(cpu, low);
    cpu->a = (cpu->a & 0xff00) | ((cpu->a ^ value) & 0xff);
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, true);
    cpu->a ^= value;
  }
  cpu_setZN(cpu, cpu->a, cpu->mf);
}

static void cpu_adc(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    cpu_checkInt(cpu);
    uint8_t value = cpu_read(cpu, low);
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result > 0x9) result = ((result + 0x6) & 0xf) + 0x10;
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
    } else {
      result = (cpu->a & 0xff) + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
    if(cpu->d && result > 0x9f) result += 0x60;
    cpu->c = result > 0xff;
    cpu->a = (cpu->a & 0xff00) | (result & 0xff);
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, true);
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result > 0x9) result = ((result + 0x6) & 0xf) + 0x10;
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
      if(result > 0x9f) result = ((result + 0x60) & 0xff) + 0x100;
      result = (cpu->a & 0xf00) + (value & 0xf00) + result;
      if(result > 0x9ff) result = ((result + 0x600) & 0xfff) + 0x1000;
      result = (cpu->a & 0xf000) + (value & 0xf000) + result;
    } else {
      result = cpu->a + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
    if(cpu->d && result > 0x9fff) result += 0x6000;
    cpu->c = result > 0xffff;
    cpu->a = result;
  }
  cpu_setZN(cpu, cpu->a, cpu->mf);
}

static void cpu_sbc(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    cpu_checkInt(cpu);
    uint8_t value = cpu_read(cpu, low) ^ 0xff;
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result < 0x10) result = (result - 0x6) & ((result - 0x6 < 0) ? 0xf : 0x1f);
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
    } else {
      result = (cpu->a & 0xff) + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x80) == (value & 0x80) && (value & 0x80) != (result & 0x80);
    if(cpu->d && result < 0x100) result -= 0x60;
    cpu->c = result > 0xff;
    cpu->a = (cpu->a & 0xff00) | (result & 0xff);
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, true) ^ 0xffff;
    int result = 0;
    if(cpu->d) {
      result = (cpu->a & 0xf) + (value & 0xf) + cpu->c;
      if(result < 0x10) result = (result - 0x6) & ((result - 0x6 < 0) ? 0xf : 0x1f);
      result = (cpu->a & 0xf0) + (value & 0xf0) + result;
      if(result < 0x100) result = (result - 0x60) & ((result - 0x60 < 0) ? 0xff : 0x1ff);
      result = (cpu->a & 0xf00) + (value & 0xf00) + result;
      if(result < 0x1000) result = (result - 0x600) & ((result - 0x600 < 0) ? 0xfff : 0x1fff);
      result = (cpu->a & 0xf000) + (value & 0xf000) + result;
    } else {
      result = cpu->a + value + cpu->c;
    }
    cpu->v = (cpu->a & 0x8000) == (value & 0x8000) && (value & 0x8000) != (result & 0x8000);
    if(cpu->d && result < 0x10000) result -= 0x6000;
    cpu->c = result > 0xffff;
    cpu->a = result;
  }
  cpu_setZN(cpu, cpu->a, cpu->mf);
}

static void cpu_cmp(Cpu* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    cpu_checkInt(cpu);
    uint8_t value = cpu_read(cpu, low) ^ 0xff;
    result = (cpu->a & 0xff) + value + 1;
    cpu->c = result > 0xff;
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, true) ^ 0xffff;
    result = cpu->a + value + 1;
    cpu->c = result > 0xffff;
  }
  cpu_setZN(cpu, result, cpu->mf);
}

static void cpu_cpx(Cpu* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->xf) {
    cpu_checkInt(cpu);
    uint8_t value = cpu_read(cpu, low) ^ 0xff;
    result = (cpu->x & 0xff) + value + 1;
    cpu->c = result > 0xff;
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, true) ^ 0xffff;
    result = cpu->x + value + 1;
    cpu->c = result > 0xffff;
  }
  cpu_setZN(cpu, result, cpu->xf);
}

static void cpu_cpy(Cpu* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->xf) {
    cpu_checkInt(cpu);
    uint8_t value = cpu_read(cpu, low) ^ 0xff;
    result = (cpu->y & 0xff) + value + 1;
    cpu->c = result > 0xff;
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, true) ^ 0xffff;
    result = cpu->y + value + 1;
    cpu->c = result > 0xffff;
  }
  cpu_setZN(cpu, result, cpu->xf);
}

static void cpu_bit(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    cpu_checkInt(cpu);
    uint8_t value = cpu_read(cpu, low);
    uint8_t result = (cpu->a & 0xff) & value;
    cpu->z = result == 0;
    cpu->n = value & 0x80;
    cpu->v = value & 0x40;
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, true);
    uint16_t result = cpu->a & value;
    cpu->z = result == 0;
    cpu->n = value & 0x8000;
    cpu->v = value & 0x4000;
  }
}

static void cpu_lda(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    cpu_checkInt(cpu);
    cpu->a = (cpu->a & 0xff00) | cpu_read(cpu, low);
  } else {
    cpu->a = cpu_readWord(cpu, low, high, true);
  }
  cpu_setZN(cpu, cpu->a, cpu->mf);
}

static void cpu_ldx(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->xf) {
    cpu_checkInt(cpu);
    cpu->x = cpu_read(cpu, low);
  } else {
    cpu->x = cpu_readWord(cpu, low, high, true);
  }
  cpu_setZN(cpu, cpu->x, cpu->xf);
}

static void cpu_ldy(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->xf) {
    cpu_checkInt(cpu);
    cpu->y = cpu_read(cpu, low);
  } else {
    cpu->y = cpu_readWord(cpu, low, high, true);
  }
  cpu_setZN(cpu, cpu->y, cpu->xf);
}

static void cpu_sta(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    cpu_checkInt(cpu);
    cpu_write(cpu, low, cpu->a);
  } else {
    cpu_writeWord(cpu, low, high, cpu->a, false, true);
  }
}

static void cpu_stx(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->xf) {
    cpu_checkInt(cpu);
    cpu_write(cpu, low, cpu->x);
  } else {
    cpu_writeWord(cpu, low, high, cpu->x, false, true);
  }
}

static void cpu_sty(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->xf) {
    cpu_checkInt(cpu);
    cpu_write(cpu, low, cpu->y);
  } else {
    cpu_writeWord(cpu, low, high, cpu->y, false, true);
  }
}

static void cpu_stz(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    cpu_checkInt(cpu);
    cpu_write(cpu, low, 0);
  } else {
    cpu_writeWord(cpu, low, high, 0, false, true);
  }
}

static void cpu_ror(Cpu* cpu, uint32_t low, uint32_t high) {
  bool carry = false;
  int result = 0;
  if(cpu->mf) {
    uint8_t value = cpu_read(cpu, low);
    cpu_idle(cpu);
    carry = value & 1;
    result = (value >> 1) | (cpu->c << 7);
    cpu_checkInt(cpu);
    cpu_write(cpu, low, result);
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, false);
    cpu_idle(cpu);
    carry = value & 1;
    result = (value >> 1) | (cpu->c << 15);
    cpu_writeWord(cpu, low, high, result, true, true);
  }
  cpu_setZN(cpu, result, cpu->mf);
  cpu->c = carry;
}

static void cpu_rol(Cpu* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    result = (cpu_read(cpu, low) << 1) | cpu->c;
    cpu_idle(cpu);
    cpu->c = result & 0x100;
    cpu_checkInt(cpu);
    cpu_write(cpu, low, result);
  } else {
    result = (cpu_readWord(cpu, low, high, false) << 1) | cpu->c;
    cpu_idle(cpu);
    cpu->c = result & 0x10000;
    cpu_writeWord(cpu, low, high, result, true, true);
  }
  cpu_setZN(cpu, result, cpu->mf);
}

static void cpu_lsr(Cpu* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    uint8_t value = cpu_read(cpu, low);
    cpu_idle(cpu);
    cpu->c = value & 1;
    result = value >> 1;
    cpu_checkInt(cpu);
    cpu_write(cpu, low, result);
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, false);
    cpu_idle(cpu);
    cpu->c = value & 1;
    result = value >> 1;
    cpu_writeWord(cpu, low, high, result, true, true);
  }
  cpu_setZN(cpu, result, cpu->mf);
}

static void cpu_asl(Cpu* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    result = cpu_read(cpu, low) << 1;
    cpu_idle(cpu);
    cpu->c = result & 0x100;
    cpu_checkInt(cpu);
    cpu_write(cpu, low, result);
  } else {
    result = cpu_readWord(cpu, low, high, false) << 1;
    cpu_idle(cpu);
    cpu->c = result & 0x10000;
    cpu_writeWord(cpu, low, high, result, true, true);
  }
  cpu_setZN(cpu, result, cpu->mf);
}

static void cpu_inc(Cpu* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    result = cpu_read(cpu, low) + 1;
    cpu_idle(cpu);
    cpu_checkInt(cpu);
    cpu_write(cpu, low, result);
  } else {
    result = cpu_readWord(cpu, low, high, false) + 1;
    cpu_idle(cpu);
    cpu_writeWord(cpu, low, high, result, true, true);
  }
  cpu_setZN(cpu, result, cpu->mf);
}

static void cpu_dec(Cpu* cpu, uint32_t low, uint32_t high) {
  int result = 0;
  if(cpu->mf) {
    result = cpu_read(cpu, low) - 1;
    cpu_idle(cpu);
    cpu_checkInt(cpu);
    cpu_write(cpu, low, result);
  } else {
    result = cpu_readWord(cpu, low, high, false) - 1;
    cpu_idle(cpu);
    cpu_writeWord(cpu, low, high, result, true, true);
  }
  cpu_setZN(cpu, result, cpu->mf);
}

static void cpu_tsb(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = cpu_read(cpu, low);
    cpu_idle(cpu);
    cpu->z = ((cpu->a & 0xff) & value) == 0;
    cpu_checkInt(cpu);
    cpu_write(cpu, low, value | (cpu->a & 0xff));
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, false);
    cpu_idle(cpu);
    cpu->z = (cpu->a & value) == 0;
    cpu_writeWord(cpu, low, high, value | cpu->a, true, true);
  }
}

static void cpu_trb(Cpu* cpu, uint32_t low, uint32_t high) {
  if(cpu->mf) {
    uint8_t value = cpu_read(cpu, low);
    cpu_idle(cpu);
    cpu->z = ((cpu->a & 0xff) & value) == 0;
    cpu_checkInt(cpu);
    cpu_write(cpu, low, value & ~(cpu->a & 0xff));
  } else {
    uint16_t value = cpu_readWord(cpu, low, high, false);
    cpu_idle(cpu);
    cpu->z = (cpu->a & value) == 0;
    cpu_writeWord(cpu, low, high, value & ~cpu->a, true, true);
  }
}

static void cpu_doOpcode(Cpu* cpu, uint8_t opcode) {
  switch(opcode) {
    case 0x00: { // brk imm(s)
      cpu_readOpcode(cpu);
      cpu_pushByte(cpu, cpu->k);
      cpu_pushWord(cpu, cpu->pc, false);
      cpu_pushByte(cpu, cpu_getFlags(cpu));
      cpu->i = true;
      cpu->d = false;
      cpu->k = 0;
      cpu->pc = cpu_readWord(cpu, 0xffe6, 0xffe7, true);
      break;
    }
    case 0x01: { // ora idx
      uint32_t low = 0;
      uint32_t high = cpu_adrIdx(cpu, &low);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x02: { // cop imm(s)
      uint8_t val = cpu_readOpcode(cpu);
      cpu_checkInt(cpu);
      cpu_idle(cpu);
      fprintf(stderr, "cop with operand %02x, at %04x\n", val, cpu->pc.raw());
      cpu->cop_addr = val;
      cpu->cop_size = 0;
      break;
    }
    case 0x03: { // ora sr
      uint32_t low = 0;
      uint32_t high = cpu_adrSr(cpu, &low);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x04: { // tsb dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_tsb(cpu, low, high);
      break;
    }
    case 0x05: { // ora dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x06: { // asl dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_asl(cpu, low, high);
      break;
    }
    case 0x07: { // ora idl
      uint32_t low = 0;
      uint32_t high = cpu_adrIdl(cpu, &low);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x08: { // php imp
      cpu_idle(cpu);
      cpu_checkInt(cpu);
      cpu_pushByte(cpu, cpu_getFlags(cpu));
      break;
    }
    case 0x09: { // ora imm(m)
      uint32_t low = 0;
      uint32_t high = cpu_adrImm(cpu, &low, false);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x0a: { // asla imp
      cpu_adrImp(cpu);
      if(cpu->mf) {
        cpu->c = cpu->a & 0x80;
        cpu->a = (cpu->a & 0xff00) | ((cpu->a << 1) & 0xff);
      } else {
        cpu->c = cpu->a & 0x8000;
        cpu->a <<= 1;
      }
      cpu_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x0b: { // phd imp
      cpu_idle(cpu);
      cpu_pushWord(cpu, cpu->dp, true);
      break;
    }
    case 0x0c: { // tsb abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_tsb(cpu, low, high);
      break;
    }
    case 0x0d: { // ora abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x0e: { // asl abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_asl(cpu, low, high);
      break;
    }
    case 0x0f: { // ora abl
      uint32_t low = 0;
      uint32_t high = cpu_adrAbl(cpu, &low);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x10: { // bpl rel
      cpu_doBranch(cpu, !cpu->n);
      break;
    }
    case 0x11: { // ora idy(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrIdy(cpu, &low, false);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x12: { // ora idp
      uint32_t low = 0;
      uint32_t high = cpu_adrIdp(cpu, &low);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x13: { // ora isy
      uint32_t low = 0;
      uint32_t high = cpu_adrIsy(cpu, &low);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x14: { // trb dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_trb(cpu, low, high);
      break;
    }
    case 0x15: { // ora dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x16: { // asl dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_asl(cpu, low, high);
      break;
    }
    case 0x17: { // ora ily
      uint32_t low = 0;
      uint32_t high = cpu_adrIly(cpu, &low);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x18: { // clc imp
      cpu_adrImp(cpu);
      cpu->c = false;
      break;
    }
    case 0x19: { // ora aby(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAby(cpu, &low, false);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x1a: { // inca imp
      cpu_adrImp(cpu);
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | ((cpu->a + 1) & 0xff);
      } else {
        cpu->a++;
      }
      cpu_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x1b: { // tcs imp
      cpu_adrImp(cpu);
      cpu->sp = cpu->a;
      break;
    }
    case 0x1c: { // trb abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_trb(cpu, low, high);
      break;
    }
    case 0x1d: { // ora abx(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, false);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x1e: { // asl abx
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, true);
      cpu_asl(cpu, low, high);
      break;
    }
    case 0x1f: { // ora alx
      uint32_t low = 0;
      uint32_t high = cpu_adrAlx(cpu, &low);
      cpu_ora(cpu, low, high);
      break;
    }
    case 0x20: { // jsr abs
      uint16_t value = cpu_readOpcodeWord(cpu, false);
      cpu_idle(cpu);
      cpu_pushWord(cpu, cpu->pc - 1, true);
      cpu->pc = value;
      break;
    }
    case 0x21: { // and idx
      uint32_t low = 0;
      uint32_t high = cpu_adrIdx(cpu, &low);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x22: { // jsl abl
      uint16_t value = cpu_readOpcodeWord(cpu, false);
      cpu_pushByte(cpu, cpu->k);
      cpu_idle(cpu);
      uint8_t newK = cpu_readOpcode(cpu);
      cpu_pushWord(cpu, cpu->pc - 1, true);
      cpu->pc = value;
      cpu->k = newK;
      break;
    }
    case 0x23: { // and sr
      uint32_t low = 0;
      uint32_t high = cpu_adrSr(cpu, &low);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x24: { // bit dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_bit(cpu, low, high);
      break;
    }
    case 0x25: { // and dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x26: { // rol dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_rol(cpu, low, high);
      break;
    }
    case 0x27: { // and idl
      uint32_t low = 0;
      uint32_t high = cpu_adrIdl(cpu, &low);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x28: { // plp imp
      cpu_idle(cpu);
      cpu_idle(cpu);
      cpu_checkInt(cpu);
      cpu_setFlags(cpu, cpu_pullByte(cpu));
      break;
    }
    case 0x29: { // and imm(m)
      uint32_t low = 0;
      uint32_t high = cpu_adrImm(cpu, &low, false);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x2a: { // rola imp
      cpu_adrImp(cpu);
      int result = (cpu->a << 1) | cpu->c;
      if(cpu->mf) {
        cpu->c = result & 0x100;
        cpu->a = (cpu->a & 0xff00) | (result & 0xff);
      } else {
        cpu->c = result & 0x10000;
        cpu->a = result;
      }
      cpu_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x2b: { // pld imp
      cpu_idle(cpu);
      cpu_idle(cpu);
      cpu->dp = cpu_pullWord(cpu, true);
      cpu_setZN(cpu, cpu->dp, false);
      break;
    }
    case 0x2c: { // bit abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_bit(cpu, low, high);
      break;
    }
    case 0x2d: { // and abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x2e: { // rol abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_rol(cpu, low, high);
      break;
    }
    case 0x2f: { // and abl
      uint32_t low = 0;
      uint32_t high = cpu_adrAbl(cpu, &low);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x30: { // bmi rel
      cpu_doBranch(cpu, cpu->n);
      break;
    }
    case 0x31: { // and idy(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrIdy(cpu, &low, false);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x32: { // and idp
      uint32_t low = 0;
      uint32_t high = cpu_adrIdp(cpu, &low);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x33: { // and isy
      uint32_t low = 0;
      uint32_t high = cpu_adrIsy(cpu, &low);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x34: { // bit dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_bit(cpu, low, high);
      break;
    }
    case 0x35: { // and dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x36: { // rol dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_rol(cpu, low, high);
      break;
    }
    case 0x37: { // and ily
      uint32_t low = 0;
      uint32_t high = cpu_adrIly(cpu, &low);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x38: { // sec imp
      cpu_adrImp(cpu);
      cpu->c = true;
      break;
    }
    case 0x39: { // and aby(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAby(cpu, &low, false);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x3a: { // deca imp
      cpu_adrImp(cpu);
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | ((cpu->a - 1) & 0xff);
      } else {
        cpu->a--;
      }
      cpu_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x3b: { // tsc imp
      cpu_adrImp(cpu);
      cpu->a = cpu->sp;
      cpu_setZN(cpu, cpu->a, false);
      break;
    }
    case 0x3c: { // bit abx(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, false);
      cpu_bit(cpu, low, high);
      break;
    }
    case 0x3d: { // and abx(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, false);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x3e: { // rol abx
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, true);
      cpu_rol(cpu, low, high);
      break;
    }
    case 0x3f: { // and alx
      uint32_t low = 0;
      uint32_t high = cpu_adrAlx(cpu, &low);
      cpu_and(cpu, low, high);
      break;
    }
    case 0x40: { // rti imp
      cpu_idle(cpu);
      cpu_idle(cpu);
      cpu_setFlags(cpu, cpu_pullByte(cpu));
      cpu->pc = cpu_pullWord(cpu, false);
      cpu_checkInt(cpu);
      cpu->k = cpu_pullByte(cpu);
      break;
    }
    case 0x41: { // eor idx
      uint32_t low = 0;
      uint32_t high = cpu_adrIdx(cpu, &low);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x42: { // wdm imm(s)
      uint8_t val = cpu_readOpcode(cpu);
      cpu_checkInt(cpu);
      cpu_idle(cpu);
      fprintf(stderr, "wdm with operand %02x\n", val);
      if(cpu->cop_addr + cpu->cop_size < sizeof(cpu->cop_mem)) {
        cpu->cop_mem[cpu->cop_addr + cpu->cop_size] = val;
        cpu->cop_size++;
      }
      break;
    }
    case 0x43: { // eor sr
      uint32_t low = 0;
      uint32_t high = cpu_adrSr(cpu, &low);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x44: { // mvp bm
      uint8_t dest = cpu_readOpcode(cpu);
      uint8_t src = cpu_readOpcode(cpu);
      cpu->db = dest;
      cpu_write(cpu, (dest << 16) | cpu->y, cpu_read(cpu, (src << 16) | cpu->x));
      cpu->a--;
      cpu->x--;
      cpu->y--;
      if(cpu->a != 0xffff) {
        cpu->pc -= 3;
      }
      if(cpu->xf) {
        cpu->x &= 0xff;
        cpu->y &= 0xff;
      }
      cpu_idle(cpu);
      cpu_checkInt(cpu);
      cpu_idle(cpu);
      break;
    }
    case 0x45: { // eor dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x46: { // lsr dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_lsr(cpu, low, high);
      break;
    }
    case 0x47: { // eor idl
      uint32_t low = 0;
      uint32_t high = cpu_adrIdl(cpu, &low);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x48: { // pha imp
      cpu_idle(cpu);
      if(cpu->mf) {
        cpu_checkInt(cpu);
        cpu_pushByte(cpu, cpu->a);
      } else {
        cpu_pushWord(cpu, cpu->a, true);
      }
      break;
    }
    case 0x49: { // eor imm(m)
      uint32_t low = 0;
      uint32_t high = cpu_adrImm(cpu, &low, false);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x4a: { // lsra imp
      cpu_adrImp(cpu);
      cpu->c = cpu->a & 1;
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | ((cpu->a >> 1) & 0x7f);
      } else {
        cpu->a >>= 1;
      }
      cpu_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x4b: { // phk imp
      cpu_idle(cpu);
      cpu_checkInt(cpu);
      cpu_pushByte(cpu, cpu->k);
      break;
    }
    case 0x4c: { // jmp abs
      cpu->pc = cpu_readOpcodeWord(cpu, true);
      break;
    }
    case 0x4d: { // eor abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x4e: { // lsr abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_lsr(cpu, low, high);
      break;
    }
    case 0x4f: { // eor abl
      uint32_t low = 0;
      uint32_t high = cpu_adrAbl(cpu, &low);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x50: { // bvc rel
      cpu_doBranch(cpu, !cpu->v);
      break;
    }
    case 0x51: { // eor idy(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrIdy(cpu, &low, false);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x52: { // eor idp
      uint32_t low = 0;
      uint32_t high = cpu_adrIdp(cpu, &low);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x53: { // eor isy
      uint32_t low = 0;
      uint32_t high = cpu_adrIsy(cpu, &low);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x54: { // mvn bm
      uint8_t dest = cpu_readOpcode(cpu);
      uint8_t src = cpu_readOpcode(cpu);
      cpu->db = dest;
      cpu_write(cpu, (dest << 16) | cpu->y, cpu_read(cpu, (src << 16) | cpu->x));
      cpu->a--;
      cpu->x++;
      cpu->y++;
      if(cpu->a != 0xffff) {
        cpu->pc -= 3;
      }
      if(cpu->xf) {
        cpu->x &= 0xff;
        cpu->y &= 0xff;
      }
      cpu_idle(cpu);
      cpu_checkInt(cpu);
      cpu_idle(cpu);
      break;
    }
    case 0x55: { // eor dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x56: { // lsr dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_lsr(cpu, low, high);
      break;
    }
    case 0x57: { // eor ily
      uint32_t low = 0;
      uint32_t high = cpu_adrIly(cpu, &low);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x58: { // cli imp
      cpu_adrImp(cpu);
      cpu->i = false;
      break;
    }
    case 0x59: { // eor aby(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAby(cpu, &low, false);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x5a: { // phy imp
      cpu_idle(cpu);
      if(cpu->xf) {
        cpu_checkInt(cpu);
        cpu_pushByte(cpu, cpu->y);
      } else {
        cpu_pushWord(cpu, cpu->y, true);
      }
      break;
    }
    case 0x5b: { // tcd imp
      cpu_adrImp(cpu);
      cpu->dp = cpu->a;
      cpu_setZN(cpu, cpu->dp, false);
      break;
    }
    case 0x5c: { // jml abl
      uint16_t value = cpu_readOpcodeWord(cpu, false);
      cpu_checkInt(cpu);
      cpu->k = cpu_readOpcode(cpu);
      cpu->pc = value;
      break;
    }
    case 0x5d: { // eor abx(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, false);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x5e: { // lsr abx
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, true);
      cpu_lsr(cpu, low, high);
      break;
    }
    case 0x5f: { // eor alx
      uint32_t low = 0;
      uint32_t high = cpu_adrAlx(cpu, &low);
      cpu_eor(cpu, low, high);
      break;
    }
    case 0x60: { // rts imp
      cpu_idle(cpu);
      cpu_idle(cpu);
      cpu->pc = cpu_pullWord(cpu, false) + 1;
      cpu_checkInt(cpu);
      cpu_idle(cpu);
      break;
    }
    case 0x61: { // adc idx
      uint32_t low = 0;
      uint32_t high = cpu_adrIdx(cpu, &low);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x62: { // per rll
      uint16_t value = cpu_readOpcodeWord(cpu, false);
      cpu_idle(cpu);
      cpu_pushWord(cpu, cpu->pc + (int16_t) value, true);
      break;
    }
    case 0x63: { // adc sr
      uint32_t low = 0;
      uint32_t high = cpu_adrSr(cpu, &low);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x64: { // stz dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_stz(cpu, low, high);
      break;
    }
    case 0x65: { // adc dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x66: { // ror dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_ror(cpu, low, high);
      break;
    }
    case 0x67: { // adc idl
      uint32_t low = 0;
      uint32_t high = cpu_adrIdl(cpu, &low);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x68: { // pla imp
      cpu_idle(cpu);
      cpu_idle(cpu);
      if(cpu->mf) {
        cpu_checkInt(cpu);
        cpu->a = (cpu->a & 0xff00) | cpu_pullByte(cpu);
      } else {
        cpu->a = cpu_pullWord(cpu, true);
      }
      cpu_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x69: { // adc imm(m)
      uint32_t low = 0;
      uint32_t high = cpu_adrImm(cpu, &low, false);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x6a: { // rora imp
      cpu_adrImp(cpu);
      bool carry = cpu->a & 1;
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | ((cpu->a >> 1) & 0x7f) | (cpu->c << 7);
      } else {
        cpu->a = (cpu->a >> 1) | (cpu->c << 15);
      }
      cpu->c = carry;
      cpu_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x6b: { // rtl imp
      cpu_idle(cpu);
      cpu_idle(cpu);
      cpu->pc = cpu_pullWord(cpu, false) + 1;
      cpu_checkInt(cpu);
      cpu->k = cpu_pullByte(cpu);
      break;
    }
    case 0x6c: { // jmp ind
      uint16_t adr = cpu_readOpcodeWord(cpu, false);
      cpu->pc = cpu_readWord(cpu, adr, (adr + 1) & 0xffff, true);
      break;
    }
    case 0x6d: { // adc abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x6e: { // ror abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_ror(cpu, low, high);
      break;
    }
    case 0x6f: { // adc abl
      uint32_t low = 0;
      uint32_t high = cpu_adrAbl(cpu, &low);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x70: { // bvs rel
      cpu_doBranch(cpu, cpu->v);
      break;
    }
    case 0x71: { // adc idy(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrIdy(cpu, &low, false);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x72: { // adc idp
      uint32_t low = 0;
      uint32_t high = cpu_adrIdp(cpu, &low);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x73: { // adc isy
      uint32_t low = 0;
      uint32_t high = cpu_adrIsy(cpu, &low);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x74: { // stz dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_stz(cpu, low, high);
      break;
    }
    case 0x75: { // adc dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x76: { // ror dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_ror(cpu, low, high);
      break;
    }
    case 0x77: { // adc ily
      uint32_t low = 0;
      uint32_t high = cpu_adrIly(cpu, &low);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x78: { // sei imp
      cpu_adrImp(cpu);
      cpu->i = true;
      break;
    }
    case 0x79: { // adc aby(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAby(cpu, &low, false);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x7a: { // ply imp
      cpu_idle(cpu);
      cpu_idle(cpu);
      if(cpu->xf) {
        cpu_checkInt(cpu);
        cpu->y = cpu_pullByte(cpu);
      } else {
        cpu->y = cpu_pullWord(cpu, true);
      }
      cpu_setZN(cpu, cpu->y, cpu->xf);
      break;
    }
    case 0x7b: { // tdc imp
      cpu_adrImp(cpu);
      cpu->a = cpu->dp;
      cpu_setZN(cpu, cpu->a, false);
      break;
    }
    case 0x7c: { // jmp iax
      uint16_t adr = cpu_readOpcodeWord(cpu, false);
      cpu_idle(cpu);
      cpu->pc = cpu_readWord(cpu, (cpu->k << 16) | ((adr + cpu->x) & 0xffff), (cpu->k << 16) | ((adr + cpu->x + 1) & 0xffff), true);
      break;
    }
    case 0x7d: { // adc abx(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, false);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x7e: { // ror abx
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, true);
      cpu_ror(cpu, low, high);
      break;
    }
    case 0x7f: { // adc alx
      uint32_t low = 0;
      uint32_t high = cpu_adrAlx(cpu, &low);
      cpu_adc(cpu, low, high);
      break;
    }
    case 0x80: { // bra rel
      cpu_doBranch(cpu, true);
      break;
    }
    case 0x81: { // sta idx
      uint32_t low = 0;
      uint32_t high = cpu_adrIdx(cpu, &low);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x82: { // brl rll
      cpu->pc += (int16_t) cpu_readOpcodeWord(cpu, false);
      cpu_checkInt(cpu);
      cpu_idle(cpu);
      break;
    }
    case 0x83: { // sta sr
      uint32_t low = 0;
      uint32_t high = cpu_adrSr(cpu, &low);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x84: { // sty dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_sty(cpu, low, high);
      break;
    }
    case 0x85: { // sta dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x86: { // stx dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_stx(cpu, low, high);
      break;
    }
    case 0x87: { // sta idl
      uint32_t low = 0;
      uint32_t high = cpu_adrIdl(cpu, &low);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x88: { // dey imp
      cpu_adrImp(cpu);
      if(cpu->xf) {
        cpu->y = (cpu->y - 1) & 0xff;
      } else {
        cpu->y--;
      }
      cpu_setZN(cpu, cpu->y, cpu->xf);
      break;
    }
    case 0x89: { // biti imm(m)
      if(cpu->mf) {
        cpu_checkInt(cpu);
        uint8_t result = (cpu->a & 0xff) & cpu_readOpcode(cpu);
        cpu->z = result == 0;
      } else {
        uint16_t result = cpu->a & cpu_readOpcodeWord(cpu, true);
        cpu->z = result == 0;
      }
      break;
    }
    case 0x8a: { // txa imp
      cpu_adrImp(cpu);
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | (cpu->x & 0xff);
      } else {
        cpu->a = cpu->x;
      }
      cpu_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x8b: { // phb imp
      cpu_idle(cpu);
      cpu_checkInt(cpu);
      cpu_pushByte(cpu, cpu->db);
      break;
    }
    case 0x8c: { // sty abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_sty(cpu, low, high);
      break;
    }
    case 0x8d: { // sta abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x8e: { // stx abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_stx(cpu, low, high);
      break;
    }
    case 0x8f: { // sta abl
      uint32_t low = 0;
      uint32_t high = cpu_adrAbl(cpu, &low);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x90: { // bcc rel
      cpu_doBranch(cpu, !cpu->c);
      break;
    }
    case 0x91: { // sta idy
      uint32_t low = 0;
      uint32_t high = cpu_adrIdy(cpu, &low, true);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x92: { // sta idp
      uint32_t low = 0;
      uint32_t high = cpu_adrIdp(cpu, &low);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x93: { // sta isy
      uint32_t low = 0;
      uint32_t high = cpu_adrIsy(cpu, &low);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x94: { // sty dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_sty(cpu, low, high);
      break;
    }
    case 0x95: { // sta dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x96: { // stx dpy
      uint32_t low = 0;
      uint32_t high = cpu_adrDpy(cpu, &low);
      cpu_stx(cpu, low, high);
      break;
    }
    case 0x97: { // sta ily
      uint32_t low = 0;
      uint32_t high = cpu_adrIly(cpu, &low);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x98: { // tya imp
      cpu_adrImp(cpu);
      if(cpu->mf) {
        cpu->a = (cpu->a & 0xff00) | (cpu->y & 0xff);
      } else {
        cpu->a = cpu->y;
      }
      cpu_setZN(cpu, cpu->a, cpu->mf);
      break;
    }
    case 0x99: { // sta aby
      uint32_t low = 0;
      uint32_t high = cpu_adrAby(cpu, &low, true);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x9a: { // txs imp
      cpu_adrImp(cpu);
      cpu->sp = cpu->x;
      break;
    }
    case 0x9b: { // txy imp
      cpu_adrImp(cpu);
      if(cpu->xf) {
        cpu->y = cpu->x & 0xff;
      } else {
        cpu->y = cpu->x;
      }
      cpu_setZN(cpu, cpu->y, cpu->xf);
      break;
    }
    case 0x9c: { // stz abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_stz(cpu, low, high);
      break;
    }
    case 0x9d: { // sta abx
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, true);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0x9e: { // stz abx
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, true);
      cpu_stz(cpu, low, high);
      break;
    }
    case 0x9f: { // sta alx
      uint32_t low = 0;
      uint32_t high = cpu_adrAlx(cpu, &low);
      cpu_sta(cpu, low, high);
      break;
    }
    case 0xa0: { // ldy imm(x)
      uint32_t low = 0;
      uint32_t high = cpu_adrImm(cpu, &low, true);
      cpu_ldy(cpu, low, high);
      break;
    }
    case 0xa1: { // lda idx
      uint32_t low = 0;
      uint32_t high = cpu_adrIdx(cpu, &low);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xa2: { // ldx imm(x)
      uint32_t low = 0;
      uint32_t high = cpu_adrImm(cpu, &low, true);
      cpu_ldx(cpu, low, high);
      break;
    }
    case 0xa3: { // lda sr
      uint32_t low = 0;
      uint32_t high = cpu_adrSr(cpu, &low);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xa4: { // ldy dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_ldy(cpu, low, high);
      break;
    }
    case 0xa5: { // lda dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xa6: { // ldx dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_ldx(cpu, low, high);
      break;
    }
    case 0xa7: { // lda idl
      uint32_t low = 0;
      uint32_t high = cpu_adrIdl(cpu, &low);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xa8: { // tay imp
      cpu_adrImp(cpu);
      if(cpu->xf) {
        cpu->y = cpu->a & 0xff;
      } else {
        cpu->y = cpu->a;
      }
      cpu_setZN(cpu, cpu->y, cpu->xf);
      break;
    }
    case 0xa9: { // lda imm(m)
      uint32_t low = 0;
      uint32_t high = cpu_adrImm(cpu, &low, false);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xaa: { // tax imp
      cpu_adrImp(cpu);
      if(cpu->xf) {
        cpu->x = cpu->a & 0xff;
      } else {
        cpu->x = cpu->a;
      }
      cpu_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xab: { // plb imp
      cpu_idle(cpu);
      cpu_idle(cpu);
      cpu_checkInt(cpu);
      cpu->db = cpu_pullByte(cpu);
      cpu_setZN(cpu, cpu->db, true);
      break;
    }
    case 0xac: { // ldy abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_ldy(cpu, low, high);
      break;
    }
    case 0xad: { // lda abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xae: { // ldx abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_ldx(cpu, low, high);
      break;
    }
    case 0xaf: { // lda abl
      uint32_t low = 0;
      uint32_t high = cpu_adrAbl(cpu, &low);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xb0: { // bcs rel
      cpu_doBranch(cpu, cpu->c);
      break;
    }
    case 0xb1: { // lda idy(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrIdy(cpu, &low, false);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xb2: { // lda idp
      uint32_t low = 0;
      uint32_t high = cpu_adrIdp(cpu, &low);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xb3: { // lda isy
      uint32_t low = 0;
      uint32_t high = cpu_adrIsy(cpu, &low);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xb4: { // ldy dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_ldy(cpu, low, high);
      break;
    }
    case 0xb5: { // lda dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xb6: { // ldx dpy
      uint32_t low = 0;
      uint32_t high = cpu_adrDpy(cpu, &low);
      cpu_ldx(cpu, low, high);
      break;
    }
    case 0xb7: { // lda ily
      uint32_t low = 0;
      uint32_t high = cpu_adrIly(cpu, &low);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xb8: { // clv imp
      cpu_adrImp(cpu);
      cpu->v = false;
      break;
    }
    case 0xb9: { // lda aby(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAby(cpu, &low, false);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xba: { // tsx imp
      cpu_adrImp(cpu);
      if(cpu->xf) {
        cpu->x = cpu->sp & 0xff;
      } else {
        cpu->x = cpu->sp;
      }
      cpu_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xbb: { // tyx imp
      cpu_adrImp(cpu);
      if(cpu->xf) {
        cpu->x = cpu->y & 0xff;
      } else {
        cpu->x = cpu->y;
      }
      cpu_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xbc: { // ldy abx(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, false);
      cpu_ldy(cpu, low, high);
      break;
    }
    case 0xbd: { // lda abx(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, false);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xbe: { // ldx aby(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAby(cpu, &low, false);
      cpu_ldx(cpu, low, high);
      break;
    }
    case 0xbf: { // lda alx
      uint32_t low = 0;
      uint32_t high = cpu_adrAlx(cpu, &low);
      cpu_lda(cpu, low, high);
      break;
    }
    case 0xc0: { // cpy imm(x)
      uint32_t low = 0;
      uint32_t high = cpu_adrImm(cpu, &low, true);
      cpu_cpy(cpu, low, high);
      break;
    }
    case 0xc1: { // cmp idx
      uint32_t low = 0;
      uint32_t high = cpu_adrIdx(cpu, &low);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xc2: { // rep imm(s)
      uint8_t val = cpu_readOpcode(cpu);
      cpu_checkInt(cpu);
      cpu_setFlags(cpu, cpu_getFlags(cpu) & ~val);
      cpu_idle(cpu);
      break;
    }
    case 0xc3: { // cmp sr
      uint32_t low = 0;
      uint32_t high = cpu_adrSr(cpu, &low);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xc4: { // cpy dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_cpy(cpu, low, high);
      break;
    }
    case 0xc5: { // cmp dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xc6: { // dec dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_dec(cpu, low, high);
      break;
    }
    case 0xc7: { // cmp idl
      uint32_t low = 0;
      uint32_t high = cpu_adrIdl(cpu, &low);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xc8: { // iny imp
      cpu_adrImp(cpu);
      if(cpu->xf) {
        cpu->y = (cpu->y + 1) & 0xff;
      } else {
        cpu->y++;
      }
      cpu_setZN(cpu, cpu->y, cpu->xf);
      break;
    }
    case 0xc9: { // cmp imm(m)
      uint32_t low = 0;
      uint32_t high = cpu_adrImm(cpu, &low, false);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xca: { // dex imp
      cpu_adrImp(cpu);
      if(cpu->xf) {
        cpu->x = (cpu->x - 1) & 0xff;
      } else {
        cpu->x--;
      }
      cpu_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xcb: { // wai imp
      cpu->waiting = true;
      cpu_idle(cpu);
      cpu_idle(cpu);
      break;
    }
    case 0xcc: { // cpy abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_cpy(cpu, low, high);
      break;
    }
    case 0xcd: { // cmp abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xce: { // dec abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_dec(cpu, low, high);
      break;
    }
    case 0xcf: { // cmp abl
      uint32_t low = 0;
      uint32_t high = cpu_adrAbl(cpu, &low);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xd0: { // bne rel
      cpu_doBranch(cpu, !cpu->z);
      break;
    }
    case 0xd1: { // cmp idy(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrIdy(cpu, &low, false);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xd2: { // cmp idp
      uint32_t low = 0;
      uint32_t high = cpu_adrIdp(cpu, &low);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xd3: { // cmp isy
      uint32_t low = 0;
      uint32_t high = cpu_adrIsy(cpu, &low);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xd4: { // pei dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_pushWord(cpu, cpu_readWord(cpu, low, high, false), true);
      break;
    }
    case 0xd5: { // cmp dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xd6: { // dec dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_dec(cpu, low, high);
      break;
    }
    case 0xd7: { // cmp ily
      uint32_t low = 0;
      uint32_t high = cpu_adrIly(cpu, &low);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xd8: { // cld imp
      cpu_adrImp(cpu);
      cpu->d = false;
      break;
    }
    case 0xd9: { // cmp aby(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAby(cpu, &low, false);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xda: { // phx imp
      cpu_idle(cpu);
      if(cpu->xf) {
        cpu_checkInt(cpu);
        cpu_pushByte(cpu, cpu->x);
      } else {
        cpu_pushWord(cpu, cpu->x, true);
      }
      break;
    }
    case 0xdb: { // stp imp
      cpu_idle(cpu);
      cpu_idle(cpu);
      if(cpu->runCoprocessorHook()) {
        break;
      }
      if(cpu->cop_size > 0) {
        fprintf(stderr, "coprocessor hook declined request at %06x\n", ((cpu->k << 16) | (cpu->pc.raw() - 1)));
      } else {
        fprintf(stderr, "stp encountered at %06x\n", cpu->pc - 1);
      }
      break;
    }
    case 0xdc: { // jml ial
      uint16_t adr = cpu_readOpcodeWord(cpu, false);
      cpu->pc = cpu_readWord(cpu, adr, (adr + 1) & 0xffff, false);
      cpu_checkInt(cpu);
      cpu->k = cpu_read(cpu, (adr + 2) & 0xffff);
      break;
    }
    case 0xdd: { // cmp abx(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, false);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xde: { // dec abx
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, true);
      cpu_dec(cpu, low, high);
      break;
    }
    case 0xdf: { // cmp alx
      uint32_t low = 0;
      uint32_t high = cpu_adrAlx(cpu, &low);
      cpu_cmp(cpu, low, high);
      break;
    }
    case 0xe0: { // cpx imm(x)
      uint32_t low = 0;
      uint32_t high = cpu_adrImm(cpu, &low, true);
      cpu_cpx(cpu, low, high);
      break;
    }
    case 0xe1: { // sbc idx
      uint32_t low = 0;
      uint32_t high = cpu_adrIdx(cpu, &low);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xe2: { // sep imm(s)
      uint8_t val = cpu_readOpcode(cpu);
      cpu_checkInt(cpu);
      cpu_setFlags(cpu, cpu_getFlags(cpu) | val);
      cpu_idle(cpu);
      break;
    }
    case 0xe3: { // sbc sr
      uint32_t low = 0;
      uint32_t high = cpu_adrSr(cpu, &low);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xe4: { // cpx dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_cpx(cpu, low, high);
      break;
    }
    case 0xe5: { // sbc dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xe6: { // inc dp
      uint32_t low = 0;
      uint32_t high = cpu_adrDp(cpu, &low);
      cpu_inc(cpu, low, high);
      break;
    }
    case 0xe7: { // sbc idl
      uint32_t low = 0;
      uint32_t high = cpu_adrIdl(cpu, &low);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xe8: { // inx imp
      cpu_adrImp(cpu);
      if(cpu->xf) {
        cpu->x = (cpu->x + 1) & 0xff;
      } else {
        cpu->x++;
      }
      cpu_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xe9: { // sbc imm(m)
      uint32_t low = 0;
      uint32_t high = cpu_adrImm(cpu, &low, false);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xea: { // nop imp
      cpu_adrImp(cpu);
      // no operation
      break;
    }
    case 0xeb: { // xba imp
      uint8_t low = cpu->a & 0xff;
      uint8_t high = cpu->a >> 8;
      cpu->a = (low << 8) | high;
      cpu_setZN(cpu, high, true);
      cpu_idle(cpu);
      cpu_checkInt(cpu);
      cpu_idle(cpu);
      break;
    }
    case 0xec: { // cpx abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_cpx(cpu, low, high);
      break;
    }
    case 0xed: { // sbc abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xee: { // inc abs
      uint32_t low = 0;
      uint32_t high = cpu_adrAbs(cpu, &low);
      cpu_inc(cpu, low, high);
      break;
    }
    case 0xef: { // sbc abl
      uint32_t low = 0;
      uint32_t high = cpu_adrAbl(cpu, &low);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xf0: { // beq rel
      cpu_doBranch(cpu, cpu->z);
      break;
    }
    case 0xf1: { // sbc idy(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrIdy(cpu, &low, false);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xf2: { // sbc idp
      uint32_t low = 0;
      uint32_t high = cpu_adrIdp(cpu, &low);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xf3: { // sbc isy
      uint32_t low = 0;
      uint32_t high = cpu_adrIsy(cpu, &low);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xf4: { // pea imm(l)
      cpu_pushWord(cpu, cpu_readOpcodeWord(cpu, false), true);
      break;
    }
    case 0xf5: { // sbc dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xf6: { // inc dpx
      uint32_t low = 0;
      uint32_t high = cpu_adrDpx(cpu, &low);
      cpu_inc(cpu, low, high);
      break;
    }
    case 0xf7: { // sbc ily
      uint32_t low = 0;
      uint32_t high = cpu_adrIly(cpu, &low);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xf8: { // sed imp
      cpu_adrImp(cpu);
      cpu->d = true;
      break;
    }
    case 0xf9: { // sbc aby(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAby(cpu, &low, false);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xfa: { // plx imp
      cpu_idle(cpu);
      cpu_idle(cpu);
      if(cpu->xf) {
        cpu_checkInt(cpu);
        cpu->x = cpu_pullByte(cpu);
      } else {
        cpu->x = cpu_pullWord(cpu, true);
      }
      cpu_setZN(cpu, cpu->x, cpu->xf);
      break;
    }
    case 0xfb: { // xce imp
      cpu_adrImp(cpu);
      bool temp = cpu->c;
      cpu->c = cpu->e;
      cpu->e = temp;
      cpu_setFlags(cpu, cpu_getFlags(cpu)); // updates x and m flags, clears upper half of x and y if needed
      break;
    }
    case 0xfc: { // jsr iax
      uint8_t adrl = cpu_readOpcode(cpu);
      cpu_pushWord(cpu, cpu->pc, false);
      uint16_t adr = adrl | (cpu_readOpcode(cpu) << 8);
      cpu_idle(cpu);
      uint16_t value = cpu_readWord(cpu, (cpu->k << 16) | ((adr + cpu->x) & 0xffff), (cpu->k << 16) | ((adr + cpu->x + 1) & 0xffff), true);
      cpu->pc = value;
      break;
    }
    case 0xfd: { // sbc abx(r)
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, false);
      cpu_sbc(cpu, low, high);
      break;
    }
    case 0xfe: { // inc abx
      uint32_t low = 0;
      uint32_t high = cpu_adrAbx(cpu, &low, true);
      cpu_inc(cpu, low, high);
      break;
    }
    case 0xff: { // sbc alx
      uint32_t low = 0;
      uint32_t high = cpu_adrAlx(cpu, &low);
      cpu_sbc(cpu, low, high);
      break;
    }
    default: {
      printf("Unknown opcode: 0x%02x\n", opcode);
      break;
    }
  }
}

static const char* opcodeNames[256] = {
  "brk #$%02x     ", "ora ($%02x,x)  ", "cop #$%02x     ", "ora $%02x,s    ", "tsb $%02x      ",   "ora $%02x      ", "asl $%02x      ", "ora [$%02x]    ", "php          ", "ora #$%s   ",   "asl          ", "phd          ", "tsb $%04x    ", "ora $%04x    ", "asl $%04x    ", "ora $%06x  ",
  "bpl $%04x    ",   "ora ($%02x),y  ", "ora ($%02x)    ", "ora ($%02x,s),y", "trb $%02x      ",   "ora $%02x,x    ", "asl $%02x,x    ", "ora [$%02x],y  ", "clc          ", "ora $%04x,y  ", "inc          ", "tcs          ", "trb $%04x    ", "ora $%04x,x  ", "asl $%04x,x  ", "ora $%06x,x",
  "jsr $%04x    ",   "and ($%02x,x)  ", "jsl $%06x  ",     "and $%02x,s    ", "bit $%02x      ",   "and $%02x      ", "rol $%02x      ", "and [$%02x]    ", "plp          ", "and #$%s   ",   "rol          ", "pld          ", "bit $%04x    ", "and $%04x    ", "rol $%04x    ", "and $%06x  ",
  "bmi $%04x    ",   "and ($%02x),y  ", "and ($%02x)    ", "and ($%02x,s),y", "bit $%02x,x    ",   "and $%02x,x    ", "rol $%02x,x    ", "and [$%02x],y  ", "sec          ", "and $%04x,y  ", "dec          ", "tsc          ", "bit $%04x,x  ", "and $%04x,x  ", "rol $%04x,x  ", "and $%06x,x",
  "rti          ",   "eor ($%02x,x)  ", "wdm #$%02x     ", "eor $%02x,s    ", "mvp $%02x, $%02x ", "eor $%02x      ", "lsr $%02x      ", "eor [$%02x]    ", "pha          ", "eor #$%s   ",   "lsr          ", "phk          ", "jmp $%04x    ", "eor $%04x    ", "lsr $%04x    ", "eor $%06x  ",
  "bvc $%04x    ",   "eor ($%02x),y  ", "eor ($%02x)    ", "eor ($%02x,s),y", "mvn $%02x, $%02x ", "eor $%02x,x    ", "lsr $%02x,x    ", "eor [$%02x],y  ", "cli          ", "eor $%04x,y  ", "phy          ", "tcd          ", "jml $%06x  ",   "eor $%04x,x  ", "lsr $%04x,x  ", "eor $%06x,x",
  "rts          ",   "adc ($%02x,x)  ", "per $%04x    ",   "adc $%02x,s    ", "stz $%02x      ",   "adc $%02x      ", "ror $%02x      ", "adc [$%02x]    ", "pla          ", "adc #$%s   ",   "ror          ", "rtl          ", "jmp ($%04x)  ", "adc $%04x    ", "ror $%04x    ", "adc $%06x  ",
  "bvs $%04x    ",   "adc ($%02x),y  ", "adc ($%02x)    ", "adc ($%02x,s),y", "stz $%02x,x    ",   "adc $%02x,x    ", "ror $%02x,x    ", "adc [$%02x],y  ", "sei          ", "adc $%04x,y  ", "ply          ", "tdc          ", "jmp ($%04x,x)", "adc $%04x,x  ", "ror $%04x,x  ", "adc $%06x,x",
  "bra $%04x    ",   "sta ($%02x,x)  ", "brl $%04x    ",   "sta $%02x,s    ", "sty $%02x      ",   "sta $%02x      ", "stx $%02x      ", "sta [$%02x]    ", "dey          ", "bit #$%s   ",   "txa          ", "phb          ", "sty $%04x    ", "sta $%04x    ", "stx $%04x    ", "sta $%06x  ",
  "bcc $%04x    ",   "sta ($%02x),y  ", "sta ($%02x)    ", "sta ($%02x,s),y", "sty $%02x,x    ",   "sta $%02x,x    ", "stx $%02x,y    ", "sta [$%02x],y  ", "tya          ", "sta $%04x,y  ", "txs          ", "txy          ", "stz $%04x    ", "sta $%04x,x  ", "stz $%04x,x  ", "sta $%06x,x",
  "ldy #$%s   ",     "lda ($%02x,x)  ", "ldx #$%s   ",     "lda $%02x,s    ", "ldy $%02x      ",   "lda $%02x      ", "ldx $%02x      ", "lda [$%02x]    ", "tay          ", "lda #$%s   ",   "tax          ", "plb          ", "ldy $%04x    ", "lda $%04x    ", "ldx $%04x    ", "lda $%06x  ",
  "bcs $%04x    ",   "lda ($%02x),y  ", "lda ($%02x)    ", "lda ($%02x,s),y", "ldy $%02x,x    ",   "lda $%02x,x    ", "ldx $%02x,y    ", "lda [$%02x],y  ", "clv          ", "lda $%04x,y  ", "tsx          ", "tyx          ", "ldy $%04x,x  ", "lda $%04x,x  ", "ldx $%04x,y  ", "lda $%06x,x",
  "cpy #$%s   ",     "cmp ($%02x,x)  ", "rep #$%02x     ", "cmp $%02x,s    ", "cpy $%02x      ",   "cmp $%02x      ", "dec $%02x      ", "cmp [$%02x]    ", "iny          ", "cmp #$%s   ",   "dex          ", "wai          ", "cpy $%04x    ", "cmp $%04x    ", "dec $%04x    ", "cmp $%06x  ",
  "bne $%04x    ",   "cmp ($%02x),y  ", "cmp ($%02x)    ", "cmp ($%02x,s),y", "pei $%02x      ",   "cmp $%02x,x    ", "dec $%02x,x    ", "cmp [$%02x],y  ", "cld          ", "cmp $%04x,y  ", "phx          ", "stp          ", "jml [$%04x]  ", "cmp $%04x,x  ", "dec $%04x,x  ", "cmp $%06x,x",
  "cpx #$%s   ",     "sbc ($%02x,x)  ", "sep #$%02x     ", "sbc $%02x,s    ", "cpx $%02x      ",   "sbc $%02x      ", "inc $%02x      ", "sbc [$%02x]    ", "inx          ", "sbc #$%s   ",   "nop          ", "xba          ", "cpx $%04x    ", "sbc $%04x    ", "inc $%04x    ", "sbc $%06x  ",
  "beq $%04x    ",   "sbc ($%02x),y  ", "sbc ($%02x)    ", "sbc ($%02x,s),y", "pea #$%04x   ",     "sbc $%02x,x    ", "inc $%02x,x    ", "sbc [$%02x],y  ", "sed          ", "sbc $%04x,y  ", "plx          ", "xce          ", "jsr ($%04x,x)", "sbc $%04x,x  ", "inc $%04x,x  ", "sbc $%06x,x"
};

static const int opcodeType[256] = {
  1, 1, 1, 1, 1, 1, 1, 1, 0, 4, 0, 0, 2, 2, 2, 3,
  6, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 0, 2, 2, 2, 3,
  2, 1, 3, 1, 1, 1, 1, 1, 0, 4, 0, 0, 2, 2, 2, 3,
  6, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 0, 2, 2, 2, 3,
  0, 1, 1, 1, 8, 1, 1, 1, 0, 4, 0, 0, 2, 2, 2, 3,
  6, 1, 1, 1, 8, 1, 1, 1, 0, 2, 0, 0, 3, 2, 2, 3,
  0, 1, 7, 1, 1, 1, 1, 1, 0, 4, 0, 0, 2, 2, 2, 3,
  6, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 0, 2, 2, 2, 3,
  6, 1, 7, 1, 1, 1, 1, 1, 0, 4, 0, 0, 2, 2, 2, 3,
  6, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 0, 2, 2, 2, 3,
  5, 1, 5, 1, 1, 1, 1, 1, 0, 4, 0, 0, 2, 2, 2, 3,
  6, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 0, 2, 2, 2, 3,
  5, 1, 1, 1, 1, 1, 1, 1, 0, 4, 0, 0, 2, 2, 2, 3,
  6, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 0, 2, 2, 2, 3,
  5, 1, 1, 1, 1, 1, 1, 1, 0, 4, 0, 0, 2, 2, 2, 3,
  6, 1, 1, 1, 2, 1, 1, 1, 0, 2, 0, 0, 2, 2, 2, 3
};

uint8_t cpu_getInstructionSize(uint8_t opcode, bool mf, bool xf) {
  switch(opcodeType[opcode]) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 3;
    case 3: return 4;
    case 4: return mf ? 2 : 3;
    case 5: return xf ? 2 : 3;
    case 6: return 2;
    case 7: return 3;
    case 8: return 3;
    default: return 1;
  }
}

void cpu_disassembleInstruction(uint32_t address, bool mf, bool xf, const uint8_t* bytes, uint8_t size, CpuInstructionInfo* info) {
  cpu_decodeInstruction(address, mf, xf, bytes, size, info);
}

static void cpu_trimRight(char* text) {
  size_t len = strlen(text);
  while(len > 0 && text[len - 1] == ' ') {
    text[--len] = '\0';
  }
}

static void cpu_splitInstructionText(const char* formatted, CpuInstructionInfo* info) {
  const char* operandStart = strchr(formatted, ' ');
  if(operandStart == nullptr) {
    snprintf(info->mnemonic, sizeof(info->mnemonic), "%s", formatted);
    info->operands[0] = '\0';
    return;
  }

  size_t mnemonicLength = static_cast<size_t>(operandStart - formatted);
  if(mnemonicLength >= sizeof(info->mnemonic)) {
    mnemonicLength = sizeof(info->mnemonic) - 1;
  }
  memcpy(info->mnemonic, formatted, mnemonicLength);
  info->mnemonic[mnemonicLength] = '\0';

  while(*operandStart == ' ') {
    operandStart++;
  }
  snprintf(info->operands, sizeof(info->operands), "%s", operandStart);
  cpu_trimRight(info->operands);
}

void Cpu::beginInstructionTrace() {
  tracedInstructionAddress = (k << 16) | pc.raw();
  tracedInstructionMf = mf;
  tracedInstructionXf = xf;
  tracedInstructionSize = 0;
  memset(tracedInstructionBytes, 0, sizeof(tracedInstructionBytes));
}

void Cpu::appendInstructionByte(uint8_t value) {
  if(tracedInstructionSize < sizeof(tracedInstructionBytes)) {
    tracedInstructionBytes[tracedInstructionSize++] = value;
  }
}

void Cpu::emitInstructionTrace() {
  if(instructionHook == nullptr || tracedInstructionSize == 0) {
    return;
  }

  CpuInstructionInfo info = {};
  cpu_decodeInstruction(
    tracedInstructionAddress,
    tracedInstructionMf,
    tracedInstructionXf,
    tracedInstructionBytes,
    tracedInstructionSize,
    &info
  );
  instructionHook(instructionHookUserData, this, &info);
}

static void cpu_decodeInstruction(
  uint32_t address,
  bool mf,
  bool xf,
  const uint8_t* bytes,
  uint8_t size,
  CpuInstructionInfo* info
) {
  memset(info, 0, sizeof(*info));
  info->address = address;
  info->opcode = bytes[0];
  info->size = size;
  memcpy(info->bytes, bytes, size > sizeof(info->bytes) ? sizeof(info->bytes) : size);

  const uint8_t opcode = bytes[0];
  const uint8_t byte = size > 1 ? bytes[1] : 0;
  const uint8_t byte2 = size > 2 ? bytes[2] : 0;
  const uint8_t byte3 = size > 3 ? bytes[3] : 0;
  const uint16_t word = (byte2 << 8) | byte;
  const uint32_t longv = (byte3 << 16) | word;
  const uint16_t pc = static_cast<uint16_t>(address & 0xffff);
  const uint16_t rel = static_cast<uint16_t>(pc + 2 + static_cast<int8_t>(byte));
  const uint16_t rell = static_cast<uint16_t>(pc + 3 + static_cast<int16_t>(word));

  switch(opcodeType[opcode]) {
    case 0:
      snprintf(info->formatted, sizeof(info->formatted), "%s", opcodeNames[opcode]);
      break;
    case 1:
      snprintf(info->formatted, sizeof(info->formatted), opcodeNames[opcode], byte);
      break;
    case 2:
      snprintf(info->formatted, sizeof(info->formatted), opcodeNames[opcode], word);
      break;
    case 3:
      snprintf(info->formatted, sizeof(info->formatted), opcodeNames[opcode], longv);
      break;
    case 4: {
      char num[5] = "    ";
      if(mf) {
        snprintf(num, sizeof(num), "%02x", byte);
      } else {
        snprintf(num, sizeof(num), "%04x", word);
      }
      snprintf(info->formatted, sizeof(info->formatted), opcodeNames[opcode], num);
      break;
    }
    case 5: {
      char num[5] = "    ";
      if(xf) {
        snprintf(num, sizeof(num), "%02x", byte);
      } else {
        snprintf(num, sizeof(num), "%04x", word);
      }
      snprintf(info->formatted, sizeof(info->formatted), opcodeNames[opcode], num);
      break;
    }
    case 6:
      snprintf(info->formatted, sizeof(info->formatted), opcodeNames[opcode], rel);
      break;
    case 7:
      snprintf(info->formatted, sizeof(info->formatted), opcodeNames[opcode], rell);
      break;
    case 8:
      snprintf(info->formatted, sizeof(info->formatted), opcodeNames[opcode], byte2, byte);
      break;
    default:
      snprintf(info->formatted, sizeof(info->formatted), "db $%02x", opcode);
      break;
  }

  cpu_trimRight(info->formatted);
  cpu_splitInstructionText(info->formatted, info);

  for(char* p = info->mnemonic; *p != '\0'; ++p) {
    if(*p >= 'a' && *p <= 'z') {
      *p = static_cast<char>(*p - ('a' - 'A'));
    }
  }
}

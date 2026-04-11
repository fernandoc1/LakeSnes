
#ifndef SNES_H
#define SNES_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Snes Snes;
typedef void (*SnesAccessHook)(void* userData, uint32_t adr, uint8_t val, bool write);
typedef void (*SnesMemoryAccessCallback)(void* userData, Snes* snes, uint32_t adr, uint8_t val, bool write);
typedef void (*LakesnesMemoryAccessCallbackRegistrar)(void* userData, uint32_t adr, SnesMemoryAccessCallback callback, void* callbackUserData);
typedef void (*LakesnesMemoryAccessCallbackRegistrationHook)(Snes* snes, LakesnesMemoryAccessCallbackRegistrar registrar, void* registrarUserData);

#define LAKESNES_MEMORY_ACCESS_CALLBACK_REGISTRATION_SYMBOL "lakesnes_register_memory_access_callback"

#include "cpu.h"
#include "apu.h"
#include "dma.h"
#include "ppu.h"
#include "cart.h"
#include "input.h"
#include "statehandler.h"

#define SNES_RAM_SIZE 0x20000

struct Snes {
  Cpu* cpu;
  Apu* apu;
  Ppu* ppu;
  Dma* dma;
  Cart* cart;
  bool palTiming;
  // input
  Input* input1;
  Input* input2;
  // ram
  uint8_t ram[SNES_RAM_SIZE];
  uint32_t ramAdr;
  // frame timing
  uint16_t hPos;
  uint16_t vPos;
  uint32_t frames;
  uint64_t cycles;
  uint64_t syncCycle;
  // cpu handling
  double apuCatchupCycles;
  // nmi / irq
  bool hIrqEnabled;
  bool vIrqEnabled;
  bool nmiEnabled;
  uint16_t hTimer;
  uint16_t vTimer;
  bool inNmi;
  bool irqCondition;
  bool inIrq;
  bool inVblank;
  // joypad handling
  uint16_t portAutoRead[4]; // as read by auto-joypad read
  bool autoJoyRead;
  uint16_t autoJoyTimer; // times how long until reading is done
  bool ppuLatch;
  // multiplication/division
  uint8_t multiplyA;
  uint16_t multiplyResult;
  uint16_t divideA;
  uint16_t divideResult;
  // misc
  bool fastMem;
  uint8_t openBus;
  uint32_t romFileSize;
  uint32_t romFileHeaderSize;
  bool printRtl;
  SnesAccessHook accessHook;
  void* accessHookUserData;
  void** memoryAccessCallbackPages;
  uint32_t memoryAccessCallbackCount;
};

Snes* snes_init(void);
void snes_free(Snes* snes);
void snes_reset(Snes* snes, bool hard);
void snes_handleState(Snes* snes, StateHandler* sh);
void snes_runFrame(Snes* snes);
// used by dma, cpu
void snes_runCycles(Snes* snes, int cycles);
void snes_syncCycles(Snes* snes, bool start, int syncCycles);
uint8_t snes_readBBus(Snes* snes, uint8_t adr);
void snes_writeBBus(Snes* snes, uint8_t adr, uint8_t val);
uint8_t snes_read(Snes* snes, uint32_t adr);
void snes_write(Snes* snes, uint32_t adr, uint8_t val);
bool snes_getRomFileOffset(const Snes* snes, uint32_t address, uint32_t* fileOffset);
void snes_cpuIdle(Snes* snes, bool waiting);
uint8_t snes_cpuRead(Snes* snes, uint32_t adr);
void snes_cpuWrite(Snes* snes, uint32_t adr, uint8_t val);
// debugging
void snes_runCpuCycle(Snes* snes);
void snes_runSpcCycle(Snes* snes);

// snes_other.c functions:

enum { pixelFormatXRGB = 0, pixelFormatRGBX = 1 };

bool snes_loadRom(Snes* snes, const uint8_t* data, int length);
void snes_setButtonState(Snes* snes, int player, int button, bool pressed);
void snes_setPixelFormat(Snes* snes, int pixelFormat);
void snes_setPixels(Snes* snes, uint8_t* pixelData);
void snes_setSamples(Snes* snes, int16_t* sampleData, int samplesPerFrame);
int snes_saveBattery(Snes* snes, uint8_t* data);
bool snes_loadBattery(Snes* snes, uint8_t* data, int size);
int snes_saveState(Snes* snes, uint8_t* data);
bool snes_loadState(Snes* snes, uint8_t* data, int size);
void snes_setAccessHook(Snes* snes, SnesAccessHook hook, void* userData);
void snes_setMemoryAccessCallback(Snes* snes, uint32_t adr, SnesMemoryAccessCallback callback, void* userData);
void snes_clearMemoryAccessCallbacks(Snes* snes);

#endif

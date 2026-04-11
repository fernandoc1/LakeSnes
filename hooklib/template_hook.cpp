#include <stdio.h>

#include "cpu.h"
#include "snes.h"

// Build:
//   make -C hooklib
//
// Run:
//   ./lakesnes --cop-lib ./hooklib/libhooklib.so game.sfc
//
// Optional startup registration:
//   If this library exports lakesnes_register_memory_access_callback(),
//   LakeSnes will call it once during startup so you can register
//   per-address bus callbacks.
//
// Protocol:
//   COP #$xx sets the base address.
//   WDM #$yy appends payload bytes starting at that base address.
//   STP dispatches this function.
//
static void onWram1600Access(void* userData, Snes* snes, uint32_t adr, uint8_t val, bool write) {
  const char* label = static_cast<const char*>(userData);
  const uint32_t pc = cpu_getCurrentInstructionAddress(snes->cpu);
  uint32_t fileOffset = 0;
  const bool hasFileOffset = snes_getRomFileOffset(snes, pc, &fileOffset);
  if(hasFileOffset) {
    fprintf(
      stderr,
      "hooklib: %s %s at %06x value=%02x pc=%06x file@%06x\n",
      label != NULL ? label : "memory callback",
      write ? "write" : "read",
      adr & 0xffffff,
      val,
      pc,
      fileOffset);
    return;
  }
  fprintf(
    stderr,
    "hooklib: %s %s at %06x value=%02x pc=%06x\n",
    label != NULL ? label : "memory callback",
    write ? "write" : "read",
    adr & 0xffffff,
    val,
    pc);
}

extern "C" void lakesnes_register_memory_access_callback(
  Snes* snes,
  LakesnesMemoryAccessCallbackRegistrar registrar,
  void* registrarUserData
) {
  (void) snes;
  registrar(registrarUserData, 0x7e1600, onWram1600Access, (void*) "watch $7E1600");
  registrar(registrarUserData, 0x7e1601, onWram1600Access, (void*) "watch $7E1601");
  fprintf(stderr, "hooklib: registered memory access callbacks for $7E1600-$7E1601\n");
}

// Exported symbol name must match LAKESNES_COPROCESSOR_HOOK_SYMBOL.
extern "C" bool lakesnes_cop_execute(
  void* userData,
  Snes* snes,
  uint8_t address,
  const uint8_t* data,
  uint16_t size
) {
  (void) userData;
  (void) snes;

  fprintf(stderr, "hooklib: COP=%02x size=%u\n", address, size);
  for(uint16_t i = 0; i < size; ++i) {
    fprintf(stderr, "hooklib: data[%u]=%02x\n", i, data[i]);
  }

  // Example: read WRAM directly from the console state.
  uint8_t wram00 = snes->ram[0x0000];
  fprintf(stderr, "hooklib: WRAM[$0000]=%02x\n", wram00);

  // Example: use payload byte 0 to update WRAM at $7E0010.
  if(size > 0) {
    snes->ram[0x0010] = data[0];
    fprintf(stderr, "hooklib: wrote %02x to $7E0010\n", data[0]);
  }

  // Resume execution after STP.
  snes->cpu->stopped = false;
  return true;
}

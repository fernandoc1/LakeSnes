#include <stdio.h>

#include "cpu.h"
#include "snes.h"

// Build:
//   make -C hooklib
//
// Run:
//   ./lakesnes --cop-lib ./hooklib/libhooklib.so game.sfc
//
// Protocol:
//   COP #$xx sets the base address.
//   WDM #$yy appends payload bytes starting at that base address.
//   STP dispatches this function.
//
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

#include <stdint.h>
#include <stdio.h>
#include <cstring>

#include "cpu.h"
#include "snes.h"

static bool ff6_handleFunction01(Snes* snes, Cpu* cpu) {
  fprintf(stderr, "hooklib_ff6: function 01 called snes=%p, cpu=%p\n", (void*)snes, (void*)cpu);
  return true;
}

extern "C" bool lakesnes_cop_execute(
  void* userData,
  Snes* snes,
  uint8_t address,
  const uint8_t* data,
  uint16_t size
) {
  (void) userData;
  (void) address;
  Cpu* cpu = snes->cpu;

  if(size == 0) {
    fprintf(stderr, "hooklib_ff6: missing function id in COP payload\n");
    return false;
  }

  switch(data[0]) {
    case 0x01:
      cpu->stopped = false;
      return ff6_handleFunction01(snes, cpu);
    default:
      fprintf(stderr, "hooklib_ff6: unknown function id %02x\n", data[0]);
      return false;
  }
}

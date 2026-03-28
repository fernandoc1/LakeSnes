#include <stdint.h>
#include <stdio.h>
#include <cstring>

#include "cpu.h"
#include "snes.h"

static bool ff6_handleFunction01(Snes* snes) {
  fprintf(stderr, "hooklib_ff6: function 01 called snes=%p\n", snes);
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

  if(size == 0) {
    fprintf(stderr, "hooklib_ff6: missing function id in COP payload\n");
    return false;
  }

  switch(data[0]) {
    case 0x01:
      snes->cpu->stopped = false;
      return ff6_handleFunction01(snes);
    default:
      fprintf(stderr, "hooklib_ff6: unknown function id %02x\n", data[0]);
      return false;
  }
}

#include <stdint.h>
#include <stdio.h>
#include <cstring>

#include "cpu.h"
#include "snes.h"

static bool ff6_handleFunction02(Snes* snes) {
  uint8_t* ram = (uint8_t*)snes->ram;
  fprintf(stderr, "hooklib_ff6: function 01 called\n");
  memset(ram + 0x001600, 0x05, 0x001694 - 0x001600);
  return true;
}

static bool ff6_handleFunction01(Snes* snes) {
  uint8_t* ram = (uint8_t*)snes->ram;

  fprintf(stderr, "hooklib_ff6: function 01 called\n");

  // --------------------------------------------------
  // Clear ONLY character structs (safe)
  // --------------------------------------------------
  memset(ram + 0x1600, 0x00, 0x001694 - 0x001600);

  // --------------------------------------------------
  // Party definition
  // --------------------------------------------------
  ram[0x1600] = 0x01; // Locke
  ram[0x1601] = 0x00; // Terra
  ram[0x1602] = 0x02; // Cyan
  ram[0x1603] = 0x03; // Shadow

  // --------------------------------------------------
  // Minimal valid character initializer
  // --------------------------------------------------
  auto init_char = [&](uint8_t id) {
    uint16_t base = 0x1600 + (id * 0x25);

    // Character ID (CRITICAL)
    ram[base + 0x00] = id;

    // Level (must be non-zero)
    ram[base + 0x08] = 10;

    // HP (current + max)
    ram[base + 0x0C] = 0x2C;
    ram[base + 0x0D] = 0x01;
    ram[base + 0x0E] = 0x2C;
    ram[base + 0x0F] = 0x01;

    // Commands (CRITICAL)
    ram[base + 0x15] = 0x00; // Fight
    ram[base + 0x16] = 0x01; // Item
    ram[base + 0x17] = 0x02; // Magic
    ram[base + 0x18] = 0x05; // Steal (just to prove it works)

    // Give some basic stats so engine accepts it
    ram[base + 0x09] = 10; // Vigor
    ram[base + 0x0A] = 10; // Speed
    ram[base + 0x0B] = 10; // Stamina
  };

  init_char(0x01); // Locke
  init_char(0x00); // Terra
  init_char(0x02); // Cyan
  init_char(0x03); // Shadow

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

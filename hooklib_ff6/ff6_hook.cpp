#include <stdint.h>
#include <stdio.h>
#include <cstring>

#include "cpu.h"
#include "snes.h"

static uint8_t read8(Cpu* cpu, uint32_t address) {
  return cpu->read(cpu->mem, address);
}

static void write8(Cpu* cpu, uint32_t address, uint8_t value) {
  cpu->write(cpu->mem, address, value);
}

static uint16_t read16(Cpu* cpu, uint32_t address) {
  return static_cast<uint16_t>(read8(cpu, address) | (read8(cpu, address + 1) << 8));
}

static bool ff6_handleFunction01(Snes* snes, Cpu* cpu) {
  (void) snes;
  uint8_t* mem = static_cast<uint8_t*>(cpu->mem);

  uint16_t x = read16(cpu, 0x000000);
  fprintf(stderr, "hooklib_ff6: function 01 called with x=%04x\n", x);
  memset(mem + 0x001600, 0x05, 0x001694 - 0x001600);

  return true;
}


static bool ff6_handleFunction02(Snes* snes, Cpu* cpu) {
  (void)snes;

  uint8_t* mem = static_cast<uint8_t*>(cpu->mem);

  uint16_t x = read16(cpu, 0x000000);
  fprintf(stderr, "hooklib_ff6: function 01 called with x=%04x\n", x);

  // --------------------------------------------------
  // 1. Mark characters as recruited (VERY IMPORTANT)
  // --------------------------------------------------
  mem[0x1EDC] = 0xFF;
  mem[0x1EDD] = 0xFF;

  // --------------------------------------------------
  // 2. Define party (4 members)
  // --------------------------------------------------
  mem[0x1600] = 0x01; // Locke
  mem[0x1601] = 0x00; // Terra
  mem[0x1602] = 0x02; // Cyan
  mem[0x1603] = 0x03; // Shadow

  // --------------------------------------------------
  // 3. Initialize each character safely
  // --------------------------------------------------
  auto init_char = [&](uint8_t char_id, uint16_t hp) {
    uint16_t base = 0x1600 + (char_id * 0x25);

    // Current HP
    mem[base + 0x0C] = hp & 0xFF;
    mem[base + 0x0D] = hp >> 8;

    // Max HP
    mem[base + 0x0E] = hp & 0xFF;
    mem[base + 0x0F] = hp >> 8;

    // Clear status (alive, no poison, etc.)
    mem[base + 0x18] = 0x00;
    mem[base + 0x19] = 0x00;
  };

  init_char(0x01, 300); // Locke
  init_char(0x00, 300); // Terra
  init_char(0x02, 300); // Cyan
  init_char(0x03, 300); // Shadow

  return true;
}

extern "C" bool lakesnes_cop_execute(
  void* userData,
  Snes* snes,
  Cpu* cpu,
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
      cpu->stopped = false;
      return ff6_handleFunction01(snes, cpu);
    default:
      fprintf(stderr, "hooklib_ff6: unknown function id %02x\n", data[0]);
      return false;
  }
}

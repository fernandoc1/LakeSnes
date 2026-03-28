#include <stdint.h>
#include <stdio.h>
#include <cstring>

#include "cpu.h"
#include "snes.h"
/*
static uint8_t read8(Cpu* cpu, uint32_t address) {
  return cpu->read(cpu->snes, address);
}

static void write8(Cpu* cpu, uint32_t address, uint8_t value) {
  cpu->write(cpu->snes, address, value);
}

static uint16_t read16(Cpu* cpu, uint32_t address) {
  return static_cast<uint16_t>(read8(cpu, address) | (read8(cpu, address + 1) << 8));
}

static bool ff6_handleFunction02(Snes* snes, Cpu* cpu) {
  (void) snes;
  uint8_t* mem = static_cast<uint8_t*>(cpu->mem);

  uint16_t x = read16(cpu, 0x000000);
  fprintf(stderr, "hooklib_ff6: function 01 called with x=%04x\n", x);
  memset(mem + 0x001600, 0x05, 0x001694 - 0x001600);

  return true;
}
*/

static bool ff6_handleFunction01(Snes* snes, Cpu* cpu) {
  fprintf(stderr, "hooklib_ff6: function 01 called snes=%p, cpu=%p\n", (void*)snes, (void*)cpu);

  uint8_t* mem = (uint8_t*)cpu->snes;

  // ---------------------------------------
  // 1. Recruited
  // ---------------------------------------
  mem[0x1EDC] = 0xFF;
  mem[0x1EDD] = 0xFF;

  // ---------------------------------------
  // 2. Party definition (field layer)
  // ---------------------------------------
  mem[0x1600] = 0x01; // Locke
  mem[0x1601] = 0x00; // Terra
  mem[0x1602] = 0x02; // Cyan
  mem[0x1603] = 0x03; // Shadow

  // ---------------------------------------
  // 3. ACTIVE PARTY (CRITICAL FIX)
  // ---------------------------------------
  mem[0x1A6E] = 0x01;
  mem[0x1A6F] = 0x00;
  mem[0x1A70] = 0x02;
  mem[0x1A71] = 0x03;

  // ---------------------------------------
  // 4. Enable actors
  // ---------------------------------------
  mem[0x1850] = 1;
  mem[0x1851] = 1;
  mem[0x1852] = 1;
  mem[0x1853] = 1;

  // ---------------------------------------
  // 5. Initialize characters
  // ---------------------------------------
  auto init_char = [&](uint8_t id) {
    uint16_t base = 0x1600 + (id * 0x25);

    mem[base + 0x0C] = 0x2C;
    mem[base + 0x0D] = 0x01;
    mem[base + 0x0E] = 0x2C;
    mem[base + 0x0F] = 0x01;

    mem[base + 0x18] = 0;
    mem[base + 0x19] = 0;
  };

  init_char(0x01);
  init_char(0x00);
  init_char(0x02);
  init_char(0x03);

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

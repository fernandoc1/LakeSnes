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

  uint16_t x = read16(cpu, 0x000000);
  fprintf(stderr, "hooklib_ff6: function 01 called with x=%04x\n", x);
  memset(static_cast<uint8_t*>(cpu->mem) + 0x001600, 0x05, 0x001694 - 0x001600);

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

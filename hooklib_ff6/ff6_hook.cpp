#include <stdint.h>
#include <stdio.h>
#include <cstring>
#include <ctime>

#include "cpu.h"
#include "snes.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ----------------------------------------
// Main function (FULLY SELF-CONTAINED)
// ----------------------------------------
static bool ff6_handleFillPartyData(Snes* snes) {
    if (!snes || !snes->ram) return false;

    uint8_t* mem = (uint8_t*)snes->ram;
    
mem[0x1600] = 0x00;
mem[0x1602] = 0xBF;
mem[0x1603] = 0xBF;
mem[0x1604] = 0xBF;
mem[0x1605] = 0xBF;
mem[0x1606] = 0xBF;
mem[0x1607] = 0xFF;
mem[0x1608] = 0x04;
mem[0x1609] = 0x28;
mem[0x160B] = 0x4D;
mem[0x160D] = 0x18;
mem[0x160F] = 0x1D;
mem[0x1611] = 0xE4;
mem[0x1614] = 0x08;
mem[0x1617] = 0x03;
mem[0x1618] = 0x02;
mem[0x1619] = 0x01;
mem[0x161A] = 0x1F;
mem[0x161B] = 0x21;
mem[0x161C] = 0x1C;
mem[0x161D] = 0x27;
mem[0x161F] = 0x01;
mem[0x1620] = 0x5A;
mem[0x1621] = 0x69;
mem[0x1622] = 0x84;
mem[0x1623] = 0xFF;
mem[0x1624] = 0xFF;
mem[0x1806] = 0x20;
mem[0x1807] = 0x0E;
mem[0x1808] = 0x96;
mem[0x1809] = 0x84;
mem[0x180A] = 0x83;
mem[0x180B] = 0x86;
mem[0x180C] = 0x84;
mem[0x180D] = 0xFF;
mem[0x180E] = 0x03;
mem[0x180F] = 0x3B;
mem[0x1811] = 0x5B;
mem[0x1815] = 0x08;
mem[0x1817] = 0x84;
mem[0x181A] = 0x08;
mem[0x181D] = 0xFF;
mem[0x181E] = 0xFF;
mem[0x181F] = 0x01;
mem[0x1820] = 0x28;
mem[0x1821] = 0x23;
mem[0x1822] = 0x2E;
mem[0x1823] = 0x1D;
mem[0x1825] = 0x0A;
mem[0x1826] = 0x5A;
mem[0x1827] = 0x69;
mem[0x1828] = 0x84;
mem[0x1829] = 0xFF;
mem[0x182A] = 0xFF;
mem[0x182B] = 0x21;
mem[0x182C] = 0x0E;
mem[0x182D] = 0x95;
mem[0x182E] = 0x88;
mem[0x182F] = 0x82;
mem[0x1830] = 0x8A;
mem[0x1831] = 0x92;
mem[0x1832] = 0xFF;
mem[0x1833] = 0x03;
mem[0x1834] = 0x40;
mem[0x1836] = 0x5D;
mem[0x183A] = 0x08;
mem[0x183C] = 0x84;
mem[0x183F] = 0x08;
mem[0x1842] = 0xFF;
mem[0x1843] = 0xFF;
mem[0x1844] = 0x01;
mem[0x1845] = 0x29;
mem[0x1846] = 0x24;
mem[0x1847] = 0x2D;
mem[0x1848] = 0x1C;
mem[0x184A] = 0x0A;
mem[0x184B] = 0x5A;
mem[0x184C] = 0x69;
mem[0x184D] = 0x84;
mem[0x184E] = 0xFF;
mem[0x184F] = 0xFF;
mem[0x1850] = 0xC1;
mem[0x185E] = 0x49;
mem[0x185F] = 0x51;
mem[0x1860] = 0x32;
mem[0x1861] = 0x0D;
mem[0x1866] = 0x43;
mem[0x1869] = 0xE8;
mem[0x1969] = 0x04;
mem[0x1A6D] = 0x01;
mem[0x1A6E] = 0xFF;
mem[0x1A9B] = 0xFF;
mem[0x201D] = 0x07;
mem[0x2022] = 0xFF;
mem[0x2023] = 0xFF;
mem[0x2024] = 0xFF;
mem[0x2025] = 0x60;
mem[0x202E] = 0x1D;
mem[0x202F] = 0x0E;
mem[0x2033] = 0x00;
mem[0x2034] = 0x02;
mem[0x2035] = 0x01;
mem[0x2037] = 0x01;
mem[0x2038] = 0x00;
mem[0x203A] = 0x1D;
mem[0x203B] = 0x0E;
mem[0x203F] = 0x00;
mem[0x2042] = 0x00;
mem[0x2043] = 0x01;
mem[0x2044] = 0x00;
mem[0x2046] = 0x1D;
mem[0x2047] = 0x0E;
mem[0x204B] = 0x00;
mem[0x204E] = 0x00;
mem[0x204F] = 0x01;
mem[0x2050] = 0x00;
mem[0x2092] = 0x2D;
mem[0x2093] = 0x7F;
mem[0x2094] = 0x21;
mem[0x2095] = 0x05;
fprintf(stderr, "hooklib_ff6: filling party data in RAM\n");
//mem[0x2099] = 0x00;
//mem[0x209D] = 0x00;
//mem[0x209E] = 0x00;
mem[0x209F] = 0x7F;
mem[0x20A0] = 0x61;
mem[0x20A1] = 0x04;
//mem[0x20A5] = 0x00;
//mem[0x20A9] = 0x00;
//mem[0x20AD] = 0x00;
//mem[0x20B1] = 0x00;
//mem[0x20B5] = 0x00;
//mem[0x20B9] = 0x00;
//mem[0x20BD] = 0x00;
//mem[0x20C1] = 0x00;
//mem[0x20C5] = 0x00;
//mem[0x20C9] = 0x00;
//mem[0x20CD] = 0x00;
//mem[0x20D1] = 0x00;
//mem[0x20D5] = 0x00;
//mem[0x20D9] = 0x00;
//mem[0x20DD] = 0x00;
//mem[0x20E1] = 0x00;
//mem[0x20E5] = 0x00;
//mem[0x20E9] = 0x00;
//mem[0x20ED] = 0x00;
//mem[0x20F1] = 0x00;
//mem[0x20F5] = 0x00;
//mem[0x20F9] = 0x00;
//mem[0x20FD] = 0x00;

    return true;
}

static bool ff6_handleFunction05(Snes* snes) {
  fprintf(stderr, "hooklib_ff6: function 05 called\n");
  return true;


  //Dump RAM for debugging in /tmp/
  //Create a unique filename based on the current time
  char filename[256];
  snprintf(filename, sizeof(filename), "/tmp/ff6_ram_dump_%lu.bin", (unsigned long)time(NULL));
  FILE* f = fopen(filename, "wb");
  if(f) {
    fwrite(snes->ram, 1, SNES_RAM_SIZE, f);
    fclose(f);
    fprintf(stderr, "hooklib_ff6: RAM dumped to %s\n", filename);
  } else {
    fprintf(stderr, "hooklib_ff6: failed to dump RAM to %s\n",  filename);
  }
  return true;
}

static bool cpy__ff6_handleFillPartyData(Snes* snes) {
  fprintf(stderr, "hooklib_ff6: function 07 called\n");
  //return true;

  //Load ram from file for testing
  char filename[256];
  uint8_t testData[SNES_RAM_SIZE] = {0};
  snprintf(filename, sizeof(filename), "/tmp/ff6_ram_dump.bin");
  FILE* f = fopen(filename, "rb");
  if(f) {
    fread(testData, 1, SNES_RAM_SIZE, f);
    fclose(f);
    fprintf(stderr, "hooklib_ff6: RAM loaded from %s\n", filename);
  } else {
    fprintf(stderr, "hooklib_ff6: failed to load RAM from %s\n", filename);
  }
  //memcpy(snes->ram + 0x1600, testData + 0x1600, 0x450);
  memcpy(snes->ram + 0x1600, testData + 0x1600, 0x500);
  //memcpy(snes->ram + 0x6400, testData + 0x6400, 0x0200);
  return true;
}

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
  } else {
    fprintf(
      stderr,
      "hooklib: %s %s at %06x value=%02x pc=%06x no file offset\n",
      label != NULL ? label : "memory callback",
      write ? "write" : "read",
      adr & 0xffffff,
      val,
      pc);
  }
}

extern "C" void lakesnes_register_memory_access_callback(
  Snes* snes,
  LakesnesMemoryAccessCallbackRegistrar registrar,
  void* registrarUserData
) {
  (void) snes;
  registrar(registrarUserData, 0x7e1600, onWram1600Access, (void*) "watch $7E1600");
  fprintf(stderr, "hooklib: registered memory access callbacks for $7E1600-$7E1601\n");
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
    //case 0x01:
    //  snes->cpu->stopped = false;
    //  return ff6_handleFunction01(snes);
    case 0x05:
      snes->cpu->stopped = false;
      return ff6_handleFunction05(snes);
    case 0x07:
      snes->cpu->stopped = false;
      //return ff6_handleFunction07(snes);
      return ff6_handleFillPartyData(snes);
    default:
      fprintf(stderr, "hooklib_ff6: unknown function id %02x\n", data[0]);
      return false;
  }
}

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

    uint8_t* ram = (uint8_t*)snes->ram;
    
    // 1600 range
    ram[0x1600] = 0x00;
    ram[0x1601] = 0x00;
    ram[0x1602] = 0xBF;
    ram[0x1603] = 0xBF;
    ram[0x1604] = 0xBF;
    ram[0x1605] = 0xBF;
    ram[0x1606] = 0xBF;
    ram[0x1607] = 0xFF;
    ram[0x1608] = 0x04;
    ram[0x1609] = 0x28;
    ram[0x160A] = 0x00;
    ram[0x160B] = 0x4D;
    ram[0x160C] = 0x00;
    ram[0x160D] = 0x18;
    ram[0x160E] = 0x00;
    ram[0x160F] = 0x1D;
    ram[0x1610] = 0x00;
    ram[0x1611] = 0xE4;
    ram[0x1612] = 0x00;
    ram[0x1613] = 0x00;
    ram[0x1614] = 0x08;
    ram[0x1615] = 0x00;
    ram[0x1616] = 0x00;
    ram[0x1617] = 0x03;
    ram[0x1618] = 0x02;
    ram[0x1619] = 0x01;
    ram[0x161A] = 0x1F;
    ram[0x161B] = 0x21;
    ram[0x161C] = 0x1C;
    ram[0x161D] = 0x27;
    ram[0x161E] = 0xFF;
    ram[0x161F] = 0x01;
    ram[0x1620] = 0x5A;
    ram[0x1621] = 0x69;
    ram[0x1622] = 0x84;
    ram[0x1623] = 0xFF;
    ram[0x1624] = 0xFF;
    ram[0x1625] = 0xFF;

    // 1800 range
    ram[0x1800] = 0x20;
    ram[0x1801] = 0x0E;
    ram[0x1802] = 0x96;
    ram[0x1803] = 0x84;
    ram[0x1804] = 0x83;
    ram[0x1805] = 0x86;
    ram[0x1806] = 0x84;
    ram[0x1807] = 0xFF;
    ram[0x1808] = 0x03;
    ram[0x1809] = 0x3B;
    ram[0x180A] = 0x00;
    ram[0x180B] = 0x5B;
    ram[0x180C] = 0x00;
    ram[0x180D] = 0x00;
    ram[0x180E] = 0x00;
    ram[0x180F] = 0x08;
    ram[0x1810] = 0x00;
    ram[0x1811] = 0x84;
    ram[0x1812] = 0x00;
    ram[0x1813] = 0x00;
    ram[0x1814] = 0x08;
    ram[0x1815] = 0x00;
    ram[0x1816] = 0x00;
    ram[0x1817] = 0xFF;
    ram[0x1818] = 0xFF;
    ram[0x1819] = 0x01;
    ram[0x181A] = 0x28;
    ram[0x181B] = 0x23;
    ram[0x181C] = 0x2E;
    ram[0x181D] = 0x1D;
    ram[0x181E] = 0xFF;
    ram[0x181F] = 0x0A;
    ram[0x1820] = 0x5A;
    ram[0x1821] = 0x69;
    ram[0x1822] = 0x84;
    ram[0x1823] = 0xFF;
    ram[0x1824] = 0xFF;
    ram[0x1825] = 0x21;
    ram[0x1826] = 0x0E;
    ram[0x1827] = 0x95;
    ram[0x1828] = 0x88;
    ram[0x1829] = 0x82;
    ram[0x182A] = 0x8A;
    ram[0x182B] = 0x92;
    ram[0x182C] = 0xFF;
    ram[0x182D] = 0x03;
    ram[0x182E] = 0x40;
    ram[0x182F] = 0x00;
    ram[0x1830] = 0x5D;
    ram[0x1831] = 0x00;
    ram[0x1832] = 0x00;
    ram[0x1833] = 0x00;
    ram[0x1834] = 0x08;
    ram[0x1835] = 0x00;
    ram[0x1836] = 0x84;
    ram[0x1837] = 0x00;
    ram[0x1838] = 0x00;
    ram[0x1839] = 0x08;
    ram[0x183A] = 0x00;
    ram[0x183B] = 0x00;
    ram[0x183C] = 0xFF;
    ram[0x183D] = 0xFF;
    ram[0x183E] = 0x01;
    ram[0x183F] = 0x29;
    ram[0x1840] = 0x24;
    ram[0x1841] = 0x2D;
    ram[0x1842] = 0x1C;
    ram[0x1843] = 0xFF;
    ram[0x1844] = 0x0A;
    ram[0x1845] = 0x5A;
    ram[0x1846] = 0x69;
    ram[0x1847] = 0x84;
    ram[0x1848] = 0xFF;
    ram[0x1849] = 0xFF;
    ram[0x184A] = 0xC1;
    ram[0x184B] = 0x00;
    ram[0x184C] = 0x00;
    ram[0x184D] = 0x00;
    ram[0x184E] = 0x00;
    ram[0x184F] = 0x00;
    ram[0x1850] = 0x00;
    ram[0x1851] = 0x00;
    ram[0x1852] = 0x00;
    ram[0x1853] = 0x00;
    ram[0x1854] = 0x00;
    ram[0x1855] = 0x00;
    ram[0x1856] = 0x00;
    ram[0x1857] = 0x00;
    ram[0x1858] = 0x49;
    ram[0x1859] = 0x51;
    ram[0x185A] = 0x32;
    ram[0x185B] = 0x0D;
    ram[0x185C] = 0x00;
    ram[0x185D] = 0x00;
    ram[0x185E] = 0x00;
    ram[0x185F] = 0x00;
    ram[0x1860] = 0x43;
    ram[0x1861] = 0x00;
    ram[0x1862] = 0x00;
    ram[0x1863] = 0xE8;

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

#include <stdint.h>
#include <stdio.h>
#include <cstring>
#include <ctime>

#include "cpu.h"
#include "snes.h"

static const uint8_t ff6_actor_block[] = {
    // Extracted EXACTLY from your dump ($1800–$1850)

    0x20,0x0E,0x96,0x84,0x83,0x86,0x84,0xFF,
    0x03,0x3B,0x00,0x5B,0x00,0x00,0x00,0x08,
    0x00,0x84,0x00,0x00,0x08,0x00,0x00,0xFF,
    0xFF,0x01,0x28,0x23,0x2E,0x1D,0xFF,0x0A,

    0x21,0x0E,0x95,0x88,0x82,0x8A,0x92,0xFF,
    0x03,0x40,0x00,0x5D,0x00,0x00,0x00,0x08,
    0x00,0x84,0x00,0x00,0x08,0x00,0x00,0xFF,
    0xFF,0x01,0x29,0x24,0x2D,0x1C,0xFF,0x0A
};

static bool ff6_handleFillPartyData(Snes* snes) {
    uint8_t* ram = (uint8_t*)snes->ram;

    fprintf(stderr, "ff6: actor injection\n");

    // --- Step 1: valid character stats ---
    for (int i = 0; i < 3; i++) {
        uint8_t* c = ram + 0x1600 + (i * 0x25);

        c[0x00] = i;
        c[0x08] = 5;

        c[0x09] = 0x64; c[0x0A] = 0x00;
        c[0x0B] = 0x64; c[0x0C] = 0x00;
    }

    // --- Step 2: CRITICAL actor block ---
    memcpy(ram + 0x1800, ff6_actor_block, sizeof(ff6_actor_block));

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

static bool ff6_handleFunction07(Snes* snes) {
  fprintf(stderr, "hooklib_ff6: function 07 called\n");
  return true;

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

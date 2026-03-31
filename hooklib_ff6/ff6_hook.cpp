#include <stdint.h>
#include <stdio.h>
#include <cstring>
#include <ctime>

#include "cpu.h"
#include "snes.h"

#define FF6_PARTY_OFFSET        0x15E0   // party formation area
#define FF6_CHAR_BASE           0x1600   // character structs
#define FF6_CHAR_SIZE           0x25

#define FF6_SLOT_COUNT          4

// Actor IDs (confirmed FF6)
#define ACTOR_TERRA  0x00
#define ACTOR_BIGGS  0x0E
#define ACTOR_WEDGE  0x0F

static void ff6_initCharacter(uint8_t* ram, uint8_t id, uint16_t hp) {
    uint32_t base = FF6_CHAR_BASE + (id * FF6_CHAR_SIZE);

    // Clear struct
    memset(&ram[base], 0, FF6_CHAR_SIZE);

    // ---- Minimal required fields ----

    ram[base + 0x00] = id;        // character ID

    // HP (IMPORTANT: must be non-zero or character won't appear)
    ram[base + 0x0B] = hp & 0xFF;
    ram[base + 0x0C] = (hp >> 8);

    // Max HP
    ram[base + 0x0D] = hp & 0xFF;
    ram[base + 0x0E] = (hp >> 8);

    // Enable commands (Fight at minimum)
    ram[base + 0x15] = 0x00; // Fight

    // Clear bad status (avoid Imp issue you saw)
    ram[base + 0x01] = 0x00;
}

static bool ff6_handleFillPartyData(Snes* snes) {
    if (!snes || !snes->ram) return false;

    uint8_t* ram = (uint8_t*)snes->ram;

    // --------------------------------
    // 1. Set party formation
    // --------------------------------
    ram[FF6_PARTY_OFFSET + 0] = ACTOR_TERRA;
    ram[FF6_PARTY_OFFSET + 1] = ACTOR_BIGGS;
    ram[FF6_PARTY_OFFSET + 2] = ACTOR_WEDGE;
    ram[FF6_PARTY_OFFSET + 3] = 0xFF; // empty

    // --------------------------------
    // 2. Initialize characters
    // --------------------------------
    ff6_initCharacter(ram, ACTOR_TERRA, 300);
    ff6_initCharacter(ram, ACTOR_BIGGS, 280);
    ff6_initCharacter(ram, ACTOR_WEDGE, 260);

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

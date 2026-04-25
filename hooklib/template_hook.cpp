#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "snes.h"

// Build:
//   make -C hooklib
//
// Run:
//   ./lakesnes --cop-lib ./hooklib/libhooklib.so game.sfc
//
// Protocol:
//   COP #$xx sets the base address.
//   WDM #$yy appends payload bytes starting at that base address.
//   STP dispatches this function.

/**
 * Helper to execute a sequence of SNES CPU instructions.
 * This copies the provided code to a scratchpad area in WRAM ($7FFF00),
 * sets the CPU's PC to that address, and runs instructions until the PC
 * leaves the scratchpad area or the CPU stops.
 */
void executeSnesInstructions(Snes* snes, const uint8_t* code, size_t size) {
  if (size == 0 || size > 256) return;

  Cpu* cpu = snes->cpu;
  
  // Save CPU state that we will modify
  uint16_t old_pc = cpu->pc;
  uint8_t old_k = cpu->k;
  uint16_t old_a = cpu->a;
  uint16_t old_x = cpu->x;
  uint16_t old_y = cpu->y;
  uint16_t old_sp = cpu->sp;
  uint16_t old_dp = cpu->dp;
  uint8_t old_db = cpu->db;
  bool old_c = cpu->c, old_z = cpu->z, old_v = cpu->v, old_n = cpu->n;
  bool old_i = cpu->i, old_d = cpu->d, old_xf = cpu->xf, old_mf = cpu->mf, old_e = cpu->e;
  bool old_stopped = cpu->stopped;

  // Use the end of WRAM as a scratchpad ($7FFF00 - $7FFFFF)
  uint32_t scratchpad = 0x7FFF00;
  for (size_t i = 0; i < size; ++i) {
    snes->ram[scratchpad - 0x7E0000 + i] = code[i];
  }

  // Point PC to the scratchpad and clear stopped flag
  cpu->pc = scratchpad & 0xFFFF;
  cpu->k = (scratchpad >> 16) & 0xFF;
  cpu->stopped = false;

  uint32_t end_addr = scratchpad + size;
  
  fprintf(stderr, "hooklib: executing %zu bytes of SNES code at %06x\n", size, scratchpad);

  // Execute instructions until we leave the code buffer or hit an STP
  while (!cpu->stopped) {
    uint32_t current_pc = (cpu->k << 16) | cpu->pc;
    if (current_pc < scratchpad || current_pc >= end_addr) {
      break;
    }
    cpu_runOpcode(cpu);
  }

  // Restore previous execution point and registers
  // (Note: If you WANT the code to change registers, don't restore them)
  cpu->pc = old_pc;
  cpu->k = old_k;
  cpu->a = old_a;
  cpu->x = old_x;
  cpu->y = old_y;
  cpu->sp = old_sp;
  cpu->dp = old_dp;
  cpu->db = old_db;
  cpu->c = old_c; cpu->z = old_z; cpu->v = old_v; cpu->n = old_n;
  cpu->i = old_i; cpu->d = old_d; cpu->xf = old_xf; cpu->mf = old_mf; cpu->e = old_e;
  cpu->stopped = old_stopped;
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
  }
  fprintf(
    stderr,
    "hooklib: %s %s at %06x value=%02x pc=%06x\n",
    label != NULL ? label : "memory callback",
    write ? "write" : "read",
    adr & 0xffffff,
    val,
    pc);
}

extern "C" void lakesnes_register_memory_access_callback(
  Snes* snes,
  LakesnesMemoryAccessCallbackRegistrar registrar,
  void* registrarUserData
) {
  (void) snes;
  registrar(registrarUserData, 0x7e1600, onWram1600Access, (void*) "watch $7E1600");
  registrar(registrarUserData, 0x7e1601, onWram1600Access, (void*) "watch $7E1601");
  fprintf(stderr, "hooklib: registered memory access callbacks for $7E1600-$7E1601\n");
}

static bool g_in_hook = false;

// Exported symbol name must match LAKESNES_COPROCESSOR_HOOK_SYMBOL.
extern "C" bool lakesnes_cop_execute(
  void* userData,
  Snes* snes,
  uint8_t address,
  const uint8_t* data,
  uint16_t size
) {
  (void) userData;

  // Prevent recursion if injected code uses STP
  if (g_in_hook) return true;
  g_in_hook = true;

  fprintf(stderr, "hooklib: COP=%02x size=%u\n", address, size);

  // Example 1: If COP #$01 is called, treat 'data' as raw SNES instructions and run them.
  // Useful for dynamic code injection from the game or a script.
  if (address == 0x01 && size > 0) {
    executeSnesInstructions(snes, data, size);
  }

  // Example 2: If COP #$02 is called, manually increment WRAM $7E0010 using SNES instructions.
  // SNES Code: INC $0010 ($E6 $10)
  if (address == 0x02) {
    uint8_t inc_code[] = { 0xE6, 0x10 }; 
    executeSnesInstructions(snes, inc_code, sizeof(inc_code));
  }

  // Example 3: read WRAM directly from the console state.
  uint8_t wram00 = snes->ram[0x0000];
  fprintf(stderr, "hooklib: WRAM[$0000]=%02x\n", wram00);

  // Resume execution after STP.
  snes->cpu->stopped = false;
  g_in_hook = false;
  return true;
}

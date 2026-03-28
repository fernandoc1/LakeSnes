#include <stdio.h>

#include "rom_disasm.h"
#include "cpu.h"

static void rom_disasm_format(const CpuInstructionInfo* info, FILE* out) {
  if(info->operands[0] != '\0') {
    fprintf(out, "%06x  %-4s %s\n", info->address & 0xffffff, info->mnemonic, info->operands);
  } else {
    fprintf(out, "%06x  %s\n", info->address & 0xffffff, info->mnemonic);
  }
}

bool rom_disassemble(Snes* snes, FILE* out, int instructionLimit) {
  if(snes == NULL || out == NULL || instructionLimit <= 0) {
    return false;
  }

  uint16_t pc = snes_read(snes, 0xfffc) | (snes_read(snes, 0xfffd) << 8);
  uint8_t bank = 0x00;
  bool e = true;
  bool mf = true;
  bool xf = true;
  bool c = false;
  bool cKnown = true;

  for(int i = 0; i < instructionLimit; ++i) {
    uint32_t address = (bank << 16) | pc;
    uint8_t bytes[4] = {
      snes_read(snes, address),
      snes_read(snes, (address + 1) & 0xffffff),
      snes_read(snes, (address + 2) & 0xffffff),
      snes_read(snes, (address + 3) & 0xffffff)
    };
    uint8_t size = cpu_getInstructionSize(bytes[0], mf, xf);
    CpuInstructionInfo info = {};
    cpu_disassembleInstruction(address, mf, xf, bytes, size, &info);
    rom_disasm_format(&info, out);

    switch(bytes[0]) {
      case 0x18:
        c = false;
        cKnown = true;
        break;
      case 0x38:
        c = true;
        cKnown = true;
        break;
      case 0xc2:
        if(size > 1) {
          mf = mf && ((bytes[1] & 0x20) == 0);
          xf = xf && ((bytes[1] & 0x10) == 0);
          if(e) {
            mf = true;
            xf = true;
          }
        }
        break;
      case 0xe2:
        if(size > 1) {
          if(bytes[1] & 0x20) mf = true;
          if(bytes[1] & 0x10) xf = true;
        }
        break;
      case 0xfb:
        if(cKnown) {
          bool newCarry = e;
          e = c;
          c = newCarry;
          if(e) {
            mf = true;
            xf = true;
          }
        }
        break;
      default:
        break;
    }

    pc = static_cast<uint16_t>(pc + size);
  }

  return true;
}

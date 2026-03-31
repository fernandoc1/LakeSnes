#ifndef ROM_DISASM_H
#define ROM_DISASM_H

#include <stdio.h>
#include <stdbool.h>

#include "snes.h"

bool rom_disassemble(Snes* snes, FILE* out, int instructionLimit);
bool rom_disassemble_cfg(Snes* snes, FILE* out, int instructionLimit);

#endif

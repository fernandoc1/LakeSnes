# LakeSnes
A SNES emulator, in C

## Notice

I have decided to archive this repository. Some personal/mental issues mean that I don't really have the motivation to work on it further (or really, on emulation in general). There is a more active fork by [Dinkc64](https://github.com/dinkc64) over [here](https://github.com/dinkc64/LakeSnes), which already has a bunch of fixes, optimisations, as well as support for the CX4 expansion chip.

## About

This is a SNES emulator, written in C, mostly as a followup on my [earlier Javascript version](https://github.com/angelo-wf/SnesJs). The main drive behind rewriting it in C was C's speed. The JS version could barely run at 20 FPS on my system, whereas this C version runs at full speed.

The intent is for the actual emulation itself to be split off into a library, which can then be used in other projects. (Maybe it could be compiled for the web with Emscripten as well, to replace the core from that JS emulator). This is not done yet, and as of now a full emulator with basic frontend (using [SDL2](https://www.libsdl.org)) is build.

Performance, although much better than my JS version, is still quite bad though, especially when compared to emulators like BSNES or SNES9X (it used around 80% of one core whereas SNES9X only used around 15%, on my old hardware).

## Nightly releases

Nightly builds can be downloaded [from the releases here](https://github.com/angelo-wf/LakeSnes/releases/tag/nightly). The source-downloads there do not seem to be updated by the Github-action used to handle making releases, so are out of date.

- The macOS build is an app-bundle (includes SDL2) but not signed and notarized, and does not have proper version information. These are currently Intel-only, although manually compiling for/on Apple Silicon (arm64) works without issues. It might not run on older macOS versions.
- The Linux build depends on SDL2 being installed already and is an x86_64 (64-bit Intel) build.
- The Windows build includes `SDL2.dll` and is a 64-bit (Intel) build.

## Compiling

### MacOS (plain executable)

- Install [homebrew](https://brew.sh) (This also install the Xcode CLI-tools, providing clang and make)
- Install SDL2 with homebrew: `brew install sdl2`
- Run `make`

This build depends on SDL2 being installed with homebrew.

### MacOS (app-bundle)

- Make sure clang and make are available (from the Xcode CLI-tools)
- Download the latest SDL2 build (the `.dmg`) from the [SDL2 Releases](https://github.com/libsdl-org/SDL/releases)
- Create a directory called `sdl2` in this repo's directory and copy `SDL2.framework` from the dmg into it
- Run `make LakeSnes.app`

This is a stand-alone application (includes SDL2) and shows up in the 'open with' menu for smc, sfc and zip files.

### Linux

- Make sure clang (or gcc) and make are available
- Get SDL2-dev via the package manager (Ubuntu/Debian: `sudo apt install libsdl2-dev`)
- Run `make` (or `make CC=gcc` to use gcc)

This build depends on SDL2 being installed.

### Windows

NOTE: Only tested with Msys2 using clang for x86_64, but building for arm64 should work as well, and using gcc, other environments, other tools (Cygwin, Mingw, etc) or Visual Studio might also be possible. Some changes might be needed due to some of the includes and functions used.

- Install [Msys2](https://www.msys2.org)
- Open a `clang64` environment (and run `pacman -Suy`)
- Install clang and make: `pacman -S mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-make`
- Install SDL2: `pacman -S mingw-w64-clang-x86_64-SDL2`
- Navigate to this repo's directory (`cd /c/Users/...`)
- Run `mingw32-make lakesnes.exe` (or `mingw32-make` to build iconless exe)
- Run `cp /clang64/bin/SDL2.dll .` (to copy `SDL2.dll`)

This build depends on `SDL2.dll` being placed next to the executable.

## Usage and controls

The emulator can be run by opening `lakesnes` directly or by running `./lakesnes`, taking an optional path to a ROM-file to open. ROM-files can also be dragged on the emulator window to open them. ZIP-files also work, the first file within with a `.smc` or `.sfc` will be loaded (zip support uses [this](https://github.com/kuba--/zip) zip-library, which uses Miniz, both under the Unlicence).

## Command line options

`lakesnes` supports normal emulator startup plus a few analysis/export modes.

### Basic forms

Run the emulator, optionally opening a ROM immediately:

```bash
./lakesnes
./lakesnes path/to/game.sfc
```

Run with trace recording enabled from startup:

```bash
./lakesnes --record-trace path/to/game.sfc
```

Load a coprocessor hook library:

```bash
./lakesnes --cop-lib path/to/libhook.so path/to/game.sfc
```

Disassemble ROM bytes starting from reset:

```bash
./lakesnes --disasm-rom --disasm-notes-out path/to/disasm_notes.json [--disasm-limit N] path/to/game.sfc
```

Build a static CFG from ROM analysis:

```bash
./lakesnes --cfg-rom --cfg-out path/to/rom_cfg.dot [--cfg-limit N] [--cfg-notes-out path/to/notes.json] path/to/game.sfc
```

Record a runtime CFG during emulation and dump it on exit:

```bash
./lakesnes --runtime-cfg-out path/to/runtime_cfg.dot path/to/game.sfc
```

Record runtime mem_viewer annotations during emulation and dump them on exit:

```bash
./lakesnes --runtime-notes-out path/to/runtime_notes.json path/to/game.sfc
```

### Options

| Option | Meaning |
| ------ | ------- |
| `--record-trace` | Starts instruction trace recording automatically after the ROM is loaded. This uses the same trace file path the interactive trace hotkeys use. |
| `--cop-lib <path>` | Loads a dynamic library and installs its exported coprocessor hook (`lakesnes_cop_execute`) into the emulator CPU. |
| `--disasm-rom` | Runs reset-based ROM disassembly mode instead of launching the interactive emulator. It prints the linear disassembly to stdout. |
| `--disasm-notes-out <file.json>` | Required with `--disasm-rom`. Writes mem_viewer-compatible note annotations for the linear disassembly, keyed to ROM file offsets so the ROM and notes can be opened together in `mem_viewer/bin_view`. |
| `--disasm-limit <N>` | Limits the ROM disassembly to `N` instructions. Must be greater than zero. Default: `4096`. |
| `--cfg-rom` | Runs static control-flow graph generation mode instead of launching the interactive emulator. |
| `--cfg-out <file.dot>` | Required with `--cfg-rom`. Writes the static CFG in Graphviz DOT format. |
| `--cfg-limit <N>` | Limits static CFG analysis to `N` discovered nodes. `0` means no node limit. Default: `4096`. |
| `--cfg-notes-out <file.json>` | Optional with `--cfg-rom`. Writes mem_viewer-compatible note annotations for discovered instructions. |
| `--runtime-cfg-out <file.dot>` | Enables runtime CFG capture during normal emulation and writes the executed instruction graph to the given DOT file when the ROM is closed or the emulator exits. |
| `--runtime-notes-out <file.json>` | Enables runtime note capture during normal emulation and writes mem_viewer-compatible annotations for executed ROM instructions and cartridge ROM accesses when the ROM is closed or the emulator exits. |

### Notes

- `--disasm-rom` and `--cfg-rom` are mutually exclusive.
- `--disasm-notes-out` is mandatory when `--disasm-rom` is used.
- `--cfg-out` is mandatory when `--cfg-rom` is used.
- In `--cfg-rom` mode on POSIX systems, `SIGUSR1` prints progress and `SIGUSR2` requests a clean stop.
- Runtime CFG export is based on executed instructions only. Static CFG export is based on reachable decoded ROM instructions.
- Disassembly notes export is keyed to ROM file offsets and is intended for use with `mem_viewer/bin_view` on the ROM file itself.
- Runtime notes export is keyed to ROM file offsets and is intended for use with `mem_viewer/bin_view` on the ROM file itself.
- The plain emulator path still accepts an optional ROM path even when no other flags are supplied.

### Examples

Start the emulator with a ROM:

```bash
./lakesnes roms/ff6-en1.sfc
```

Start the emulator and immediately begin trace recording:

```bash
./lakesnes --record-trace roms/ff6-en1.sfc
```

Start the emulator with a coprocessor hook library:

```bash
./lakesnes --cop-lib ./hooklib_ff6/libff6cop.so roms/ff6-en1.sfc
```

Dump a short reset-based ROM disassembly to stdout and create mem_viewer notes:

```bash
./lakesnes --disasm-rom --disasm-notes-out /tmp/ff6_disasm_notes.json --disasm-limit 256 roms/ff6-en1.sfc
```

Open the ROM together with those linear disassembly notes in `bin_view`:

```bash
./mem_viewer/bin_view roms/ff6-en1.sfc /tmp/ff6_disasm_notes.json
```

Build a static CFG and stop after 10,000 discovered nodes:

```bash
./lakesnes --cfg-rom --cfg-out /tmp/ff6_cfg.dot --cfg-limit 10000 roms/ff6-en1.sfc
```

Build a static CFG plus mem_viewer notes:

```bash
./lakesnes --cfg-rom --cfg-out /tmp/ff6_cfg.dot --cfg-notes-out /tmp/ff6_cfg_notes.json roms/ff6-en1.sfc
```

Build an unlimited static CFG and ask it for progress from another terminal:

```bash
./lakesnes --cfg-rom --cfg-out /tmp/ff6_cfg.dot --cfg-limit 0 roms/ff6-en1.sfc
kill -USR1 <pid>
kill -USR2 <pid>
```

Capture a runtime CFG while playing normally, then dump it on exit:

```bash
./lakesnes --runtime-cfg-out /tmp/ff6_runtime_cfg.dot roms/ff6-en1.sfc
```

Capture runtime mem_viewer notes while playing normally, then dump them on exit:

```bash
./lakesnes --runtime-notes-out /tmp/ff6_runtime_notes.json roms/ff6-en1.sfc
```

Capture both runtime CFG and runtime notes in one run:

```bash
./lakesnes --runtime-cfg-out /tmp/ff6_runtime_cfg.dot --runtime-notes-out /tmp/ff6_runtime_notes.json roms/ff6-en1.sfc
```

Currently, only normal joypads are supported, and only controller 1 has controls set up.

| Button | Key         |
| ------ | ----------- |
| Up     | Up arrow    |
| Down   | Down arrow  |
| Left   | Left arrow  |
| Right  | Right arrow |
| Start  | Enter       |
| Select | Right shift |
| A      | X           |
| B      | Z           |
| X      | S           |
| Y      | A           |
| L      | D           |
| R      | C           |

Additionally, the following command are available:

| Key | Action            |
| --- | ----------------- |
| R   | Soft reset        |
| E   | Hard reset        |
| P   | Pause             |
| O   | Frame advance     |
| T   | Turbo (hold)      |
| L   | Run one CPU cycle |
| K   | Run one SPC cycle |
| J   | Dumps some data   |
| M   | Make save state   |
| N   | Load save state   |

Alt+Enter can be used to toggle fullscreen mode.

L will run one CPU cycle, and then logs the CPU state (opcode, registers, flags).
K does the same, but for the SPC instead (note that this acts as additional SPC cycles).

J currently dumps the 128K WRAM, 64K VRAM, 512B CGRAM, 544B OAM and 64K ARAM to a file called `dump.bin`.

Battery saves, save states and `dump.bin` are stored in the SDL-provided preference directory, this is usually in `~/Library/Application Support/LakeSnes` on macOS, `~/.local/share/LakeSnes` on Linux and `%USERPROFILE%\AppData\Roaming\LakeSnes` on Windows. Battery saves go in a subdirectory `saves` and save states in `states`.
Battery saves and save states are currently named after the roms full name without extension, with `.srm` or `.lss` appended respectively.

Note that the save state format and exact naming and location for battery saves and save states is still being worked on and subject to change. Further updates will likely break compatibility with older save states and battery saves might need to be moved around and/or renamed.

Minimizing or hiding the window can cause high CPU usage as this can cause v-sync to stop working.

## Compatibility

The emulator currently only supports regular LoROM, HiROM and ExHiROM games (no co-processors and such).
SPC files can not be loaded yet, but are planned.

This emulator is definitely not fully accurate. The PPU renders per scanline, so mid-scanline effects are not supported. The DSP executes on a per-sample basis. The SPC and CPU-side timing should be cycle-accurate now, but the exact timing of certain event is still somewhat off. Communication between the CPU and SPC is also not cycle-accurate.

Quite a few TODO's are scattered throughout the code for things that are currently not quite fully emulated, mostly related to edge cases and some lesser-used PPU features.

Some things that are not emulated at all are full emulation-mode for the 65816, and the test-register ($f0) for the SPC.

Some games that I have tested seem to run without obvious issues, although some games do seem to glitch somewhat or freeze. `bugs.md` contains a non-exhaustive list of games that have emulation-bugs.

## License

This project is licensed under the MIT license, see 'LICENSE.txt' for details.

It uses 'kuba--/zip' which is under the Unlicense, and links against SDL2 which is under the zlib license.

## Resources

- The WDC datasheet for the W65C816S cpu.
- The [65C816 opcodes tutorial](http://6502.org/tutorials/65c816opcodes.html) from 6502.org.
- The [65816 opcode matrix](http://www.oxyron.de/html/opcodes816.html) from oxyron.de (the cycle timing and notes there aren't fully accurate, and the addressing modes IAL and IAX are swapped for JMP (opcodes $7C and $EC)).
- The [SuperFamicon wiki](https://wiki.superfamicom.org).
- The [SnesDev Wiki](https://snesdev.mesen.ca/wiki/index.php?title=Main_Page) by Sour (seems to be abadoned).
- The [SnesDev Wiki](https://snes.nesdev.org/wiki/Main_Page) at NesDev.
- Some quick peeks at Sour's [Mesen-S source](https://github.com/SourMesen/Mesen-S), nuw superseeded by [Mesen 2](https://github.com/SourMesen/Mesen2).
- Some quick peeks at [Higan's source](https://github.com/higan-emu/higan), [Bsnes' source](https://github.com/bsnes-emu/bsnes) and [Ares' source](https://github.com/ares-emulator/ares), all formely by Near.
- The [nocach fullsnes document](https://problemkaputt.de/fullsnes.txt).
- Some posts and resources in the [SnesDev section](https://forums.nesdev.org/viewforum.php?f=12) of the NesDev forums.
- The [Super NES Programming pages](https://en.wikibooks.org/wiki/Super_NES_Programming) in Wikibooks.
- Anomie's docs from [Romhacking.net](https://www.romhacking.net/community/548/).
- Various roms (especially the CPU tests) by PeterLemon/krom found [here](https://github.com/PeterLemon/SNES).
- The 65816 tests by TomHarte found [here](https://github.com/TomHarte/ProcessorTests).
- The SPC700 tests by raddad772 found [here](https://github.com/raddad772/jsmoo/tree/main/misc/tests/GeneratedTests) (part of JSmoo, a JS-based set of emulators; tests also included in TomHarte's repo).
- Various SPC and DSP test by Blargg (from [here](https://forums.nesdev.org/viewtopic.php?f=12&t=10697&p=121027#p121027) and [here](https://forums.nesdev.org/viewtopic.php?f=12&t=18005)).
- The source for the BRR-tools from [SMW central](https://www.smwcentral.net), found [here](https://github.com/jimbo1qaz/BRRtools/tree/32-bit-samples).
- [SDL2](https://www.libsdl.org) is used for the frontend window/rendering/audio/input handling.
- [This](https://github.com/kuba--/zip) zip-library is used for zipped rom loading support.

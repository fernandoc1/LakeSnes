
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include "strings.h"

#ifdef SDL2SUBDIR
#include "SDL2/SDL.h"
#else
#include "SDL.h"
#endif

#include "zip.h"

#include "rom_disasm.h"
#include "snes.h"
#include "trace_recorder.h"
#include "tracing.h"

/* depends on behaviour:
casting uintX_t to/from intX_t does 'expceted' unsigned<->signed conversion
  ((int8_t) 255) == -1
same with assignment
  int8_t a; a = 0xff; a == -1
overflow is handled as expected
  (uint8_t a = 255; a++; a == 0; uint8_t b = 0; b--; b == 255)
clipping is handled as expected
  (uint16_t a = 0x123; uint8_t b = a; b == 0x23)
giving non 0/1 value to boolean makes it 0/1
  (bool a = 2; a == 1)
giving out-of-range vaue to function parameter clips it in range
  (void test(uint8_t a) {...}; test(255 + 1); a == 0 within test)
int is at least 32 bits
shifting into sign bit makes value negative
  int a = ((int16_t) (0x1fff << 3)) >> 3; a == -1
*/

static struct {
  // rendering
  SDL_Window* window;
  SDL_Renderer* renderer;
  SDL_Texture* texture;
  // audio
  SDL_AudioDeviceID audioDevice;
  int audioFrequency;
  int16_t* audioBuffer;
  // paths
  char* prefPath;
  const char* pathSeparator;
  // snes, timing
  Snes* snes;
  float wantedFrames;
  int wantedSamples;
  // loaded rom
  bool loaded;
  char* romName;
  char* savePath;
  char* statePath;
  char* tracePath;
  char* traceDisassemblyPath;
  char* runtimeCfgPath;
  char* runtimeNotesPath;
  char* runtimeWramNotesPath;
  TraceRecorder* traceRecorder;
  bool recordTraceOnStartup;
  void* copLibHandle;
} glb = {};

static uint8_t* readFile(const char* name, int* length);
static uint8_t* readRomImage(const char* path, int* length);
static void loadRom(const char* path);
static void closeRom(void);
static void setPaths(const char* path);
static void setTitle(const char* path);
static bool checkExtention(const char* name, bool forZip);
static void playAudio(void);
static void renderScreen(void);
static void handleInput(int keyCode, bool pressed);
static bool saveTraceFile(bool verbose);
static void beginTraceRecording(bool fromStartup);
static int runRomDisassembly(const char* romPath, int instructionLimit, bool cfgMode, const char* outputPath, const char* notesOutputPath);
static bool loadCoprocessorLibrary(const char* path);
static void unloadCoprocessorLibrary(void);
static bool dumpRuntimeWramBinary(const char* jsonPath);
static void handleCfgStopSignal(int signalNumber);
static void handleCfgStatusSignal(int signalNumber);
static void printCfgProgress(void* userData, const RomDisassemblyProgress* progress);
static void registerMemoryAccessCallbackFromLibrary(void* userData, uint32_t adr, SnesMemoryAccessCallback callback, void* callbackUserData);

static volatile sig_atomic_t g_cfgStopRequested = 0;
static volatile sig_atomic_t g_cfgStatusRequested = 0;

int main(int argc, char** argv) {
  bool disasmRomMode = false;
  bool cfgRomMode = false;
  int disasmInstructionLimit = 4096;
  const char* romPath = NULL;
  const char* copLibPath = NULL;
  const char* cfgOutputPath = NULL;
  const char* cfgNotesOutputPath = NULL;
  const char* runtimeCfgOutputPath = NULL;
  const char* runtimeNotesOutputPath = NULL;
  const char* runtimeWramNotesOutputPath = NULL;

  for(int i = 1; i < argc; ++i) {
    if(strcmp(argv[i], "--record-trace") == 0) {
      glb.recordTraceOnStartup = true;
    } else if(strcmp(argv[i], "--cop-lib") == 0) {
      if(i + 1 >= argc) {
        fprintf(stderr, "Missing value for --cop-lib\n");
        return 1;
      }
      copLibPath = argv[++i];
    } else if(strcmp(argv[i], "--disasm-rom") == 0) {
      disasmRomMode = true;
    } else if(strcmp(argv[i], "--cfg-rom") == 0) {
      cfgRomMode = true;
    } else if(strcmp(argv[i], "--disasm-limit") == 0) {
      if(i + 1 >= argc) {
        fprintf(stderr, "Missing value for --disasm-limit\n");
        return 1;
      }
      disasmInstructionLimit = atoi(argv[++i]);
      if(disasmInstructionLimit <= 0) {
        fprintf(stderr, "Invalid --disasm-limit value\n");
        return 1;
      }
    } else if(strcmp(argv[i], "--cfg-limit") == 0) {
      if(i + 1 >= argc) {
        fprintf(stderr, "Missing value for --cfg-limit\n");
        return 1;
      }
      disasmInstructionLimit = atoi(argv[++i]);
      if(disasmInstructionLimit < 0) {
        fprintf(stderr, "Invalid --cfg-limit value\n");
        return 1;
      }
    } else if(strcmp(argv[i], "--cfg-out") == 0) {
      if(i + 1 >= argc) {
        fprintf(stderr, "Missing value for --cfg-out\n");
        return 1;
      }
      cfgOutputPath = argv[++i];
    } else if(strcmp(argv[i], "--cfg-notes-out") == 0) {
      if(i + 1 >= argc) {
        fprintf(stderr, "Missing value for --cfg-notes-out\n");
        return 1;
      }
      cfgNotesOutputPath = argv[++i];
    } else if(strcmp(argv[i], "--runtime-cfg-out") == 0) {
      if(i + 1 >= argc) {
        fprintf(stderr, "Missing value for --runtime-cfg-out\n");
        return 1;
      }
      runtimeCfgOutputPath = argv[++i];
    } else if(strcmp(argv[i], "--runtime-notes-out") == 0) {
      if(i + 1 >= argc) {
        fprintf(stderr, "Missing value for --runtime-notes-out\n");
        return 1;
      }
      runtimeNotesOutputPath = argv[++i];
    } else if(strcmp(argv[i], "--runtime-wram-notes-out") == 0) {
      if(i + 1 >= argc) {
        fprintf(stderr, "Missing value for --runtime-wram-notes-out\n");
        return 1;
      }
      runtimeWramNotesOutputPath = argv[++i];
    } else if(romPath == NULL) {
      romPath = argv[i];
    } else {
      fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      return 1;
    }
  }

  if(disasmRomMode && cfgRomMode) {
    fprintf(stderr, "Choose either --disasm-rom or --cfg-rom\n");
    return 1;
  }

  if(disasmRomMode) {
    if(romPath == NULL) {
      fprintf(stderr, "Usage: %s --disasm-rom [--disasm-limit N] <rom>\n", argv[0]);
      return 1;
    }
    return runRomDisassembly(romPath, disasmInstructionLimit, false, NULL, NULL);
  }

  if(cfgRomMode) {
    if(romPath == NULL || cfgOutputPath == NULL) {
      fprintf(stderr, "Usage: %s --cfg-rom --cfg-out <file.dot> [--cfg-limit N] <rom>\n", argv[0]);
      return 1;
    }
    return runRomDisassembly(romPath, disasmInstructionLimit, true, cfgOutputPath, cfgNotesOutputPath);
  }

  if(getenv("LAKESNES_DEBUG") != NULL) {
    fprintf(stderr, "Debug mode enabled. Press Enter to continue. PID: %d\n", getpid());
    getchar();
  }
  // set up SDL
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    printf("Failed to init SDL: %s\n", SDL_GetError());
    return 1;
  }
  glb.window = SDL_CreateWindow("LakeSnes", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 512, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if(glb.window == NULL) {
    printf("Failed to create window: %s\n", SDL_GetError());
    return 1;
  }
  glb.renderer = SDL_CreateRenderer(glb.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if(glb.renderer == NULL) {
    printf("Failed to create renderer: %s\n", SDL_GetError());
    return 1;
  }
  SDL_RenderSetLogicalSize(glb.renderer, 512, 480); // preserve aspect ratio
  glb.texture = SDL_CreateTexture(glb.renderer, SDL_PIXELFORMAT_RGBX8888, SDL_TEXTUREACCESS_STREAMING, 512, 480);
  if(glb.texture == NULL) {
    printf("Failed to create texture: %s\n", SDL_GetError());
    return 1;
  }
  // get pref path, create directories
  glb.prefPath = SDL_GetPrefPath("", "LakeSnes");
  char* savePath = (char*)malloc(strlen(glb.prefPath) + 6); // "saves" (5) + '\0'
  char* statePath = (char*)malloc(strlen(glb.prefPath) + 7); // "states" (6) + '\0'
  strcpy(savePath, glb.prefPath);
  strcat(savePath, "saves");
  strcpy(statePath, glb.prefPath);
  strcat(statePath, "states");
#ifdef _WIN32
  mkdir(savePath); // ignore mkdir error, this (should) mean that the directory already exists
  mkdir(statePath);
  glb.pathSeparator = "\\";
#else
  mkdir(savePath, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
  mkdir(statePath, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
  glb.pathSeparator = "/";
#endif
  free(savePath);
  free(statePath);
  // set up audio
  glb.audioFrequency = 48000;
  SDL_AudioSpec want, have;
  SDL_memset(&want, 0, sizeof(want));
  want.freq = glb.audioFrequency;
  want.format = AUDIO_S16;
  want.channels = 2;
  want.samples = 2048;
  want.callback = NULL; // use queue
  glb.audioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if(glb.audioDevice == 0) {
    printf("Failed to open audio device: %s\n", SDL_GetError());
    return 1;
  }
  glb.audioBuffer = (int16_t*)malloc(glb.audioFrequency / 50 * 4); // *2 for stereo, *2 for sizeof(int16)
  SDL_PauseAudioDevice(glb.audioDevice, 0);
  // print version
  SDL_version version;
  SDL_version compiledVersion;
  SDL_GetVersion(&version);
  SDL_VERSION(&compiledVersion);
  printf(
    "LakeSnes - Running with SDL %d.%d.%d (compiled with %d.%d.%d)\n",
    version.major, version.minor, version.patch, compiledVersion.major, compiledVersion.minor, compiledVersion.patch
  );
  // init snes, load rom
  glb.snes = snes_init();
  glb.wantedFrames = 1.0 / 60.0;
  glb.wantedSamples = glb.audioFrequency / 60;
  glb.loaded = false;
  glb.romName = NULL;
  glb.savePath = NULL;
  glb.statePath = NULL;
  glb.tracePath = NULL;
  glb.traceDisassemblyPath = NULL;
  glb.runtimeCfgPath = runtimeCfgOutputPath != NULL ? strdup(runtimeCfgOutputPath) : NULL;
  glb.runtimeNotesPath = runtimeNotesOutputPath != NULL ? strdup(runtimeNotesOutputPath) : NULL;
  glb.runtimeWramNotesPath = runtimeWramNotesOutputPath != NULL ? strdup(runtimeWramNotesOutputPath) : NULL;
  glb.traceRecorder = traceRecorder_init(glb.snes);
  glb.copLibHandle = NULL;
  if(copLibPath != NULL && !loadCoprocessorLibrary(copLibPath)) {
    return 1;
  }

  if(romPath != NULL) {
    loadRom(romPath);
  } else {
    puts("No rom loaded");
  }
  // sdl loop
  bool running = true;
  bool paused = false;
  bool runOne = false;
  bool turbo = false;
  SDL_Event event;
  int fullscreenFlags = 0;
  // timing
  uint64_t countFreq = SDL_GetPerformanceFrequency();
  uint64_t lastCount = SDL_GetPerformanceCounter();
  float timeAdder = 0.0;

  while(running) {
    while(SDL_PollEvent(&event)) {
      switch(event.type) {
        case SDL_KEYDOWN: {
          switch(event.key.keysym.sym) {
            case SDLK_r: snes_reset(glb.snes, false); break;
            case SDLK_e: snes_reset(glb.snes, true); break;
            case SDLK_o: runOne = true; break;
            case SDLK_p: paused = !paused; break;
            case SDLK_t: turbo = true; break;
            case SDLK_j: {
              char* filePath = (char*)malloc(strlen(glb.prefPath) + 9); // "dump.bin" (8) + '\0'
              strcpy(filePath, glb.prefPath);
              strcat(filePath, "dump.bin");
              printf("Dumping to %s...\n", filePath);
              FILE* f = fopen(filePath, "wb");
              if(f == NULL) {
                puts("Failed to open file for writing");
                free(filePath);
                break;
              }
              fwrite(glb.snes->ram, SNES_RAM_SIZE, 1, f);
              fwrite(glb.snes->ppu->vram, 0x10000, 1, f);
              fwrite(glb.snes->ppu->cgram, 0x200, 1, f);
              fwrite(glb.snes->ppu->oam, 0x200, 1, f);
              fwrite(glb.snes->ppu->highOam, 0x20, 1, f);
              fwrite(glb.snes->apu->ram, 0x10000, 1, f);
              fclose(f);
              free(filePath);
              break;
            }
            case SDLK_l: {
              // run one cpu cycle
              snes_runCpuCycle(glb.snes);
              char line[80];
              getProcessorStateCpu(glb.snes, line);
              puts(line);
              break;
            }
            case SDLK_k: {
              // run one spc cycle
              snes_runSpcCycle(glb.snes);
              char line[57];
              getProcessorStateSpc(glb.snes, line);
              puts(line);
              break;
            }
            case SDLK_m: {
              // save state
              int size = snes_saveState(glb.snes, NULL);
              uint8_t* stateData = (uint8_t*)malloc(size);
              snes_saveState(glb.snes, stateData);
              FILE* f = fopen(glb.statePath, "wb");
              if(f != NULL) {
                fwrite(stateData, size, 1, f);
                fclose(f);
                puts("Saved state");
              } else {
                puts("Failed to save state");
              }
              free(stateData);
              break;
            }
            case SDLK_n: {
              // load state
              int size = 0;
              uint8_t* stateData = readFile(glb.statePath, &size);
              if(stateData != NULL) {
                if(snes_loadState(glb.snes, stateData, size)) {
                  puts("Loaded state");
                } else {
                  puts("Failed to load state, file contents invalid");
                }
                free(stateData);
              } else {
                puts("Failed to load state, failed to read file");
              }
              break;
            }
            case SDLK_F5: {
              beginTraceRecording(false);
              break;
            }
            case SDLK_F6: {
              traceRecorder_end(glb.traceRecorder);
              saveTraceFile(false);
              printf("Stopped trace recording (%d instructions)\n", traceRecorder_getRecordCount(glb.traceRecorder));
              break;
            }
            case SDLK_F7: {
              if(glb.tracePath != NULL && traceRecorder_saveToFile(glb.traceRecorder, glb.tracePath)) {
                printf("Saved trace to %s\n", glb.tracePath);
              } else {
                puts("Failed to save trace");
              }
              break;
            }
            case SDLK_F8: {
              if(glb.tracePath != NULL && traceRecorder_loadFromFile(glb.traceRecorder, glb.tracePath)) {
                puts("Loaded trace");
              } else {
                puts("Failed to load trace");
              }
              break;
            }
            case SDLK_F9: {
              if(traceRecorder_restoreInitialState(glb.traceRecorder)) {
                puts("Restored initial trace state");
              } else {
                puts("Failed to restore initial trace state");
              }
              break;
            }
            case SDLK_F10: {
              if(glb.traceDisassemblyPath != NULL && traceRecorder_dumpDisassembly(glb.traceRecorder, glb.traceDisassemblyPath)) {
                printf("Dumped trace disassembly to %s\n", glb.traceDisassemblyPath);
              } else {
                puts("Failed to dump trace disassembly");
              }
              break;
            }
            case SDLK_F11: {
              const CpuInstructionInfo* info = traceRecorder_getRecord(glb.traceRecorder, 0);
              if(info != NULL) {
                char line[96];
                traceRecorder_formatRecord(info, line, sizeof(line));
                puts(line);
              } else {
                puts("Trace is empty");
              }
              break;
            }
            case SDLK_RETURN: {
              if(event.key.keysym.mod & KMOD_ALT) {
                fullscreenFlags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(glb.window, fullscreenFlags);
              }
              break;
            }
          }
          if((event.key.keysym.mod & (KMOD_ALT | KMOD_CTRL | KMOD_GUI)) == 0) {
            // only send keypress if not holding ctrl/alt/meta
            handleInput(event.key.keysym.sym, true);
          }
          break;
        }
        case SDL_KEYUP: {
          switch(event.key.keysym.sym) {
            case SDLK_t: turbo = false; break;
          }
          handleInput(event.key.keysym.sym, false);
          break;
        }
        case SDL_QUIT: {
          running = false;
          break;
        }
        case SDL_DROPFILE: {
          char* droppedFile = event.drop.file;
          loadRom(droppedFile);
          SDL_free(droppedFile);
          break;
        }
      }
    }

    uint64_t curCount = SDL_GetPerformanceCounter();
    uint64_t delta = curCount - lastCount;
    lastCount = curCount;
    float seconds = delta / (float) countFreq;
    timeAdder += seconds;
    // allow 2 ms earlier, to prevent skipping due to being just below wanted
    while(timeAdder >= glb.wantedFrames - 0.002) {
      timeAdder -= glb.wantedFrames;
      // run frame
      if(glb.loaded && (!paused || runOne)) {
        runOne = false;
        if(turbo) {
          snes_runFrame(glb.snes);
        }
        snes_runFrame(glb.snes);
        playAudio();
        renderScreen();
      }
    }

    SDL_RenderClear(glb.renderer);
    SDL_RenderCopy(glb.renderer, glb.texture, NULL, NULL);
    SDL_RenderPresent(glb.renderer); // should vsync
  }
  // close rom (saves battery)
  closeRom();
  unloadCoprocessorLibrary();
  traceRecorder_free(glb.traceRecorder);
  // free snes
  snes_free(glb.snes);
  // clean sdl and free global allocs
  SDL_PauseAudioDevice(glb.audioDevice, 1);
  SDL_CloseAudioDevice(glb.audioDevice);
  free(glb.audioBuffer);
  SDL_free(glb.prefPath);
  if(glb.romName) free(glb.romName);
  if(glb.savePath) free(glb.savePath);
  if(glb.statePath) free(glb.statePath);
  if(glb.tracePath) free(glb.tracePath);
  if(glb.traceDisassemblyPath) free(glb.traceDisassemblyPath);
  if(glb.runtimeCfgPath) free(glb.runtimeCfgPath);
  if(glb.runtimeNotesPath) free(glb.runtimeNotesPath);
  if(glb.runtimeWramNotesPath) free(glb.runtimeWramNotesPath);
  SDL_DestroyTexture(glb.texture);
  SDL_DestroyRenderer(glb.renderer);
  SDL_DestroyWindow(glb.window);
  SDL_Quit();
  return 0;
}

static void playAudio() {
  snes_setSamples(glb.snes, glb.audioBuffer, glb.wantedSamples);
  if(SDL_GetQueuedAudioSize(glb.audioDevice) <= glb.wantedSamples * 4 * 6) {
    // don't queue audio if buffer is still filled
    SDL_QueueAudio(glb.audioDevice, glb.audioBuffer, glb.wantedSamples * 4);
  }
}

static void renderScreen() {
  void* pixels = NULL;
  int pitch = 0;
  if(SDL_LockTexture(glb.texture, NULL, &pixels, &pitch) != 0) {
    printf("Failed to lock texture: %s\n", SDL_GetError());
    return;
  }
  snes_setPixels(glb.snes, (uint8_t*) pixels);
  SDL_UnlockTexture(glb.texture);
}

static void handleInput(int keyCode, bool pressed) {
  switch(keyCode) {
    case SDLK_z: snes_setButtonState(glb.snes, 1, 0, pressed); break;
    case SDLK_a: snes_setButtonState(glb.snes, 1, 1, pressed); break;
    case SDLK_RSHIFT: snes_setButtonState(glb.snes, 1, 2, pressed); break;
    case SDLK_RETURN: snes_setButtonState(glb.snes, 1, 3, pressed); break;
    case SDLK_UP: snes_setButtonState(glb.snes, 1, 4, pressed); break;
    case SDLK_DOWN: snes_setButtonState(glb.snes, 1, 5, pressed); break;
    case SDLK_LEFT: snes_setButtonState(glb.snes, 1, 6, pressed); break;
    case SDLK_RIGHT: snes_setButtonState(glb.snes, 1, 7, pressed); break;
    case SDLK_x: snes_setButtonState(glb.snes, 1, 8, pressed); break;
    case SDLK_s: snes_setButtonState(glb.snes, 1, 9, pressed); break;
    case SDLK_d: snes_setButtonState(glb.snes, 1, 10, pressed); break;
    case SDLK_c: snes_setButtonState(glb.snes, 1, 11, pressed); break;
  }
}

static bool checkExtention(const char* name, bool forZip) {
  if(name == NULL) return false;
  int length = strlen(name);
  if(length < 4) return false;
  if(forZip) {
    if(strcasecmp(name + length - 4, ".zip") == 0) return true;
  } else {
    if(strcasecmp(name + length - 4, ".smc") == 0) return true;
    if(strcasecmp(name + length - 4, ".sfc") == 0) return true;
  }
  return false;
}

static void loadRom(const char* path) {
  int length = 0;
  uint8_t* file = readRomImage(path, &length);
  if(file == NULL) {
    printf("Failed to read file '%s'\n", path);
    return;
  }
  // close currently loaded rom (saves battery)
  closeRom();
  // load new rom
  if(snes_loadRom(glb.snes, file, length)) {
    // get rom name and paths, set title
    setPaths(path);
    setTitle(glb.romName);
    // set wantedFrames and wantedSamples
    glb.wantedFrames = 1.0 / (glb.snes->palTiming ? 50.0 : 60.0);
    glb.wantedSamples = glb.audioFrequency / (glb.snes->palTiming ? 50 : 60);
    glb.loaded = true;
    // load battery for loaded rom
    int size = 0;
    uint8_t* saveData = readFile(glb.savePath, &size);
    if(saveData != NULL) {
      if(snes_loadBattery(glb.snes, saveData, size)) {
        puts("Loaded battery data");
      } else {
        puts("Failed to load battery data");
      }
      free(saveData);
    }
    if(glb.recordTraceOnStartup) beginTraceRecording(true);
    if(glb.runtimeCfgPath != NULL) {
      traceRecorder_clearRuntimeGraph(glb.traceRecorder);
      traceRecorder_setRuntimeGraphEnabled(glb.traceRecorder, true);
      printf("Started runtime CFG capture to %s\n", glb.runtimeCfgPath);
    } else {
      traceRecorder_setRuntimeGraphEnabled(glb.traceRecorder, false);
      traceRecorder_clearRuntimeGraph(glb.traceRecorder);
    }
    if(glb.runtimeNotesPath != NULL) {
      traceRecorder_clearRuntimeNotes(glb.traceRecorder);
      traceRecorder_setRuntimeNotesEnabled(glb.traceRecorder, true);
      printf("Started runtime notes capture to %s\n", glb.runtimeNotesPath);
    } else {
      traceRecorder_setRuntimeNotesEnabled(glb.traceRecorder, false);
      traceRecorder_clearRuntimeNotes(glb.traceRecorder);
    }
    if(glb.runtimeWramNotesPath != NULL) {
      traceRecorder_clearRuntimeWramNotes(glb.traceRecorder);
      traceRecorder_setRuntimeWramNotesEnabled(glb.traceRecorder, true);
      printf("Started runtime WRAM notes capture to %s\n", glb.runtimeWramNotesPath);
    } else {
      traceRecorder_setRuntimeWramNotesEnabled(glb.traceRecorder, false);
      traceRecorder_clearRuntimeWramNotes(glb.traceRecorder);
    }
  } // else, rom load failed, old rom still loaded
  free(file);
}

static int runRomDisassembly(const char* romPath, int instructionLimit, bool cfgMode, const char* outputPath, const char* notesOutputPath) {
  int length = 0;
  uint8_t* file = readRomImage(romPath, &length);
  if(file == NULL) {
    fprintf(stderr, "Failed to read ROM '%s'\n", romPath);
    return 1;
  }

  cpu_setMemViewerEnabled(false);
  Snes* snes = snes_init();
  cpu_setMemViewerEnabled(true);
  int result = 0;
  FILE* out = stdout;
  FILE* notesOut = NULL;
  bool signalHandlersInstalled = false;
#ifndef _WIN32
  struct sigaction oldSigUsr1 = {};
  struct sigaction oldSigUsr2 = {};
#endif
  if(!snes_loadRom(snes, file, length)) {
    fprintf(stderr, "Failed to load ROM '%s'\n", romPath);
    result = 1;
  } else {
    if(cfgMode) {
      out = fopen(outputPath, "w");
      if(out == NULL) {
        fprintf(stderr, "Failed to open CFG output '%s'\n", outputPath);
        result = 1;
      } else {
        if(notesOutputPath != NULL) {
          notesOut = fopen(notesOutputPath, "w");
          if(notesOut == NULL) {
            fprintf(stderr, "Failed to open CFG notes output '%s'\n", notesOutputPath);
            result = 1;
          }
        }
      }
      if(result == 0) {
        RomDisassemblyControl control = {};
        g_cfgStopRequested = 0;
        g_cfgStatusRequested = 0;
#ifndef _WIN32
        struct sigaction stopAction = {};
        stopAction.sa_handler = handleCfgStopSignal;
        sigemptyset(&stopAction.sa_mask);
        struct sigaction statusAction = {};
        statusAction.sa_handler = handleCfgStatusSignal;
        sigemptyset(&statusAction.sa_mask);
        sigaction(SIGUSR1, &statusAction, &oldSigUsr1);
        sigaction(SIGUSR2, &stopAction, &oldSigUsr2);
        signalHandlersInstalled = true;
        fprintf(stderr, "CFG export running. Send SIGUSR1 for status, SIGUSR2 to stop cleanly.\n");
#endif
        control.stopRequested = &g_cfgStopRequested;
        control.statusRequested = &g_cfgStatusRequested;
        control.statusCallback = printCfgProgress;
        control.userData = NULL;
        control.statusInterval = 50000;
        if(!rom_disassemble_cfg_with_outputs(snes, out, notesOut, instructionLimit, &control)) {
          fprintf(stderr, "Failed to build CFG for ROM '%s'\n", romPath);
          result = 1;
        }
      }
    } else if(!rom_disassemble(snes, out, instructionLimit)) {
      fprintf(stderr, "Failed to disassemble ROM '%s'\n", romPath);
      result = 1;
    }
  }

#ifndef _WIN32
  if(signalHandlersInstalled) {
    sigaction(SIGUSR1, &oldSigUsr1, NULL);
    sigaction(SIGUSR2, &oldSigUsr2, NULL);
  }
#endif

  if(out != NULL && out != stdout) {
    fclose(out);
  }
  if(notesOut != NULL) {
    fclose(notesOut);
  }

  snes_free(snes);
  free(file);
  return result;
}

static void handleCfgStopSignal(int signalNumber) {
  (void) signalNumber;
  g_cfgStopRequested = 1;
}

static void handleCfgStatusSignal(int signalNumber) {
  (void) signalNumber;
  g_cfgStatusRequested = 1;
}

static void printCfgProgress(void* userData, const RomDisassemblyProgress* progress) {
  (void) userData;
  if(progress == NULL) {
    return;
  }

  fprintf(
    stderr,
    "CFG status: nodes=%zu unique_instr=%zu funcs=%zu code_bytes=%zu coverage=%.4f%% edges=%zu processed=%zu queued=%zu unresolved_jumps=%zu unresolved_calls=%zu unresolved_returns=%zu recursive_cutoff=%zu mutual_cutoff=%zu depth_cutoff=%zu context_cutoff=%zu%s%s\n",
    progress->nodes,
    progress->uniqueInstructionAddresses,
    progress->discoveredFunctions,
    progress->reachableRomBytes,
    progress->romCoveragePercent,
    progress->edges,
    progress->processedNodes,
    progress->queuedNodes,
    progress->unresolvedIndirectJumps,
    progress->unresolvedIndirectCalls,
    progress->unresolvedReturns,
    progress->recursiveCallsCutOff,
    progress->mutualRecursiveCallsCutOff,
    progress->maxCallDepthCutOff,
    progress->contextLimitCutOff,
    progress->stopRequested ? " stop-requested" : "",
    progress->completed ? (progress->hitNodeLimit ? " stopped-at-limit" : (progress->stopRequested ? " stopped-by-signal" : " finished")) : ""
  );
}

static void closeRom() {
  if(!glb.loaded) return;
  if(glb.runtimeCfgPath != NULL) {
    if(traceRecorder_dumpRuntimeGraph(glb.traceRecorder, glb.runtimeCfgPath)) {
      printf("Saved runtime CFG to %s\n", glb.runtimeCfgPath);
    } else {
      printf("Failed to save runtime CFG to %s\n", glb.runtimeCfgPath);
    }
  }
  if(glb.runtimeNotesPath != NULL) {
    if(traceRecorder_dumpRuntimeNotes(glb.traceRecorder, glb.runtimeNotesPath)) {
      printf("Saved runtime notes to %s\n", glb.runtimeNotesPath);
    } else {
      printf("Failed to save runtime notes to %s\n", glb.runtimeNotesPath);
    }
  }
  if(glb.runtimeWramNotesPath != NULL) {
    if(traceRecorder_dumpRuntimeWramNotes(glb.traceRecorder, glb.runtimeWramNotesPath)) {
      printf("Saved runtime WRAM notes to %s\n", glb.runtimeWramNotesPath);
      if(dumpRuntimeWramBinary(glb.runtimeWramNotesPath)) {
        printf("Saved runtime WRAM binary to %s.bin\n", glb.runtimeWramNotesPath);
      } else {
        printf("Failed to save runtime WRAM binary to %s.bin\n", glb.runtimeWramNotesPath);
      }
    } else {
      printf("Failed to save runtime WRAM notes to %s\n", glb.runtimeWramNotesPath);
    }
  }
  saveTraceFile(false);
  traceRecorder_end(glb.traceRecorder);
  traceRecorder_setRuntimeGraphEnabled(glb.traceRecorder, false);
  traceRecorder_setRuntimeNotesEnabled(glb.traceRecorder, false);
  traceRecorder_setRuntimeWramNotesEnabled(glb.traceRecorder, false);
  traceRecorder_clear(glb.traceRecorder);
  int size = snes_saveBattery(glb.snes, NULL);
  if(size > 0) {
    uint8_t* saveData = (uint8_t*)malloc(size);
    snes_saveBattery(glb.snes, saveData);
    FILE* f = fopen(glb.savePath, "wb");
    if(f != NULL) {
      fwrite(saveData, size, 1, f);
      fclose(f);
      puts("Saved battery data");
    } else {
      puts("Failed to save battery data");
    }
    free(saveData);
  }
}

static bool dumpRuntimeWramBinary(const char* jsonPath) {
  if(jsonPath == NULL || glb.snes == NULL) {
    return false;
  }

  const size_t pathLength = strlen(jsonPath);
  char* binPath = (char*)malloc(pathLength + 5);
  if(binPath == NULL) {
    return false;
  }
  memcpy(binPath, jsonPath, pathLength + 1);
  memcpy(binPath + pathLength, ".bin", 5);

  FILE* f = fopen(binPath, "wb");
  free(binPath);
  if(f == NULL) {
    return false;
  }

  const bool ok = fwrite(glb.snes->ram, SNES_RAM_SIZE, 1, f) == 1;
  fclose(f);
  return ok;
}

static bool saveTraceFile(bool verbose) {
  if(glb.tracePath == NULL) {
    if(verbose) puts("Trace path is not available");
    return false;
  }
  if(traceRecorder_getRecordCount(glb.traceRecorder) == 0 && !traceRecorder_isRecording(glb.traceRecorder)) {
    return false;
  }
  if(traceRecorder_saveToFile(glb.traceRecorder, glb.tracePath)) {
    if(verbose) printf("Saved trace to %s\n", glb.tracePath);
    return true;
  }
  if(verbose) puts("Failed to save trace");
  return false;
}

static void beginTraceRecording(bool fromStartup) {
  if(!traceRecorder_begin(glb.traceRecorder)) {
    if(fromStartup) {
      puts("Failed to start trace recording from startup");
    } else {
      puts("Failed to start trace recording");
    }
    return;
  }

  if(!saveTraceFile(false)) {
    if(fromStartup) {
      puts("Failed to create initial trace file from startup");
    } else {
      puts("Failed to create initial trace file");
    }
    return;
  }

  if(glb.tracePath != NULL) {
    if(fromStartup) {
      printf("Started trace recording from startup to %s\n", glb.tracePath);
    } else {
      printf("Started trace recording to %s\n", glb.tracePath);
    }
  } else if(fromStartup) {
    puts("Started trace recording from startup");
  } else {
    puts("Started trace recording");
  }
}

static void setPaths(const char* path) {
  // get rom name
  if(glb.romName) free(glb.romName);
  const char* filename = strrchr(path, glb.pathSeparator[0]); // get last occurence of '/' or '\'
  if(filename == NULL) {
    filename = path;
  } else {
    filename += 1; // skip past '/' or '\' itself
  }
  glb.romName = (char*)malloc(strlen(filename) + 1); // +1 for '\0'
  strcpy(glb.romName, filename);
  // get extension length
  const char* extStart = strrchr(glb.romName, '.'); // last occurence of '.'
  int extLen = extStart == NULL ? 0 : strlen(extStart);
  // get save name
  if(glb.savePath) free(glb.savePath);
  glb.savePath = (char*)malloc(strlen(glb.prefPath) + strlen(glb.romName) + 11); // "saves/" (6) + ".srm" (4) + '\0'
  strcpy(glb.savePath, glb.prefPath);
  strcat(glb.savePath, "saves");
  strcat(glb.savePath, glb.pathSeparator);
  strncat(glb.savePath, glb.romName, strlen(glb.romName) - extLen); // cut off extension
  strcat(glb.savePath, ".srm");
  // get state name
  if(glb.statePath) free(glb.statePath);
  glb.statePath = (char*)malloc(strlen(glb.prefPath) + strlen(glb.romName) + 12); // "states/" (7) + ".lss" (4) + '\0'
  strcpy(glb.statePath, glb.prefPath);
  strcat(glb.statePath, "states");
  strcat(glb.statePath, glb.pathSeparator);
  strncat(glb.statePath, glb.romName, strlen(glb.romName) - extLen); // cut off extension
  strcat(glb.statePath, ".lss");
  // get trace file name
  if(glb.tracePath) free(glb.tracePath);
  glb.tracePath = (char*)malloc(strlen(glb.prefPath) + strlen(glb.romName) + 13); // "states/" (7) + ".ltrc" (5) + '\0'
  strcpy(glb.tracePath, glb.prefPath);
  strcat(glb.tracePath, "states");
  strcat(glb.tracePath, glb.pathSeparator);
  strncat(glb.tracePath, glb.romName, strlen(glb.romName) - extLen);
  strcat(glb.tracePath, ".ltrc");
  // get disassembly file name
  if(glb.traceDisassemblyPath) free(glb.traceDisassemblyPath);
  glb.traceDisassemblyPath = (char*)malloc(strlen(glb.prefPath) + strlen(glb.romName) + 12); // "states/" (7) + ".txt" (4) + '\0'
  strcpy(glb.traceDisassemblyPath, glb.prefPath);
  strcat(glb.traceDisassemblyPath, "states");
  strcat(glb.traceDisassemblyPath, glb.pathSeparator);
  strncat(glb.traceDisassemblyPath, glb.romName, strlen(glb.romName) - extLen);
  strcat(glb.traceDisassemblyPath, ".txt");
}

static void setTitle(const char* romName) {
  if(romName == NULL) {
    SDL_SetWindowTitle(glb.window, "LakeSnes");
    return;
  }
  char* title = (char*)malloc(strlen(romName) + 12); // "LakeSnes - " (11) + '\0'
  strcpy(title, "LakeSnes - ");
  strcat(title, romName);
  SDL_SetWindowTitle(glb.window, title);
  free(title);
}

static bool loadCoprocessorLibrary(const char* path) {
#ifdef _WIN32
  fprintf(stderr, "--cop-lib is not implemented on Windows\n");
  (void) path;
  return false;
#else
  glb.copLibHandle = dlopen(path, RTLD_NOW);
  if(glb.copLibHandle == NULL) {
    fprintf(stderr, "Failed to load coprocessor library '%s': %s\n", path, dlerror());
    return false;
  }

  void* symbol = dlsym(glb.copLibHandle, LAKESNES_COPROCESSOR_HOOK_SYMBOL);
  if(symbol == NULL) {
    fprintf(stderr, "Failed to find symbol '%s' in '%s': %s\n", LAKESNES_COPROCESSOR_HOOK_SYMBOL, path, dlerror());
    dlclose(glb.copLibHandle);
    glb.copLibHandle = NULL;
    return false;
  }

  cpu_setCoprocessorHook(glb.snes->cpu, reinterpret_cast<CpuCoprocessorHook>(symbol), NULL);

  void* registrationSymbol = dlsym(glb.copLibHandle, LAKESNES_MEMORY_ACCESS_CALLBACK_REGISTRATION_SYMBOL);
  if(registrationSymbol != NULL) {
    reinterpret_cast<LakesnesMemoryAccessCallbackRegistrationHook>(registrationSymbol)(
      glb.snes,
      registerMemoryAccessCallbackFromLibrary,
      NULL
    );
    printf("Registered memory access callbacks from %s\n", path);
  }
  printf("Loaded coprocessor library %s\n", path);
  return true;
#endif
}

static void unloadCoprocessorLibrary(void) {
#ifndef _WIN32
  if(glb.snes != NULL) {
    cpu_setCoprocessorHook(glb.snes->cpu, NULL, NULL);
    snes_clearMemoryAccessCallbacks(glb.snes);
  }
  if(glb.copLibHandle != NULL) {
    dlclose(glb.copLibHandle);
    glb.copLibHandle = NULL;
  }
#endif
}

static void registerMemoryAccessCallbackFromLibrary(
  void* userData,
  uint32_t adr,
  SnesMemoryAccessCallback callback,
  void* callbackUserData
) {
  (void) userData;
  if(glb.snes == NULL) {
    return;
  }
  snes_setMemoryAccessCallback(glb.snes, adr, callback, callbackUserData);
}

static uint8_t* readFile(const char* name, int* length) {
  FILE* f = fopen(name, "rb");
  if(f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  int size = ftell(f);
  rewind(f);
  uint8_t* buffer = (uint8_t*)malloc(size);
  if(fread(buffer, size, 1, f) != 1) {
    fclose(f);
    return NULL;
  }
  fclose(f);
  *length = size;
  return buffer;
}

static uint8_t* readRomImage(const char* path, int* length) {
  // zip library from https://github.com/kuba--/zip
  uint8_t* file = NULL;
  *length = 0;
  if(checkExtention(path, true)) {
    struct zip_t* zip = zip_open(path, 0, 'r');
    if(zip != NULL) {
      int entries = zip_total_entries(zip);
      for(int i = 0; i < entries; i++) {
        zip_entry_openbyindex(zip, i);
        const char* zipFilename = zip_entry_name(zip);
        if(checkExtention(zipFilename, false)) {
          printf("Read \"%s\" from zip\n", zipFilename);
          size_t size = 0;
          zip_entry_read(zip, (void**) &file, &size);
          *length = (int) size;
          break;
        }
        zip_entry_close(zip);
      }
      zip_close(zip);
    }
  } else {
    file = readFile(path, length);
  }
  return file;
}

#pragma once

// PS Vita pieces of the platform layer that src/linux calls into. Everything
// else the dedicated server needs from the OS comes through newlib, which on
// the Vita offers the same POSIX calls as Linux (docs/vita.md).

#if !defined(__vita__)
#error "vita/vita_platform.h is for the PS Vita build only"
#endif

#include <stddef.h>

// Folder that holds the game data (main/, zone/) and receives the console log
// and configs: ux0:data/KisakCOD. It is also the default fs_basepath.
const char *Sys_VitaDataPath();

// Runs first in main(): raises the CPU clock, starts the network, makes the
// data folder the working directory, opens the console log and replaces the
// (empty) Vita argument list with the one in cmdline.txt. False when the
// server cannot run (no data folder, no network library); the reason is in the
// log.
bool Vita_Startup(int *argc, char ***argv);

// True when the player holds START + SELECT: the server then quits as if
// "quit" had been typed. Also keeps the console from sleeping while it serves.
bool Vita_QuitRequested();

// Appends console text to console.log in the data folder.
void Vita_ConsoleWrite(const char *text, size_t length);

// Sets the calling thread's float mode to IEEE (no flush-to-zero, no default
// NaN), as on x86 and on Linux armv7. Returns the FPSCR value found.
unsigned int Vita_SetIeeeFloatMode();

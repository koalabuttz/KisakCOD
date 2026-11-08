#pragma once

// PS Vita pieces of the platform layer that src/linux calls into. Everything
// else the dedicated server needs from the OS comes through newlib, which on
// the Vita offers the same POSIX calls as Linux (docs/vita.md).

#if !defined(__vita__)
#error "vita/vita_platform.h is for the PS Vita build only"
#endif

#include <stddef.h>
#include <stdint.h>

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

// Appends console text to console.log in the data folder. Each line starts
// with the seconds since start-up, so the log times the map load.
void Vita_ConsoleWrite(const char *text, size_t length);

// Sets the calling thread's float mode to IEEE (no flush-to-zero, no default
// NaN), as on x86 and on Linux armv7. Returns the FPSCR value found.
unsigned int Vita_SetIeeeFloatMode();

// Load and memory figures for the first runs (src/vita/vita_stats.cpp), so
// console.log answers how many players the Vita can serve.
//
// Vita_StatsInit runs once after Com_Init: it logs the memory left after the
// engine's reservations and registers the "vitastats" command. Vita_FrameBegin
// and Vita_FrameEnd wrap each Com_Frame in the main loop; NET_Sleep reports
// the time it spent idle through Vita_AddIdleTime. Once a minute the log gets
// one line: how busy the server was, its slowest frame, and memory use.
void Vita_StatsInit();
void Vita_FrameBegin();
void Vita_FrameEnd();
void Vita_AddIdleTime(uint64_t microseconds);

// Logs memory use straight to console.log, for Sys_Error: an out-of-memory
// error then shows which pool ran out.
void Vita_LogMemoryAtError();

// Size of newlib's heap block in MB (_newlib_heap_size_user).
int Vita_HeapSizeMB();

// Ends the process at once. newlib's exit() and _exit() free the heap block
// before the kernel stops the other threads, and a worker still running in that
// gap faults on freed memory (Vita3K shows it at every fatal error). The
// kernel reclaims everything anyway, so the server's exits skip that clean-up.
[[noreturn]] void Vita_Exit(int status);

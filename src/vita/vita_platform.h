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
// the time it spent idle through Vita_AddIdleTime. Once a minute (every
// vita_statsEvery seconds) the log gets one line: how busy the server was, its
// slowest frame, and memory use.
void Vita_StatsInit();
void Vita_FrameBegin();
void Vita_FrameEnd();
void Vita_AddIdleTime(uint64_t microseconds);

// True once, when a run that nobody watches has reached its end
// (vita_run_plan.h): "+set vita_quitAfter 90" in the command line file quits 90
// seconds after start-up, "+set vita_quitAfterMenu 15" 15 seconds after the main
// menu first showed and "+set vita_quitAfterMap 30" 30 seconds after the map was
// first in play. Each prints every figure first.
bool Vita_QuitAfterDue();

// Goes up by one each time every figure is asked for: every vita_statsEvery
// seconds (60 by default), before a timed quit, and by the "vitastats"
// command. The renderer and the sound thread print their own lines when they
// see it change, so all figures share one period.
unsigned int Vita_StatsGeneration();

// "+set vita_assertExit 1": a failed assertion or an exception nothing caught
// syncs the log and ends the process with exit code 1 where it would stop for
// a core dump. An emulator hangs on that stop and loses the log's tail; a
// console's dump is worth keeping, so it is off by default (the log is synced
// and says what was thrown either way). The start-up reads it from the command
// line file (an assertion can fail before any dvar exists); afterwards it
// follows the dvar.
bool Vita_AssertExits();
void Vita_SetAssertExits(bool exits);

// Has the system write console.log's bytes out.
void Vita_ConsoleSync();

// Logs memory use straight to console.log and to standard output, for
// Sys_Error: an out-of-memory error then shows which pool ran out.
void Vita_LogMemoryAtError();

// Size of newlib's heap block in MB (_newlib_heap_size_user).
int Vita_HeapSizeMB();

// Closes console.log and ends the process at once (no line is logged after
// it). newlib's exit() and _exit() free the heap block
// before the kernel stops the other threads, and a worker still running in that
// gap faults on freed memory (Vita3K shows it at every fatal error). The
// kernel reclaims everything anyway, so the server's exits skip that clean-up.
[[noreturn]] void Vita_Exit(int status);

// The network library, started once: the bench needs it before the log is
// open, the engine after. False when it could not be started; the start-up
// logs why.
bool Vita_StartNetwork();

// The bench that fetches its work (vita_bench.h; the multiplayer client
// only, and only with bench.txt on the card). Vita_BenchBegin runs first at
// start-up, before the list of starts, which it may have just fetched; it
// does not return when the job at hand runs on the other build. False: the
// bench started this process and has nothing left to do, so it ends.
// Vita_BenchNote is what it did, for the log. Vita_BenchEnd is called at the
// process's end while a job runs (Vita_BenchActive): it starts the list's
// next start, or hands a finished list's logs to the installed build to
// send, and returns when the process should simply end.
bool Vita_BenchBegin(int argc, char **argv);
const char *Vita_BenchNote();
bool Vita_BenchActive();
void Vita_BenchEnd(bool listHasMore, bool stopped);

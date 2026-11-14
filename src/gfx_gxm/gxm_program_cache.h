#pragma once

// Compiled GXM programs kept on the memory card, so each Cg program is
// compiled once per console (the Vita's compiler takes about 0.1 s per
// program, and a map uses hundreds).
//
// A program is found by a hash of its Cg source and its stage, in the
// folder's pack when there is one (gxm_program_pack.h), else in one file
// per program: <folder>/<16 hex digits>.gxp. Changing the translator
// changes the source, so stale programs are never picked up; they only take
// space. A file that fails sceGxmProgramCheck is compiled again.
//
// Any thread may call these. The cache itself (card and compiler) is used by
// one thread at a time: GetProgram waits for its turn, which can be a whole
// compile. RequestProgram never waits: it hands the work to the cache's own
// thread (gxm_program_queue.h), which is how the game's shaders get theirs.

#include "gxm_program_queue.h"
#include "gxm_shader_compiler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gfx_gxm
{
// Null or empty: no files, every program is compiled.
void SetProgramCacheFolder(const char *folder);

struct ProgramCacheStats
{
    uint32_t compiled;
    uint32_t fromFiles;
    uint32_t failed;
    uint32_t writeFailures;
    uint64_t compileMicros;
    uint64_t readMicros;
    // Times the compiler was unloaded to give its memory back, and the heap
    // bytes that gave back.
    uint32_t compilerUnloads;
    uint64_t compilerGaveBack;
    // The slowest single compile and read from the card.
    uint64_t slowestCompileMicros;
    uint64_t slowestReadMicros;
    // Programs asked for with RequestProgram: how many, how many of them a
    // draw hurried, how many were dropped unmade, and the most that waited
    // at once.
    uint64_t requested;
    uint64_t hurried;
    uint64_t dropped;
    uint32_t mostWaiting;
    // The folder's pack (gxm_program_pack.h): the programs it holds, how
    // many of fromFiles came out of it, and whether a pack was there that
    // could not be read.
    uint32_t packPrograms;
    uint32_t fromPack;
    bool packRefused;
};

// The program for this source, from its file or compiled now (and then
// written). False with the compiler's log (or why the compiler is missing).
// fastMath and optimization are CompileCg's: a caller that changes them
// gives the source a different text (a comment will do), since the file is
// found by the source alone.
bool GetProgram(const std::string &cg, ShaderStage stage, std::vector<uint8_t> &program, std::string &log,
                bool fastMath = true, int optimization = 3);
// The same program without waiting for it: read or compiled on the cache's
// thread, in the order asked. The request says when it is ready
// (ProgramRequest::state); label names it in the log line of a failure.
// Dropping the request before its turn drops the work.
ProgramRequestRef RequestProgram(std::string cg, ShaderStage stage, std::string label);
// A draw wants this request's program: it is made before the others.
void HurryProgram(const ProgramRequestRef &request);
// Requested programs not made yet.
size_t ProgramsWaiting();
// Requests that failed since the last call, for the log.
void TakeProgramFailures(std::vector<ProgramRequestRef> &failures);
// Once a frame: has the compiler unloaded when it has been idle for a while,
// so that it holds no memory while the game runs. It is loaded again when a
// program has to be compiled. Never waits for a compile.
void ProgramCacheFrame();
// Whether the folder holds a file of this name: switches for runs on a
// console or an emulator ("dump-cg" below, the draw tests' "run-tests").
bool ProgramCacheMarker(const char *name);
// The 16 hex digits of this source's file name, for logs.
uint64_t ProgramCacheKey(const std::string &cg, ShaderStage stage);
//
// For finding out what a program does on a console or an emulator: when the
// folder holds a file named "dump-cg", every program asked for also leaves
// its Cg source there as <16 hex digits>.cg. The sources come from the
// game's shaders, so they stay on that machine like the game's files.
ProgramCacheStats GetProgramCacheStats();
} // namespace gfx_gxm

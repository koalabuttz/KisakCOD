#pragma once

// Compiled GXM programs kept on the memory card, so each Cg program is
// compiled once per console (the Vita's compiler takes about 0.1 s per
// program, and a map uses hundreds).
//
// A program is found by a hash of its Cg source and its stage, in one file
// per program: <folder>/<16 hex digits>.gxp. Changing the translator
// changes the source, so stale programs are never picked up; they only take
// space. A file that fails sceGxmProgramCheck is compiled again.
//
// One thread at a time.

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
};

// The program for this source, from its file or compiled now (and then
// written). False with the compiler's log (or why the compiler is missing).
bool GetProgram(const std::string &cg, ShaderStage stage, std::vector<uint8_t> &program, std::string &log);
const ProgramCacheStats &GetProgramCacheStats();
} // namespace gfx_gxm

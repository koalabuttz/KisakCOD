#pragma once

// Cg to GXM programs on the PS Vita itself, through the system's shader
// compiler, libshacccg.suprx.
//
// The compiler is part of Sony's PSM runtime and cannot be shipped with the
// game: each user extracts it once from their own console (ShaRKBR33D puts it
// in ur0:data/libshacccg.suprx). The client loads it when it starts and links
// to it through weak imports (SceShaccCg_stub_weak), so the program still
// starts without it and can say what is missing. ur0: is closed to "safe"
// homebrew, so a program that loads the compiler is packed unsafe
// (kisak_vita_pack_vpk ... UNSAFE).
//
// Compiling is slow, so the client is meant to keep compiled programs in a
// cache file and compile each shader once per console (stage 2 of
// docs/vita-client.md). One thread at a time compiles.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gfx_gxm
{
// Where LoadShaderCompiler looks, in order.
extern const char *const kShaderCompilerPaths[];

// False with the reason when no compiler can be loaded. message names the
// file loaded and the compiler's version otherwise.
bool LoadShaderCompiler(std::string &message);
bool ShaderCompilerLoaded();
// Frees the compiler's working memory (it stays loaded).
void ReleaseShaderCompiler();

enum class ShaderStage : uint8_t
{
    Vertex,
    Fragment,
};

// Compiles Cg source with entry point main into a GXM program (what a .gxp
// file holds), checked with sceGxmProgramCheck. log receives the compiler's
// diagnostics, one per line, errors and warnings alike.
bool CompileCg(const char *source, size_t length, ShaderStage stage, std::vector<uint8_t> &program,
               std::string &log);
} // namespace gfx_gxm

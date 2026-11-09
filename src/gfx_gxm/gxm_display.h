#pragma once

// The PS Vita's screen through sceGxm: the library, one immediate context, the
// display buffers the screen shows in turn, a depth-stencil surface, the
// render target that scenes on them use, and the shader patcher that turns
// compiled programs into vertex and fragment programs.
//
// A frame is BeginScene ... draws ... EndScene, as often as the frame needs,
// then Present: the display queue shows the back buffer at the next vertical
// blank (or at once with vsync off) and the next buffer becomes the back
// buffer. The queue holds kDisplayBuffers - 1 frames, so the CPU runs at most
// that far ahead of the screen.
//
// One thread calls everything. See docs/vita-client.md.

#include "gxm_memory.h"

#include <psp2/gxm.h>

#include <cstdint>
#include <string>

namespace gfx_gxm
{
constexpr uint32_t kDisplayWidth = 960;
constexpr uint32_t kDisplayHeight = 544;
constexpr uint32_t kDisplayStride = 960; // pixels; the display wants a multiple of 64
constexpr uint32_t kDisplayBuffers = 3;

struct DisplayDesc
{
    // The parameter buffer the GPU bins geometry into, from main memory.
    // sceGxm needs at least 256 KB; a frame whose geometry overflows it is
    // split into partial renders, which costs time, not correctness.
    uint32_t parameterBufferBytes = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    // The GPU memory pools (gxm_memory.h). The display's own buffers come out
    // of them: the display buffers and depth-stencil about 9 MB of CDRAM,
    // the context's ring buffers about 2.6 MB of main memory and 16 KB of
    // fragment USSE.
    MemorySizes memory;
    bool vsync = true;
    // One line per call, no newline. Null: nowhere.
    void (*log)(void *user, const char *line) = nullptr;
    void *logUser = nullptr;
};

// False with the reason; whatever was set up is torn down again.
bool InitDisplay(const DisplayDesc &desc, std::string &error);
void ShutdownDisplay();
bool DisplayReady();

SceGxmContext *Context();
SceGxmShaderPatcher *ShaderPatcher();

// The buffer the frame draws into, and the one queued for display last.
void *BackBufferData();
void *FrontBufferData();
const SceGxmColorSurface *BackBufferSurface();
// The sync object a scene drawing into the back buffer passes as its
// fragment sync object, so the display waits for it.
SceGxmSyncObject *BackBufferSync();
const SceGxmDepthStencilSurface *DepthStencilSurface();

// A render target for scenes of this size, made on first use and kept until
// ShutdownDisplay (sceGxm wants one per surface size). Null on failure,
// logged.
SceGxmRenderTarget *RenderTargetFor(uint32_t width, uint32_t height);

// Scenes on the back buffer and the depth-stencil surface.
bool BeginScene();
bool EndScene();
bool InScene();

// Ends an open scene, queues the back buffer for display and moves on to the
// next buffer.
bool Present();
// Waits until the GPU has finished everything submitted.
void Finish();

struct DisplayStats
{
    uint64_t frames;
    uint64_t scenes;
};
DisplayStats GetDisplayStats();
} // namespace gfx_gxm

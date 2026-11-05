#pragma once

// Linux stand-in for deps/binklib/binktextures.h, the RAD sample code the
// engine's cinematic player (src/gfx_d3d/r_cinematic.cpp) decodes into.
//
// The engine uses two things from it: the BINKTEXTURESET layout (a set of
// system-memory textures Bink decodes into, plus the draw textures the
// cinematic material samples) and Lock_Bink_textures/Unlock_Bink_textures
// around BinkDoFrame. The RAD file also builds its own YUV shaders with D3DX,
// which the engine never uses (its cinematic material does the conversion), so
// only the layout and the two lock functions are kept, with the same names.
// The functions are in bink_textures.cpp, over the native D3D9 boundary.
// The D3D9 types first (the boundary refuses any other order); with
// KISAK_LINUX_NATIVE_D3D9 this is DXVK Native's d3d9.h.
#include <linux/d3d9_types.h>

#include <binklib/bink.h>

typedef struct BINKFRAMETEXTURES
{
    U32 Ysize;
    U32 cRsize;
    U32 cBsize;
    U32 Asize;
    LPDIRECT3DTEXTURE9 Ytexture;
    LPDIRECT3DTEXTURE9 cRtexture;
    LPDIRECT3DTEXTURE9 cBtexture;
    LPDIRECT3DTEXTURE9 Atexture;
} BINKFRAMETEXTURES;

typedef struct BINKTEXTURESET
{
    // What Bink decodes into.
    BINKFRAMEBUFFERS bink_buffers;
    // The system-memory textures behind bink_buffers, one set per frame buffer.
    BINKFRAMETEXTURES textures[BINKMAXFRAMEBUFFERS];
    // The textures drawn with; each new frame is copied into them
    // (USE_SEPARATE_BLIT_TEXTURE in the RAD code).
    BINKFRAMETEXTURES tex_draw;
} BINKTEXTURESET;

// Locks every frame buffer's textures and points bink_buffers at their pixels,
// so that BinkDoFrame can decode into them.
void Lock_Bink_textures(BINKTEXTURESET *set_textures);
// Unlocks them, and copies the frame BinkDoFrame just decoded into tex_draw.
void Unlock_Bink_textures(LPDIRECT3DDEVICE9 d3d_device, BINKTEXTURESET *set_textures, HBINK Bink);

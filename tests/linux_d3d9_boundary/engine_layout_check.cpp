// engine_layout_check.cpp -- the engine structures that hold D3D9 types keep one
// layout, whichever D3D9 view a translation unit has.
//
// Compiled twice by scripts/tests/run_native_d3d9_boundary_checks.py, with the
// same compiler and flags apart from one define:
//
//   * stand-in mode: <linux/d3d9_types.h> declares opaque interface tags and the
//     D3DFORMAT-family enums itself (what the dedicated server and every
//     non-renderer translation unit sees);
//   * native mode:   -DKISAK_LINUX_NATIVE_D3D9 and the DXVK Native 2.7.1 include
//     directory, so the same headers resolve to DXVK's real declarations through
//     src/linux/d3d9_native.h.
//
// Each build checks the documented x86-32 sizes (the `// sizeof=` comments taken
// from the original Windows binary) at compile time, then prints one line per
// size/offset. The runner requires both builds to print byte-identical output, so
// a DXVK type that changed any engine layout fails even where no documented
// constant exists for the field.
//
// Asset-free: it reads nothing, writes only to stdout, and calls no D3D9 function.
// It is not linked against DXVK; including declarations needs no library.

#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_sky.h>
#include <gfx_d3d/rb_backend.h>

#include <cstddef>
#include <cstdio>

#if defined(KISAK_LINUX_NATIVE_D3D9)
#define KISAK_LAYOUT_MODE "native"
#else
#define KISAK_LAYOUT_MODE "stand-in"
#endif

static_assert(sizeof(void *) == 4, "the layout check describes the 32-bit engine: build with -m32");

// Documented sizes of every engine structure that embeds a D3D9 type directly,
// plus the asset and device containers built from them. Values are the original
// x86-32 sizes; KISAK_RADIANT changes DxGlobals and is not built here.
#define KISAK_D3D9_ENGINE_SIZES(X)                    \
    X(GfxTexture, 0x4)                                \
    X(GfxImageLoadDef, 0x14)                          \
    X(GfxImage, 0x24)                                 \
    X(GfxVertexBufferState, 0x10)                     \
    X(GfxIndexBufferState, 0x10)                      \
    X(GfxWorldVertexData, 0x8)                        \
    X(GfxWorldVertexLayerData, 0x8)                   \
    X(GfxBuffers, 0x2400A0)                           \
    X(GfxAssets, 0x4)                                 \
    X(GfxMetrics, 0x28)                               \
    X(GfxWindowTarget, 0x10)                          \
    X(DxGlobals, 0x2CE0)                              \
    X(MaterialVertexStreamRouting, 0x60)              \
    X(MaterialVertexDeclaration, 0x64)                \
    X(MaterialVertexShaderProgram, 0xC)               \
    X(MaterialPixelShaderProgram, 0xC)                \
    X(GfxRenderTargetSurface, 0x8)                    \
    X(GfxRenderTarget, 0x14)                          \
    X(SunFlareDynamic, 0x28)                          \
    X(GfxCmdBufPrimState_stream, 0xC)                 \
    X(GfxCmdBufPrimState, 0x28)                       \
    X(GfxCmdBufState, 0xA10)                          \
    X(GfxCmdBuf, 0x4)                                 \
    X(D3DDISPLAYMODE, 0x10)

// Offsets of the D3D9-typed fields themselves, and of the fields that follow a
// D3D9-typed member, where a size change would first show.
#define KISAK_D3D9_ENGINE_OFFSETS(X)                  \
    X(GfxImageLoadDef, format, 0x8)                   \
    X(GfxImageLoadDef, resourceSize, 0xC)             \
    X(GfxImage, texture, 0x4)                         \
    X(GfxImage, picmip, 0x8)                          \
    X(GfxImage, name, 0x20)                           \
    X(GfxVertexBufferState, buffer, 0x8)              \
    X(GfxVertexBufferState, verts, 0xC)               \
    X(GfxIndexBufferState, buffer, 0x8)               \
    X(GfxWindowTarget, hwnd, 0x0)                     \
    X(GfxWindowTarget, swapChain, 0x4)                \
    X(GfxWindowTarget, width, 0x8)                    \
    X(DxGlobals, d3d9, 0x4)                           \
    X(DxGlobals, device, 0x8)                         \
    X(DxGlobals, depthStencilFormat, 0x24)            \
    X(DxGlobals, displayModeCount, 0x28)              \
    X(DxGlobals, displayModes, 0x2C)                  \
    X(DxGlobals, resolutionNameTable, 0x102C)         \
    X(MaterialVertexStreamRouting, decl, 0x20)        \
    X(MaterialVertexDeclaration, routing, 0x4)        \
    X(MaterialVertexShaderProgram, loadDef, 0x4)      \
    X(MaterialPixelShaderProgram, loadDef, 0x4)       \
    X(GfxRenderTargetSurface, depthStencil, 0x4)      \
    X(GfxRenderTarget, surface, 0x4)                  \
    X(GfxRenderTarget, width, 0xC)                    \
    X(GfxCmdBufPrimState_stream, vb, 0x4)             \
    X(GfxCmdBufPrimState, indexBuffer, 0x4)           \
    X(GfxCmdBufPrimState, streams, 0xC)               \
    X(GfxCmdBufPrimState, vertexDecl, 0x24)           \
    X(D3DDISPLAYMODE, Format, 0xC)

#define KISAK_ASSERT_SIZE(type, size) \
    static_assert(sizeof(type) == (size), #type " must keep its documented x86-32 size");
#define KISAK_ASSERT_OFFSET(type, field, offset) \
    static_assert(offsetof(type, field) == (offset), #type "." #field " must keep its documented x86-32 offset");

KISAK_D3D9_ENGINE_SIZES(KISAK_ASSERT_SIZE)
KISAK_D3D9_ENGINE_OFFSETS(KISAK_ASSERT_OFFSET)

int main()
{
    std::printf("# D3D9-bearing engine layouts (%s view)\n", KISAK_LAYOUT_MODE);
    // The mode line above is the only line allowed to differ between the builds;
    // the runner compares everything after it.
#define KISAK_PRINT_SIZE(type, size) \
    std::printf("size %s %u align %u\n", #type, unsigned(sizeof(type)), unsigned(alignof(type)));
#define KISAK_PRINT_OFFSET(type, field, offset) \
    std::printf("offset %s.%s %u\n", #type, #field, unsigned(offsetof(type, field)));
    KISAK_D3D9_ENGINE_SIZES(KISAK_PRINT_SIZE)
    KISAK_D3D9_ENGINE_OFFSETS(KISAK_PRINT_OFFSET)
    // Enumerator values the stand-in and DXVK must agree on; d3d9_native.h already
    // static_asserts these, and printing them makes the two builds comparable.
#define KISAK_PRINT_ENUMERATOR(name, value) \
    std::printf("enum %s %lld\n", #name, static_cast<long long>(name));
    KISAK_D3D9_FORMAT_VALUES(KISAK_PRINT_ENUMERATOR)
    KISAK_D3D9_MULTISAMPLE_VALUES(KISAK_PRINT_ENUMERATOR)
    KISAK_D3D9_TEXTUREFILTER_VALUES(KISAK_PRINT_ENUMERATOR)
    KISAK_D3D9_CUBEMAP_FACE_VALUES(KISAK_PRINT_ENUMERATOR)
    std::printf("PASS engine D3D9 layouts\n");
    return 0;
}

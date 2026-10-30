// d3d9_types.h -- the D3D9 public type surface that the engine's headers name,
// declared for a native Linux build that has no D3D9 SDK.
//
// What this is for: the gfx_d3d headers are part of the shared include web even
// in a dedicated build (qcommon/threads.h pulls in gfx_d3d/rb_backend.h for
// THREAD_CONTEXT_COUNT, database/database.h and client headers pull in the
// rest). Those headers only ever hold D3D9 *types*: opaque interface pointers,
// plus D3DFORMAT/D3DDISPLAYMODE fields whose sizes are part of GfxImage /
// DxGlobals layouts.
//
// What this is NOT: a D3D9 implementation. There are no interface method
// declarations, no COM vtables and no functions of any kind, so nothing here
// can be called and no call can quietly succeed. A dedicated build constructs
// no device: the POSIX/dedicated renderer-absent path simply never instantiates
// these types. When the renderer is built, the real d3d9.h must be used instead
// of this header (see the __linux__ guards in gfx_d3d/*.h).
//
// Values for the enums are the D3D9 public values from d3d9types.h, and the
// layouts asserted at the bottom are the x86-32 ones, because GfxImageLoadDef
// and DxGlobals embed them by value.

#pragma once

#ifndef __linux__
#error "src/linux/d3d9_types.h is the stand-in for <d3d9.h> on native Linux only"
#endif

#include <linux/compat.h>

#include <cstdint>

// D3D9 marks its COM interfaces as `struct IDirect3DDevice9 : public IUnknown`.
// The engine only ever stores pointers to them, so declaring the tags is the
// accurate representation; defining vtables would be inventing an ABI.
struct IDirect3D9;
struct IDirect3DDevice9;
struct IDirect3DSwapChain9;
struct IDirect3DBaseTexture9;
struct IDirect3DTexture9;
struct IDirect3DCubeTexture9;
struct IDirect3DVolumeTexture9;
struct IDirect3DSurface9;
struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;
struct IDirect3DVertexDeclaration9;
struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;
struct IDirect3DQuery9;
struct IDirect3DStateBlock9;

// The pointer typedefs are part of the public D3D9 surface; declare them so
// code written against d3d9.h keeps compiling unchanged.
typedef struct IDirect3D9 *LPDIRECT3D9, *PDIRECT3D9;
typedef struct IDirect3DDevice9 *LPDIRECT3DDEVICE9, *PDIRECT3DDEVICE9;
typedef struct IDirect3DSwapChain9 *LPDIRECT3DSWAPCHAIN9, *PDIRECT3DSWAPCHAIN9;
typedef struct IDirect3DBaseTexture9 *LPDIRECT3DBASETEXTURE9, *PDIRECT3DBASETEXTURE9;
typedef struct IDirect3DTexture9 *LPDIRECT3DTEXTURE9, *PDIRECT3DTEXTURE9;
typedef struct IDirect3DCubeTexture9 *LPDIRECT3DCUBETEXTURE9, *PDIRECT3DCUBETEXTURE9;
typedef struct IDirect3DVolumeTexture9 *LPDIRECT3DVOLUMETEXTURE9, *PDIRECT3DVOLUMETEXTURE9;
typedef struct IDirect3DSurface9 *LPDIRECT3DSURFACE9, *PDIRECT3DSURFACE9;
typedef struct IDirect3DVertexBuffer9 *LPDIRECT3DVERTEXBUFFER9, *PDIRECT3DVERTEXBUFFER9;
typedef struct IDirect3DIndexBuffer9 *LPDIRECT3DINDEXBUFFER9, *PDIRECT3DINDEXBUFFER9;
typedef struct IDirect3DVertexDeclaration9 *LPDIRECT3DVERTEXDECLARATION9, *PDIRECT3DVERTEXDECLARATION9;
typedef struct IDirect3DVertexShader9 *LPDIRECT3DVERTEXSHADER9, *PDIRECT3DVERTEXSHADER9;
typedef struct IDirect3DPixelShader9 *LPDIRECT3DPIXELSHADER9, *PDIRECT3DPIXELSHADER9;
typedef struct IDirect3DQuery9 *LPDIRECT3DQUERY9, *PDIRECT3DQUERY9;
typedef struct IDirect3DStateBlock9 *LPDIRECT3DSTATEBLOCK9, *PDIRECT3DSTATEBLOCK9;

// Four-character codes, as in d3d9types.h.
#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3)                    \
    ((std::uint32_t)(std::uint8_t)(ch0) |                 \
     ((std::uint32_t)(std::uint8_t)(ch1) << 8) |          \
     ((std::uint32_t)(std::uint8_t)(ch2) << 16) |         \
     ((std::uint32_t)(std::uint8_t)(ch3) << 24))
#endif

// D3DFORMAT. The engine embeds this by value (GfxImageLoadDef::format,
// DxGlobals::depthStencilFormat), so it must stay a 4-byte unscoped enum, which
// it does because every enumerator below fits in an int.
enum _D3DFORMAT
{
    D3DFMT_UNKNOWN = 0,

    // Uncompressed colour/indexed formats.
    D3DFMT_R8G8B8 = 20,
    D3DFMT_A8R8G8B8 = 21,
    D3DFMT_X8R8G8B8 = 22,
    D3DFMT_R5G6B5 = 23,
    D3DFMT_X1R5G5B5 = 24,
    D3DFMT_A1R5G5B5 = 25,
    D3DFMT_A4R4G4B4 = 26,
    D3DFMT_R3G3B2 = 27,
    D3DFMT_A8 = 28,
    D3DFMT_A8R3G3B2 = 29,
    D3DFMT_X4R4G4B4 = 30,
    D3DFMT_A2B10G10R10 = 31,
    D3DFMT_A8B8G8R8 = 32,
    D3DFMT_X8B8G8R8 = 33,
    D3DFMT_G16R16 = 34,
    D3DFMT_A2R10G10B10 = 35,
    D3DFMT_A16B16G16R16 = 36,

    // Palette formats.
    D3DFMT_A8P8 = 40,
    D3DFMT_P8 = 41,

    // Luminance and bump formats.
    D3DFMT_L8 = 50,
    D3DFMT_A8L8 = 51,
    D3DFMT_A4L4 = 52,
    D3DFMT_V8U8 = 60,
    D3DFMT_L6V5U5 = 61,
    D3DFMT_X8L8V8U8 = 62,
    D3DFMT_Q8W8V8U8 = 63,
    D3DFMT_V16U16 = 64,
    D3DFMT_A2W10V10U10 = 67,

    // FourCC formats: the block-compressed texture set the engine's image
    // loader names.
    D3DFMT_UYVY = MAKEFOURCC('U', 'Y', 'V', 'Y'),
    D3DFMT_R8G8_B8G8 = MAKEFOURCC('R', 'G', 'B', 'G'),
    D3DFMT_YUY2 = MAKEFOURCC('Y', 'U', 'Y', '2'),
    D3DFMT_G8R8_G8B8 = MAKEFOURCC('G', 'R', 'G', 'B'),
    D3DFMT_DXT1 = MAKEFOURCC('D', 'X', 'T', '1'),
    D3DFMT_DXT2 = MAKEFOURCC('D', 'X', 'T', '2'),
    D3DFMT_DXT3 = MAKEFOURCC('D', 'X', 'T', '3'),
    D3DFMT_DXT4 = MAKEFOURCC('D', 'X', 'T', '4'),
    D3DFMT_DXT5 = MAKEFOURCC('D', 'X', 'T', '5'),

    // Depth/stencil and the remaining render-target formats.
    D3DFMT_D16_LOCKABLE = 70,
    D3DFMT_D32 = 71,
    D3DFMT_D15S1 = 73,
    D3DFMT_D24S8 = 75,
    D3DFMT_D24X8 = 77,
    D3DFMT_D24X4S4 = 79,
    D3DFMT_D16 = 80,
    D3DFMT_L16 = 81,
    D3DFMT_D32F_LOCKABLE = 82,
    D3DFMT_D24FS8 = 83,

    // Vertex buffer and index buffer formats.
    D3DFMT_VERTEXDATA = 100,
    D3DFMT_INDEX16 = 101,
    D3DFMT_INDEX32 = 102,

    // Floating-point formats.
    D3DFMT_Q16W16V16U16 = 110,
    D3DFMT_R16F = 111,
    D3DFMT_G16R16F = 112,
    D3DFMT_A16B16G16R16F = 113,
    D3DFMT_R32F = 114,
    D3DFMT_G32R32F = 115,
    D3DFMT_A32B32G32R32F = 116,
    D3DFMT_CxV8U8 = 117,
};
typedef enum _D3DFORMAT D3DFORMAT;

enum _D3DMULTISAMPLE_TYPE
{
    D3DMULTISAMPLE_NONE = 0,
    D3DMULTISAMPLE_NONMASKABLE = 1,
    D3DMULTISAMPLE_2_SAMPLES = 2,
    D3DMULTISAMPLE_3_SAMPLES = 3,
    D3DMULTISAMPLE_4_SAMPLES = 4,
    D3DMULTISAMPLE_5_SAMPLES = 5,
    D3DMULTISAMPLE_6_SAMPLES = 6,
    D3DMULTISAMPLE_7_SAMPLES = 7,
    D3DMULTISAMPLE_8_SAMPLES = 8,
    D3DMULTISAMPLE_9_SAMPLES = 9,
    D3DMULTISAMPLE_10_SAMPLES = 10,
    D3DMULTISAMPLE_11_SAMPLES = 11,
    D3DMULTISAMPLE_12_SAMPLES = 12,
    D3DMULTISAMPLE_13_SAMPLES = 13,
    D3DMULTISAMPLE_14_SAMPLES = 14,
    D3DMULTISAMPLE_15_SAMPLES = 15,
    D3DMULTISAMPLE_16_SAMPLES = 16,
};
typedef enum _D3DMULTISAMPLE_TYPE D3DMULTISAMPLE_TYPE;

enum _D3DTEXTUREFILTERTYPE
{
    D3DTEXF_NONE = 0,
    D3DTEXF_POINT = 1,
    D3DTEXF_LINEAR = 2,
    D3DTEXF_ANISOTROPIC = 3,
    D3DTEXF_PYRAMIDALQUAD = 6,
    D3DTEXF_GAUSSIANQUAD = 7,
};
typedef enum _D3DTEXTUREFILTERTYPE D3DTEXTUREFILTERTYPE;

enum _D3DCUBEMAP_FACES
{
    D3DCUBEMAP_FACE_POSITIVE_X = 0,
    D3DCUBEMAP_FACE_NEGATIVE_X = 1,
    D3DCUBEMAP_FACE_POSITIVE_Y = 2,
    D3DCUBEMAP_FACE_NEGATIVE_Y = 3,
    D3DCUBEMAP_FACE_POSITIVE_Z = 4,
    D3DCUBEMAP_FACE_NEGATIVE_Z = 5,
};
typedef enum _D3DCUBEMAP_FACES D3DCUBEMAP_FACES;

struct _D3DDISPLAYMODE
{
    UINT Width;                 // 0x00
    UINT Height;                // 0x04
    UINT RefreshRate;           // 0x08
    D3DFORMAT Format;           // 0x0C
};
typedef struct _D3DDISPLAYMODE D3DDISPLAYMODE;

// Both of these are only ever used through a pointer by the engine headers
// (r_init.h's R_SetD3DPresentParameters / R_CreateDeviceInternal /
// R_GetDirect3DCaps), so an incomplete type is the accurate declaration: giving
// them a guessed body would be inventing fields D3D9 does not guarantee.
struct _D3DPRESENT_PARAMETERS_;
typedef struct _D3DPRESENT_PARAMETERS_ D3DPRESENT_PARAMETERS;
struct _D3DCAPS9;
typedef struct _D3DCAPS9 D3DCAPS9;

// Layout assertions for what the engine embeds by value. DxGlobals' documented
// size comment (sizeof=0x2CE0) depends on displayModes[256] being 256 * 16 bytes.
static_assert(sizeof(_D3DFORMAT) == 4, "D3DFORMAT is a 4-byte enum in d3d9types.h");
static_assert(sizeof(_D3DMULTISAMPLE_TYPE) == 4, "D3DMULTISAMPLE_TYPE is a 4-byte enum");
static_assert(sizeof(_D3DTEXTUREFILTERTYPE) == 4, "D3DTEXTUREFILTERTYPE is a 4-byte enum");
static_assert(sizeof(_D3DCUBEMAP_FACES) == 4, "D3DCUBEMAP_FACES is a 4-byte enum");
static_assert(sizeof(D3DDISPLAYMODE) == 16, "D3DDISPLAYMODE must match d3d9types.h");
static_assert(offsetof(D3DDISPLAYMODE, Format) == 12, "D3DDISPLAYMODE.Format offset");
static_assert(sizeof(D3DFMT_DXT1) == 4, "D3DFMT_* enumerator width");

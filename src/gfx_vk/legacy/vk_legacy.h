#pragma once

// The Vulkan Legacy adapter: the part of Direct3D 9 the renderer uses, on the
// shared Vulkan core (gfx_vk) and the Legacy shader translator.
//
// The renderer reaches the graphics API only through src/gfx_d3d/rhi/r_rhi.h.
// Its R_HW_* calls are D3D9-shaped, so this adapter is too: a device with
// render states, sampler states, textures, vertex and index buffers, vertex
// declarations, shader constants, render targets, draws, copies and present,
// with D3D9's semantics (lock flags, the half-pixel offset, alpha test as
// state, clears limited by viewport and scissor). It takes D3D9's enum values
// as plain numbers (the d3d namespace below), so it needs no d3d9.h and can be
// tested on its own; the shim that connects it to r_rhi.h passes the
// renderer's values straight through.
//
// What the adapter does per draw: the render states, the vertex declaration,
// the two shaders and the render-target formats pick a pipeline (built once,
// through the core's pipeline cache); the shaders' constant registers and the
// states the translator turned into uniform blocks (alpha test, sampler state,
// ...) go into per-frame memory; textures and samplers go into a descriptor
// set. Pipeline state the render states change often (depth, stencil, cull,
// topology, viewport, scissor, depth bias) is dynamic state, so it never makes
// a new pipeline.
//
// Threading: one thread calls everything, like the D3D9 device it replaces.
// See docs/vulkan-legacy-adapter.md.

#include <gfx_vk/vk_core.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace gfx_vk::legacy
{
// ---------------------------------------------------------------------------
// D3D9 values, as d3d9types.h numbers them. Only the ones the renderer uses
// (and their neighbours, where a table costs nothing) are listed.
// ---------------------------------------------------------------------------
namespace d3d
{
constexpr uint32_t FourCC(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

// D3DFORMAT
enum Format : uint32_t
{
    FMT_UNKNOWN = 0,
    FMT_A8R8G8B8 = 21,
    FMT_X8R8G8B8 = 22,
    FMT_R5G6B5 = 23,
    FMT_X1R5G5B5 = 24,
    FMT_A1R5G5B5 = 25,
    FMT_A4R4G4B4 = 26,
    FMT_A8 = 28,
    FMT_A8B8G8R8 = 32,
    FMT_X8B8G8R8 = 33,
    FMT_G16R16 = 34,
    FMT_A16B16G16R16 = 36,
    FMT_L8 = 50,
    FMT_A8L8 = 51,
    FMT_V8U8 = 60,
    FMT_D16_LOCKABLE = 70,
    FMT_D32 = 71,
    FMT_D15S1 = 73,
    FMT_D24S8 = 75,
    FMT_D24X8 = 77,
    FMT_D24X4S4 = 79,
    FMT_D16 = 80,
    FMT_D32F_LOCKABLE = 82,
    FMT_D24FS8 = 83,
    FMT_INDEX16 = 101,
    FMT_INDEX32 = 102,
    FMT_R16F = 111,
    FMT_G16R16F = 112,
    FMT_A16B16G16R16F = 113,
    FMT_R32F = 114,
    FMT_G32R32F = 115,
    FMT_A32B32G32R32F = 116,
    FMT_DXT1 = FourCC('D', 'X', 'T', '1'),
    FMT_DXT2 = FourCC('D', 'X', 'T', '2'),
    FMT_DXT3 = FourCC('D', 'X', 'T', '3'),
    FMT_DXT4 = FourCC('D', 'X', 'T', '4'),
    FMT_DXT5 = FourCC('D', 'X', 'T', '5'),
};

// D3DRENDERSTATETYPE
enum RenderState : uint32_t
{
    RS_ZENABLE = 7,
    RS_FILLMODE = 8,
    RS_ZWRITEENABLE = 14,
    RS_ALPHATESTENABLE = 15,
    RS_SRCBLEND = 19,
    RS_DESTBLEND = 20,
    RS_CULLMODE = 22,
    RS_ZFUNC = 23,
    RS_ALPHAREF = 24,
    RS_ALPHAFUNC = 25,
    RS_ALPHABLENDENABLE = 27,
    RS_FOGENABLE = 28,
    RS_STENCILENABLE = 52,
    RS_STENCILFAIL = 53,
    RS_STENCILZFAIL = 54,
    RS_STENCILPASS = 55,
    RS_STENCILFUNC = 56,
    RS_STENCILREF = 57,
    RS_STENCILMASK = 58,
    RS_STENCILWRITEMASK = 59,
    RS_TEXTUREFACTOR = 60,
    RS_CLIPPLANEENABLE = 152,
    RS_MULTISAMPLEANTIALIAS = 161,
    RS_COLORWRITEENABLE = 168,
    RS_BLENDOP = 171,
    RS_SCISSORTESTENABLE = 174,
    RS_SLOPESCALEDEPTHBIAS = 175,
    RS_ADAPTIVETESS_Y = 181, // alpha-to-coverage switch by vendor convention
    RS_TWOSIDEDSTENCILMODE = 185,
    RS_CCW_STENCILFAIL = 186,
    RS_CCW_STENCILZFAIL = 187,
    RS_CCW_STENCILPASS = 188,
    RS_CCW_STENCILFUNC = 189,
    RS_COLORWRITEENABLE1 = 190,
    RS_COLORWRITEENABLE2 = 191,
    RS_COLORWRITEENABLE3 = 192,
    RS_BLENDFACTOR = 193,
    RS_SRGBWRITEENABLE = 194,
    RS_DEPTHBIAS = 195,
    RS_SEPARATEALPHABLENDENABLE = 206,
    RS_SRCBLENDALPHA = 207,
    RS_DESTBLENDALPHA = 208,
    RS_BLENDOPALPHA = 209,
    RS_COUNT = 210,
};
// RS_ADAPTIVETESS_Y values that turn alpha-to-coverage on and off.
constexpr uint32_t kAlphaToCoverageOn = FourCC('A', 'T', 'O', 'C');

// D3DSAMPLERSTATETYPE
enum SamplerState : uint32_t
{
    SAMP_ADDRESSU = 1,
    SAMP_ADDRESSV = 2,
    SAMP_ADDRESSW = 3,
    SAMP_BORDERCOLOR = 4,
    SAMP_MAGFILTER = 5,
    SAMP_MINFILTER = 6,
    SAMP_MIPFILTER = 7,
    SAMP_MIPMAPLODBIAS = 8, // a float's bits
    SAMP_MAXMIPLEVEL = 9,
    SAMP_MAXANISOTROPY = 10,
    SAMP_SRGBTEXTURE = 11,
    SAMP_COUNT = 14,
};

enum TextureAddress : uint32_t { TADDRESS_WRAP = 1, TADDRESS_MIRROR, TADDRESS_CLAMP, TADDRESS_BORDER, TADDRESS_MIRRORONCE };
enum TextureFilter : uint32_t { TEXF_NONE = 0, TEXF_POINT, TEXF_LINEAR, TEXF_ANISOTROPIC };
enum Blend : uint32_t
{
    BLEND_ZERO = 1, BLEND_ONE, BLEND_SRCCOLOR, BLEND_INVSRCCOLOR, BLEND_SRCALPHA, BLEND_INVSRCALPHA, BLEND_DESTALPHA,
    BLEND_INVDESTALPHA, BLEND_DESTCOLOR, BLEND_INVDESTCOLOR, BLEND_SRCALPHASAT, BLEND_BOTHSRCALPHA,
    BLEND_BOTHINVSRCALPHA, BLEND_BLENDFACTOR, BLEND_INVBLENDFACTOR,
};
enum BlendOp : uint32_t { BLENDOP_ADD = 1, BLENDOP_SUBTRACT, BLENDOP_REVSUBTRACT, BLENDOP_MIN, BLENDOP_MAX };
enum Cmp : uint32_t { CMP_NEVER = 1, CMP_LESS, CMP_EQUAL, CMP_LESSEQUAL, CMP_GREATER, CMP_NOTEQUAL, CMP_GREATEREQUAL, CMP_ALWAYS };
enum StencilOp : uint32_t
{
    STENCILOP_KEEP = 1, STENCILOP_ZERO, STENCILOP_REPLACE, STENCILOP_INCRSAT, STENCILOP_DECRSAT, STENCILOP_INVERT,
    STENCILOP_INCR, STENCILOP_DECR,
};
enum Cull : uint32_t { CULL_NONE = 1, CULL_CW, CULL_CCW };
enum FillMode : uint32_t { FILL_POINT = 1, FILL_WIREFRAME, FILL_SOLID };

// D3DPRIMITIVETYPE
enum PrimitiveType : uint32_t { PT_POINTLIST = 1, PT_LINELIST, PT_LINESTRIP, PT_TRIANGLELIST, PT_TRIANGLESTRIP, PT_TRIANGLEFAN };

// D3DDECLTYPE and D3DDECLUSAGE
enum DeclType : uint8_t
{
    DECLTYPE_FLOAT1 = 0, DECLTYPE_FLOAT2, DECLTYPE_FLOAT3, DECLTYPE_FLOAT4, DECLTYPE_D3DCOLOR, DECLTYPE_UBYTE4,
    DECLTYPE_SHORT2, DECLTYPE_SHORT4, DECLTYPE_UBYTE4N, DECLTYPE_SHORT2N, DECLTYPE_SHORT4N, DECLTYPE_USHORT2N,
    DECLTYPE_USHORT4N, DECLTYPE_UDEC3, DECLTYPE_DEC3N, DECLTYPE_FLOAT16_2, DECLTYPE_FLOAT16_4, DECLTYPE_UNUSED,
};
enum DeclUsage : uint8_t
{
    DECLUSAGE_POSITION = 0, DECLUSAGE_BLENDWEIGHT, DECLUSAGE_BLENDINDICES, DECLUSAGE_NORMAL, DECLUSAGE_PSIZE,
    DECLUSAGE_TEXCOORD, DECLUSAGE_TANGENT, DECLUSAGE_BINORMAL, DECLUSAGE_TESSFACTOR, DECLUSAGE_POSITIONT,
    DECLUSAGE_COLOR, DECLUSAGE_FOG, DECLUSAGE_DEPTH, DECLUSAGE_SAMPLE,
};

// D3DUSAGE, D3DPOOL, D3DLOCK and D3DCLEAR flags
constexpr uint32_t USAGE_RENDERTARGET = 0x1;
constexpr uint32_t USAGE_DEPTHSTENCIL = 0x2;
constexpr uint32_t USAGE_WRITEONLY = 0x8;
constexpr uint32_t USAGE_DYNAMIC = 0x200;
enum Pool : uint32_t { POOL_DEFAULT = 0, POOL_MANAGED, POOL_SYSTEMMEM, POOL_SCRATCH };
constexpr uint32_t LOCK_READONLY = 0x10;
constexpr uint32_t LOCK_DISCARD = 0x2000;
constexpr uint32_t LOCK_NOOVERWRITE = 0x1000;
constexpr uint32_t CLEAR_TARGET = 0x1;
constexpr uint32_t CLEAR_ZBUFFER = 0x2;
constexpr uint32_t CLEAR_STENCIL = 0x4;

// D3DCUBEMAP_FACES: +x, -x, +y, -y, +z, -z, as Vulkan's cube layers are.
} // namespace d3d

// ---------------------------------------------------------------------------
// Objects. Opaque; created and destroyed through the device. Destroying an
// object the GPU still uses is safe: the core keeps its memory until then.
// ---------------------------------------------------------------------------
struct Device;
struct Texture;      // 2D, cube or volume texture (D3D9's three texture interfaces)
struct Surface;      // a render target, depth-stencil or plain surface, or one texture level
struct VertexBuffer;
struct IndexBuffer;
struct Shader;       // a translated vertex or pixel shader
struct VertexDecl;
struct Query;

struct DeviceDesc
{
    // Back buffer size; 0 takes the swapchain's.
    uint32_t width = 0;
    uint32_t height = 0;
    // Where the adapter's messages go (one line, no newline). Null: nowhere.
    void (*log)(void *user, const char *line) = nullptr;
    void *logUser = nullptr;
};

// The device draws through an existing core, which keeps owning the window,
// swapchain and GPU. Null on failure, with the reason.
Device *CreateDevice(Core *core, const DeviceDesc &desc, std::string &error);
void DestroyDevice(Device *device);
// The first failure, if any; a failed device ignores further calls.
const char *DeviceError(const Device *device);

struct DeviceStats
{
    uint64_t draws;
    uint64_t clears;
    uint64_t passes;          // render passes begun (a target change or a texture read after write)
    uint32_t pipelines;       // distinct pipelines built
    uint32_t pipelinesInFrame;
    uint32_t shaderPairs;     // linked vertex/pixel shader pairs
    uint64_t descriptorSets;
    uint64_t constantBytes;   // constant and render-state uniform data written
    uint64_t dynamicRenames;  // DISCARD locks that took a new buffer
    // Where the time goes on the CPU, for the load and first-draw costs.
    uint32_t shaders;          // shaders created
    uint64_t shaderMicros;     // translating them (vertex modules included)
    uint64_t pipelineMicros;   // building pipelines (pixel modules included)
    uint64_t textureUploads;   // texture levels uploaded by UnlockRect
    uint64_t uploadBytes;
    uint64_t uploadMicros;     // staging those uploads
};
const DeviceStats &Stats(const Device *device);

// ---- Resources ---------------------------------------------------------------------

enum class TextureKind : uint8_t
{
    Tex2D,
    Cube,
    Volume,
};

struct TextureDesc
{
    TextureKind kind = TextureKind::Tex2D;
    uint32_t width = 1, height = 1, depth = 1; // edge length in width for cubes
    uint32_t levels = 1;  // 0: the full chain
    uint32_t usage = 0;   // d3d::USAGE_*
    d3d::Format format = d3d::FMT_A8R8G8B8;
    d3d::Pool pool = d3d::POOL_MANAGED;
    const char *name = nullptr;
};
// Null when the format is not supported (see FormatSupported) or on failure.
Texture *CreateTexture(Device *device, const TextureDesc &desc);
void DestroyTexture(Device *device, Texture *texture);
const TextureDesc &Describe(const Texture *texture); // levels resolved
// Whether the adapter can create textures of this format for this usage.
// Without a device, whether the adapter knows the format for that usage at all
// (D3D9's format checks run before a device exists).
bool FormatSupported(Device *device, d3d::Format format, uint32_t usage);

struct LockedRect
{
    void *bits = nullptr;
    uint32_t pitch = 0;      // bytes per row (of 4x4 blocks for DXT formats)
    uint32_t slicePitch = 0; // volume textures
};
struct Box
{
    uint32_t left = 0, top = 0, front = 0, right = 0, bottom = 0, back = 1; // right/bottom/back 0: the whole level
};
// face is the cube face for cube textures, 0 otherwise. The data written
// reaches the GPU at UnlockRect. Lock levels of SYSTEMMEM textures to read
// what GetRenderTargetData copied.
bool LockRect(Device *device, Texture *texture, uint32_t face, uint32_t level, const Box *box, uint32_t flags,
              LockedRect &out);
void UnlockRect(Device *device, Texture *texture, uint32_t face, uint32_t level);

// Surfaces: GetSurfaceLevel's view of one texture level, or standalone render
// targets, depth-stencil surfaces and plain (SYSTEMMEM) surfaces.
Surface *GetSurfaceLevel(Device *device, Texture *texture, uint32_t face, uint32_t level);
Surface *CreateRenderTarget(Device *device, uint32_t width, uint32_t height, d3d::Format format, const char *name);
Surface *CreateDepthStencil(Device *device, uint32_t width, uint32_t height, d3d::Format format, const char *name);
Surface *CreatePlainSurface(Device *device, uint32_t width, uint32_t height, d3d::Format format, d3d::Pool pool,
                            const char *name);
// Surfaces from GetSurfaceLevel go away with their texture; this releases
// standalone ones (and is a no-op for texture levels and the back buffer).
void DestroySurface(Device *device, Surface *surface);
Surface *BackBuffer(Device *device);
void SurfaceSize(const Surface *surface, uint32_t &width, uint32_t &height, d3d::Format &format);
// box null: the whole surface.
bool LockSurface(Device *device, Surface *surface, const Box *box, uint32_t flags, LockedRect &out);
void UnlockSurface(Device *device, Surface *surface);

VertexBuffer *CreateVertexBuffer(Device *device, uint32_t bytes, uint32_t usage, d3d::Pool pool, const char *name);
void DestroyVertexBuffer(Device *device, VertexBuffer *buffer);
IndexBuffer *CreateIndexBuffer(Device *device, uint32_t bytes, uint32_t usage, d3d::Format format, d3d::Pool pool,
                               const char *name);
void DestroyIndexBuffer(Device *device, IndexBuffer *buffer);
// D3D9's lock semantics: DISCARD hands out fresh memory for a dynamic buffer
// the GPU may still read; NOOVERWRITE promises not to touch what it reads;
// neither keeps the contents. bytes 0: to the end.
void *Lock(Device *device, VertexBuffer *buffer, uint32_t offset, uint32_t bytes, uint32_t flags);
void Unlock(Device *device, VertexBuffer *buffer);
void *Lock(Device *device, IndexBuffer *buffer, uint32_t offset, uint32_t bytes, uint32_t flags);
void Unlock(Device *device, IndexBuffer *buffer);

// Shaders from D3D9 bytecode (vs/ps 1.1 to 3.0). Null on failure; the reason
// is in DeviceError only for device failures, translation failures go to the
// log and return null like a D3D9 CreateVertexShader that fails.
Shader *CreateShader(Device *device, const void *bytecode, size_t bytes, std::string &error);
void DestroyShader(Device *device, Shader *shader);

// D3DVERTEXELEMENT9, without the end marker.
struct VertexElement
{
    uint16_t stream;
    uint16_t offset;
    uint8_t type;   // d3d::DeclType
    uint8_t method; // must be 0 (D3DDECLMETHOD_DEFAULT)
    uint8_t usage;  // d3d::DeclUsage
    uint8_t usageIndex;
};
VertexDecl *CreateVertexDecl(Device *device, const VertexElement *elements, uint32_t count);
void DestroyVertexDecl(Device *device, VertexDecl *decl);

// ---- State --------------------------------------------------------------------------

constexpr uint32_t kMaxStreams = 16;
constexpr uint32_t kMaxRenderTargets = 4;
constexpr uint32_t kVertexSamplerBase = 257; // D3DVERTEXTEXTURESAMPLER0

void SetRenderState(Device *device, d3d::RenderState state, uint32_t value);
uint32_t GetRenderState(const Device *device, d3d::RenderState state);
// sampler: 0-15 for the pixel shader, kVertexSamplerBase + 0-3 for the vertex shader.
void SetSamplerState(Device *device, uint32_t sampler, d3d::SamplerState state, uint32_t value);
void SetTexture(Device *device, uint32_t sampler, Texture *texture);
void SetVertexShader(Device *device, Shader *shader);
void SetPixelShader(Device *device, Shader *shader);
void SetVertexShaderConstantF(Device *device, uint32_t start, const float *data, uint32_t count);
void SetPixelShaderConstantF(Device *device, uint32_t start, const float *data, uint32_t count);
void SetVertexDecl(Device *device, VertexDecl *decl);
void SetStreamSource(Device *device, uint32_t stream, VertexBuffer *buffer, uint32_t offset, uint32_t stride);
void SetIndices(Device *device, IndexBuffer *buffer);

struct Viewport
{
    uint32_t x, y, width, height;
    float minZ, maxZ;
};
void SetViewport(Device *device, const Viewport &viewport);
struct Rect
{
    int32_t left, top, right, bottom;
};
void SetScissorRect(Device *device, const Rect &rect);
// Setting target 0 resets the viewport and scissor to the whole surface, as
// D3D9 does. Null detaches (targets 1-3, or the depth surface).
void SetRenderTarget(Device *device, uint32_t index, Surface *surface);
void SetDepthStencilSurface(Device *device, Surface *surface);

// ---- Drawing --------------------------------------------------------------------------

// Clears the viewport (limited by the scissor rect when the scissor test is
// on), or the given rects inside it. color is D3DCOLOR (0xAARRGGBB).
void Clear(Device *device, uint32_t rectCount, const Rect *rects, uint32_t flags, uint32_t color, float z,
           uint32_t stencil);
void DrawIndexedPrimitive(Device *device, d3d::PrimitiveType type, int32_t baseVertex, uint32_t minIndex,
                          uint32_t vertexCount, uint32_t startIndex, uint32_t primitiveCount);
void DrawPrimitive(Device *device, d3d::PrimitiveType type, uint32_t startVertex, uint32_t primitiveCount);
// Vertices from memory into stream 0 (whose binding it then clears, like D3D9).
void DrawPrimitiveUP(Device *device, d3d::PrimitiveType type, uint32_t primitiveCount, const void *vertices,
                     uint32_t stride);

// ---- Copies, frames and queries -------------------------------------------------------

// StretchRect: a scaled copy between render-target surfaces or texture levels.
// Rects null: the whole surface. linear: D3DTEXF_LINEAR (else point).
void StretchRect(Device *device, Surface *src, const Rect *srcRect, Surface *dst, const Rect *dstRect, bool linear);
// GetRenderTargetData: waits for the GPU and copies a render target into a
// SYSTEMMEM surface of the same size and format (4 bytes per pixel formats).
bool GetRenderTargetData(Device *device, Surface *renderTarget, Surface *dst);
// UpdateTexture: every level of a SYSTEMMEM texture into a matching texture.
void UpdateTexture(Device *device, Texture *src, Texture *dst);

// D3D9's BeginScene/EndScene bracket nothing here; Present shows the back
// buffer (scaled to the window when their sizes differ).
enum class PresentResult
{
    Ok,
    Skipped, // window minimised or swapchain rebuilt: nothing shown
    Failed,
};
PresentResult Present(Device *device);

// The device's window is about to be destroyed: an open pass and frame end
// unshown, and the core lets go of its swapchain and surface (DetachWindow in
// vk_core.h). Everything else stays usable; Present skips from now on.
void DetachWindow(Device *device);
// Changes the back buffer size (D3D9's Reset); the window's swapchain follows
// on its own. Every resource stays valid.
bool ResizeBackBuffer(Device *device, uint32_t width, uint32_t height);
// Puts every state back to D3D9's defaults and unbinds everything, with the
// back buffer as render target 0 (what D3D9's Reset does to state).
void ResetState(Device *device);
void WaitIdle(Device *device);

enum class QueryKind : uint8_t
{
    Event,     // D3DQUERYTYPE_EVENT: done when the GPU passed it
    Occlusion, // D3DQUERYTYPE_OCCLUSION: not counted yet; reports every
               // sample as passed once the GPU reached the end
};
Query *CreateQuery(Device *device, QueryKind kind);
void DestroyQuery(Device *device, Query *query);
// D3DISSUE_BEGIN / D3DISSUE_END. Event queries take End only.
void IssueQuery(Device *device, Query *query, bool begin);
// D3D9's GetData: false while the result is not ready (flush asks the GPU to
// start, like D3DGETDATA_FLUSH). result receives TRUE for events and the
// sample count for occlusion queries.
bool GetQueryData(Device *device, Query *query, bool flush, uint32_t &result);
} // namespace gfx_vk::legacy

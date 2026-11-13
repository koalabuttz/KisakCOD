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

#if defined(KISAK_GFX_GXM)
// The PS Vita's back end (src/gfx_gxm/legacy) implements this API on sceGxm
// and owns the display itself: there is no Vulkan core, and CreateDevice
// takes a null one.
namespace gfx_vk
{
struct Core;
}
#else
#include <gfx_vk/vk_core.h>
#endif

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
constexpr uint32_t LOCK_NO_DIRTY_UPDATE = 0x8000;
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
    // The shader cache file (see LoadShaderCache): read when the device is
    // created, unless the process already read it, and written when the device
    // is destroyed if anything new was translated. Null or empty: none.
    // The PS Vita's GXM back end reads it as a folder, one compiled program
    // per file (src/gfx_gxm/gxm_program_cache.h).
    const char *shaderCachePath = nullptr;
    // The pipeline list file (see "Pipelines built ahead" below): read when
    // the device is created and written when it is destroyed. Null or empty:
    // none, and every pipeline is built at its first draw. The GXM back end
    // ignores it.
    const char *pipelineListPath = nullptr;
    // Samples a pixel of the back buffer (see "Multisampled targets" below).
    // The GXM back end has one.
    uint32_t backBufferSamples = 1;
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
    uint64_t passClears;      // of those, done as their pass's load operation (OptionalFeatures::passClears)
    uint64_t passes;          // render passes begun (a target change or a texture read after write)
    uint32_t pipelines;       // distinct pipelines built
    uint32_t pipelinesInFrame;
    uint32_t shaderPairs;     // linked vertex/pixel shader pairs
    uint64_t descriptorSets;
    uint64_t descriptorSetsReused; // sets bound again instead of allocated and written
    uint64_t lastingSets;     // of descriptorSets, those kept across frames (kept draw state)
    uint32_t lastingRetires;  // times every lasting set was given up (an image destroyed, too many)
    uint64_t constantBytes;   // constant and render-state uniform data written
    // Vertex buffer, index buffer and descriptor set binds a draw did not
    // record because its command buffer had them from an earlier draw.
    uint64_t bindsSkipped;
    uint64_t dynamicRenames;  // DISCARD locks that took a new buffer
    uint64_t fenceFlushes;    // frames submitted ahead of their Present by a frame fence poll (GetQueryData)
    uint64_t feedFlushes;     // parts of frames submitted while the frame was being drawn (GpuFeed)
    uint64_t feedIdleFlushes; // of those, parts the idle rule sent early (GpuFeed::idleDraws)
    // What the occlusion queries answered, on a device that counts samples:
    // the results worked out for queries that were issued (one for each END
    // that was read, however often), those of them with no sample, all their
    // samples, and the newest. The engine's one question is the sun's sprite,
    // so these say whether it saw the sun without a picture being looked at.
    uint64_t occlusionResults;
    uint64_t occlusionResultsZero;
    uint64_t occlusionSamples;
    uint32_t occlusionLast;
    // Where the time goes on the CPU, for the load and first-draw costs.
    uint32_t shaders;          // shaders created
    uint32_t shadersCached;    // of those, served by the shader cache (not translated)
    uint64_t shaderMicros;     // translating them (vertex modules included)
    uint64_t pipelineMicros;   // building pipelines (pixel modules included)
    // Of those pipelines, the pixel shader modules built for them (a shader's
    // SPIR-V with the pipeline's fixed state folded in, see PixelModule) and
    // the time that took, the rest of pipelineMicros being the driver's.
    uint32_t pixelModules;
    uint64_t pixelModuleMicros;
    // Of the pixel modules kept (built ahead or not), those built as a Modern
    // lit shader of ours (SetLinearInputs' shader), and those that wanted one
    // but were built from the pixel shader as it is (it did not link).
    uint32_t modernLitModules;
    uint32_t modernLitRefused;
    // Pipelines built ahead on worker threads (see "Pipelines built ahead"),
    // not counted in pipelines or pipelineMicros: those built, the workers'
    // time on them, those a draw then used, the draws that had to wait for
    // one a worker was still building and how long they waited, and the
    // entries the pipeline list file held when the device was created; then
    // the time the creating thread spent queueing jobs as shaders and
    // declarations were made (for shaders, inside shaderMicros).
    uint32_t pipelinesAhead;
    uint64_t aheadMicros;
    uint32_t aheadUsed;
    uint32_t aheadWaits;
    uint64_t aheadWaitMicros;
    uint32_t pipelinesListed;
    uint64_t aheadQueueMicros;
    // Of the pipelines built ahead, the pixel modules the workers built for
    // them (as pixelModules) and the workers' time on those.
    uint32_t aheadPixelModules;
    uint64_t aheadPixelModuleMicros;
    uint64_t textureUploads;   // copy commands of UnlockRect and UpdateTexture, of one or more regions of a level
    uint64_t textureReadbacks; // levels read back to lock a texture again (once each)
    uint64_t uploadBytes;
    uint64_t uploadMicros;     // staging those uploads
    uint64_t gammaPresents;    // frames Present showed through the gamma ramp (SetGammaRamp)
};
const DeviceStats &Stats(const Device *device);

#if !defined(KISAK_GFX_GXM)
// How a frame reaches the GPU while it is drawn (FeedGpu in
// vk_legacy_draw.cpp). A device starts with its core's choice
// (OptionalFeatures::gpuFeed, gpuFeedAlways and gpuFeedDraws: the kind of GPU
// and the KISAK_VK_GPU_FEED switches) and no idle rule. SetGpuFeed changes it
// at any point between two calls, so one run can compare settings; the change
// is logged.
struct GpuFeed
{
    bool on = false;     // submit a frame in parts at all
    bool always = false; // a part after every draw (testing)
    // Draws to a part while the GPU has under two submissions left to finish
    // (more for each further one, up to four times). At least 1.
    uint32_t draws = 128;
    // The idle rule, 0 for none: while the GPU has no submission left at all,
    // a part goes out after this many draws, the next such part of the frame
    // after twice as many, and so on until that reaches `draws`. A GPU that
    // has run dry gets work at once, and a frame is cut into only a few parts
    // more than without the rule.
    uint32_t idleDraws = 0;
};
GpuFeed GetGpuFeed(const Device *device);
void SetGpuFeed(Device *device, const GpuFeed &feed);

// Whether a draw takes up what the draw before it left: the pipeline and the
// dynamic state while nothing they are made of was set, the uniform blocks
// whose bytes would be the same, and the binds its command buffer has already
// (vk_legacy_draw.cpp). On when a device is created. Off, every draw makes
// its pipeline key and dynamic state again, writes all its uniform blocks and
// records every bind, as the adapter did before it kept any of them: for
// measuring what keeping them is worth on a machine, and for telling a fault
// of the kept state from any other. SetKeepDrawState changes it at any point
// between two calls; the change is logged.
bool GetKeepDrawState(const Device *device);
void SetKeepDrawState(Device *device, bool keep);

// Pipelines built ahead. A pipeline is built the first time a draw needs it,
// which costs that frame from about a millisecond (the driver's cache has it)
// to over a hundred (a cold driver compiling the shaders). The device keeps a
// list of the pipelines draws used: each entry names its shaders and vertex
// declaration by their contents and holds the rest of the pipeline's state.
// A device created with DeviceDesc::pipelineListPath reads the list at the
// start; whenever the last shader or declaration an entry needs is created
// (the game creates them as it loads a map), the entry's pipeline is queued
// to worker threads (OptionalFeatures::pipelinesAhead), so it is usually
// built before the first draw that needs it. A draw that needs one a worker
// is still building waits for it; one only queued is built by the draw. The
// list is written when the device is destroyed: entries drawn this launch,
// and those not drawn in fewer than kPipelineListAge launches that built
// them. The file names the GPU: another GPU starts with an empty list.
//
// SavePipelineList and LoadPipelineList write and read the list now (message
// says what they did); a load replaces the device's list and queues what it
// can build. FinishPipelinesAhead waits until the workers have nothing left
// and hands their pipelines to the device. For checks and tools.
constexpr uint32_t kPipelineListAge = 8;
bool SavePipelineList(Device *device, const char *path, std::string &message);
bool LoadPipelineList(Device *device, const char *path, std::string &message);
void FinishPipelinesAhead(Device *device);
uint32_t PipelineListSize(const Device *device);
#endif

// The shader cache. Translated shaders are kept by their D3D9 bytecode for the
// whole process and shared by every device, so bytecode created again (another
// device, a vid_restart, the same shader twice) is not translated again. A file
// keeps them between runs: it holds the translator's revision
// (legacy_shader::TranslatorRevision) and is ignored whole when that differs or
// when anything in it is damaged.
//
// LoadShaderCache adds a file's shaders; false when there is no usable file
// (message says why). SaveShaderCache writes every cached shader when some are
// not in the file yet (message says what it did); the file is replaced whole,
// through a temporary file beside it.
bool LoadShaderCache(const char *path, std::string &message);
bool SaveShaderCache(const char *path, std::string &message);
// Forgets every cached shader (the file stays). For checks and tools.
void ClearShaderCache();
uint32_t ShaderCacheSize();

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

#if !defined(KISAK_GFX_GXM)
// Multisampled targets: the back buffer (DeviceDesc::backBufferSamples,
// SetBackBufferSamples) and standalone render targets and depth surfaces, as
// D3D9 has them. A draw's targets must all have the same count. A
// multisampled colour surface is drawn into and cleared, and is the source
// of StretchRect, which resolves it (every pixel the average of its
// samples), and of GetRenderTargetData, which reads the resolved picture
// (D3D9 refuses that; a screenshot gets the picture it shows). It is not the
// destination of StretchRect and is not locked. Present resolves a
// multisampled back buffer into the front buffer and shows that.
//
// A count the device does not have is made with 4 samples, which every
// Vulkan device has for colour and depth: UsableSamples says what a count
// becomes (1 stays 1).
uint32_t UsableSamples(const Device *device, uint32_t asked);
Surface *CreateMultisampledRenderTarget(Device *device, uint32_t width, uint32_t height, d3d::Format format,
                                        uint32_t samples, const char *name);
Surface *CreateMultisampledDepthStencil(Device *device, uint32_t width, uint32_t height, d3d::Format format,
                                        uint32_t samples, const char *name);
uint32_t SurfaceSamples(const Surface *surface);
// The back buffer's count from the next ResizeBackBuffer on (which a call
// with the size it has also takes up). Returns the count it will have.
uint32_t SetBackBufferSamples(Device *device, uint32_t samples);
#endif
Surface *BackBuffer(Device *device);
// The image the last Present showed (D3D9's front buffer). Present flips the
// two, as D3D9's DISCARD swap effect allows, so drawing the next frame never
// touches it: GetFrontBufferData reads it at any point of a frame.
Surface *FrontBuffer(Device *device);
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
// Rects null: the whole surface. linear: D3DTEXF_LINEAR (else point). From a
// multisampled surface it is the resolve.
void StretchRect(Device *device, Surface *src, const Rect *srcRect, Surface *dst, const Rect *dstRect, bool linear);
// GetRenderTargetData: waits for the GPU and copies a render target into a
// SYSTEMMEM surface of the same size and format (4 bytes per pixel formats).
bool GetRenderTargetData(Device *device, Surface *renderTarget, Surface *dst);
// UpdateTexture: a SYSTEMMEM texture into a matching texture, as D3D9 does it:
// only what is dirty, in every level, and then nothing is. A new texture is
// dirty all over; a lock without LOCK_NO_DIRTY_UPDATE or LOCK_READONLY marks
// its region, and AddDirtyRegion marks one (AddDirtyRect, AddDirtyBox).
void UpdateTexture(Device *device, Texture *src, Texture *dst);
// box in level 0's texels; null marks the whole face. Only SYSTEMMEM and
// SCRATCH textures keep it: other textures upload at every unlock.
void AddDirtyRegion(Device *device, Texture *texture, uint32_t face, const Box *box);

// D3D9's BeginScene/EndScene bracket nothing here; Present shows the back
// buffer (scaled to the window when their sizes differ), then makes it the
// front buffer and the old front buffer the back buffer. The new back
// buffer's contents are undefined, as with D3D9's DISCARD swap effect.
enum class PresentResult
{
    Ok,
    Skipped, // window minimised or swapchain rebuilt: nothing shown
    Failed,
};
PresentResult Present(Device *device);
#if !defined(KISAK_GFX_GXM)
// With `shown`, the window's image is also read back as Present leaves it
// (ReadImage on the swapchain image, which needs CoreDesc::allowCapture), for
// checks of what reaches the window; *shown stays null when it cannot be.
PresentResult Present(Device *device, gfx_vk::Readback **shown);
#endif

#if !defined(KISAK_GFX_GXM)
// D3D9's gamma ramp of a fullscreen swapchain (SetGammaRamp): Present maps
// each channel of the shown image through it on the way to the window, with
// one full-screen pass. Null, or a ramp that keeps every value (i * 257),
// shows the image as drawn and costs nothing. The front passes it only for a
// fullscreen device, since D3D9 ignores the ramp of a windowed one.
struct GammaRamp
{
    uint16_t red[256];
    uint16_t green[256];
    uint16_t blue[256];
};
void SetGammaRamp(Device *device, const GammaRamp *ramp);
#endif

#if !defined(KISAK_GFX_GXM)
// ---- Passes of another user of the core (the Modern path) --------------------------------
//
// Ends the pass being drawn and forgets what the device has bound, so that
// the caller can record passes of its own on the core (vk_core.h) between
// draws; the next draw begins a new pass and binds everything again. Opens
// the frame if none is open. False, with nothing to record into, when the
// frame is skipped (minimised window) or the device has failed.
bool HandOverRecording(Device *device);
// The core image a surface draws into: its texture's image when the surface
// is level 0 of a 2D texture or a standalone surface, else null.
gfx_vk::Image *SurfaceImage(const Surface *surface);
// Marks a render target texture as holding linear light (the Modern path's
// scene, docs/vulkan-modern.md): draws into it as render target 0 run their
// pixel shaders with colour output 0 taken from sRGB to linear light
// (legacy_shader::BuildSpirv's linearOutput), and clears of it take their
// colour from sRGB to linear light too. Blending then happens in linear light.
void SetLinearLight(Texture *texture, bool linear);
// What the last DrawIndexedPrimitive of a triangle list drew from, as it is
// still bound: stream 0's buffer, the index buffer and range, and the
// viewport. The Modern path draws a surface that moves on its own again from
// it (its velocity pass). False when the last indexed draw was not a
// triangle list or stream 0 is not a vertex buffer.
struct IndexedDraw
{
    VkBuffer vertices = VK_NULL_HANDLE;
    VkDeviceSize vertexOffset = 0;
    uint32_t stride = 0;
    VkBuffer indices = VK_NULL_HANDLE;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    uint32_t firstIndex = 0, indexCount = 0;
    int32_t vertexBase = 0;
    // D3D9's MinVertexIndex and NumVertices: the draw reads vertices
    // vertexBase + minIndex onwards, vertexCount of them.
    uint32_t minIndex = 0, vertexCount = 0;
    Viewport viewport{};
};
bool LastIndexedDraw(Device *device, IndexedDraw &out);
// The colours the pixel shaders of the draws that follow read, for draws into
// a linear-light render target 0 (SetLinearLight): the red, green and blue of
// the float constants in constants (bit per register c0-c63), of what the
// samplers in samplers return (bit per sampler; decode(2v) / 2 for those also
// in doubled) and, with colorInput, of the colour 0 input are taken from sRGB
// to linear light, and the colour output is stored as computed
// (legacy_shader::BuildSpirv's linearInputs). With overWhite instead, no input
// is decoded and the colour output is decoded without its clamp to 1, so a
// colour computed over white stays over white (the masks are ignored). All
// zero (and false): none, and the colour output is decoded as always.
//
// shader, with overWhite, names a Modern lit shader of ours to draw the pixel
// shader as instead (ModernLitShader): it reads the same samplers, constant
// registers and vertex shader outputs, and computes in linear light itself.
// Where it cannot be linked to the vertex shader, the pixel shader is drawn
// as with overWhite alone.
enum ModernLitShader : uint8_t
{
    ModernLitNone = 0,
    // lm_[hsm_]sun_<mode>c0[d0][n0][s0]: a world surface lit by its lightmaps
    // and the sun. Always: colour map at s0, primary and secondary lightmaps
    // at s2 and s3, fog colour at c0, sun direction and colour at c17 and
    // c18; texcoord 0 (colour and lightmap UVs), texcoord 1 (normal, fog) and
    // colour 0 in. The flags below add to that.
    ModernLitLmSun = 0x40,
    // hsm: the sun's shadow map at s4, its partition switch and scale at c2
    // and c4, its coordinate in texcoord 4.
    ModernLitShadow = 0x01,
    // n0: a normal map at the next sampler (s4, or s5 with the shadow map);
    // binormal and tangent in texcoords 2 and 3.
    ModernLitNormal = 0x02,
    // s0 (only with n0): the reflection probe (a cube) at s1, a specular map
    // at the sampler after the normal map's, envMapParms at c5, the sun's
    // specular colour at c19; the eye-to-point vector in texcoord 5.
    ModernLitSpecular = 0x04,
    // d0: a detail map at the sampler after the others, its UV scale at the
    // constant after c4 (c5, or c6 with s0).
    ModernLitDetail = 0x08,
    // The mode in bits 4-5: r0 opaque, b0 blended (premultiplied, the alpha
    // is colour alpha times vertex alpha), t0 alpha-tested (kept at alpha
    // 0.5 and over).
    ModernLitModeShift = 4,
    ModernLitModeMask = 0x30,
    ModernLitOpaque = 0x00,
    ModernLitBlended = 0x10,
    ModernLitAlphaTested = 0x20,
};
// Whether shader names a Modern lit shader of ours.
constexpr bool ModernLitShaderValid(uint8_t shader)
{
    return (shader & 0xc0) == ModernLitLmSun && (shader & ModernLitModeMask) != 0x30
        && (!(shader & ModernLitSpecular) || (shader & ModernLitNormal));
}
void SetLinearInputs(Device *device, uint64_t constants, uint16_t samplers, uint16_t doubled, bool colorInput,
                     bool overWhite = false, uint8_t shader = ModernLitNone);
// The Modern lit shaders' light strengths in linear light: the sun's colour
// and the lightmap's light are multiplied by these (1 and 1 by default). Set
// before the first draw that uses one: shaders already built keep theirs.
void SetModernLitStrengths(Device *device, float sun, float sky);
#endif

// The device's window is about to be destroyed: an open pass and frame end
// unshown, and the core lets go of its swapchain and surface (DetachWindow in
// vk_core.h). Everything else stays usable; Present skips from now on.
void DetachWindow(Device *device);
// Changes the back buffer size (D3D9's Reset); the window's swapchain follows
// on its own. Every resource stays valid. The back buffer is made again even
// at the size it has, with the samples SetBackBufferSamples asked for.
bool ResizeBackBuffer(Device *device, uint32_t width, uint32_t height);
// Puts every state back to D3D9's defaults and unbinds everything, with the
// back buffer as render target 0 (what D3D9's Reset does to state).
void ResetState(Device *device);
void WaitIdle(Device *device);

enum class QueryKind : uint8_t
{
    Event,     // D3DQUERYTYPE_EVENT: done when the GPU passed it
    Occlusion, // D3DQUERYTYPE_OCCLUSION: the samples that passed the depth
               // and stencil tests in the draws between BEGIN and END
};
Query *CreateQuery(Device *device, QueryKind kind);
void DestroyQuery(Device *device, Query *query);
// D3DISSUE_BEGIN / D3DISSUE_END. Event queries take End only.
//
// Occlusion queries may overlap, and may span passes, parts of a frame and
// frames: each is the sum of the counts the core takes for it (CountDraw in
// vk_legacy_draw.cpp). BEGIN on a query that already has a result, or was
// never ended, starts it again from nothing. An END with no BEGIN counts
// nothing.
void IssueQuery(Device *device, Query *query, bool begin);
// D3D9's GetData: false while the result is not ready (flush asks the GPU to
// start, like D3DGETDATA_FLUSH). result receives TRUE for events and the
// sample count for occlusion queries: 0 for one never issued, and no result
// between BEGIN and END. A device that cannot count samples
// (OptionalFeatures::occlusionCounts off) reports 0xffffff, every sample
// passed, once the GPU reached the query's end.
//
// An event issued right after Present, with nothing recorded since, is a
// frame fence: it marks the end of the frame just presented. A caller
// that polls it with flush is waiting for that frame and records nothing
// meanwhile, so the first such poll that finds it unfinished also submits what
// was recorded after it: the next frame's work is then queued behind the
// frame the GPU is still on, instead of reaching it only at Present.
bool GetQueryData(Device *device, Query *query, bool flush, uint32_t &result);
} // namespace gfx_vk::legacy

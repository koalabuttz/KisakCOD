#pragma once

// Vulkan Legacy adapter internals shared by its .cpp files. Users include
// vk_legacy.h only.

#include "vk_legacy.h"

#include <gfx_vk/shader/vk_legacy_shader.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gfx_vk::legacy
{
namespace ls = gfx_vk::legacy_shader;

// Adds the time from its construction to its destruction to a counter, in
// microseconds.
class TimeInto
{
public:
    explicit TimeInto(uint64_t &sink) : sink_(sink), start_(std::chrono::steady_clock::now()) {}
    ~TimeInto()
    {
        sink_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count());
    }
    TimeInto(const TimeInto &) = delete;
    TimeInto &operator=(const TimeInto &) = delete;

private:
    uint64_t &sink_;
    std::chrono::steady_clock::time_point start_;
};

// ---- Formats (vk_legacy_resources.cpp) ------------------------------------------------

struct FormatInfo
{
    d3d::Format d3d;
    VkFormat vk;                // VK_FORMAT_UNDEFINED for depth: the core's DepthStencilFormat
    VkComponentMapping swizzle; // how shaders see it (D3D9 reads missing channels as 1, or 0 for A8's colour)
    uint8_t blockBytes;         // bytes per texel, or per 4x4 block
    bool compressed;            // 4x4 blocks
    bool depth;
    bool noAlpha;               // X8R8G8B8 and friends: blending reads destination alpha as 1
};
// Null for formats the adapter does not know.
const FormatInfo *LookupFormat(d3d::Format format);

// ---- Objects ------------------------------------------------------------------------------

// One texture level of one cube face: its CPU copy while locked, and always
// for SYSTEMMEM and SCRATCH textures.
struct Subresource
{
    std::vector<uint8_t> cpu; // the whole level, rows of blocks tightly packed
    bool locked = false;
    bool readOnly = false;
    bool uploaded = false; // the image holds data from an earlier unlock
    // Locked again after an upload: the level was read back from the image,
    // and cpu is kept from then on, as D3D9's managed copy is.
    bool keepCpu = false;
    Box box{};                // the locked region, in texels
};

struct Texture
{
    TextureDesc desc{};
    const FormatInfo *format = nullptr;
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    Image *image = nullptr; // null for SYSTEMMEM and SCRATCH textures
    // Over 1: a multisampled render target or depth surface (the back buffer
    // or a standalone surface). No shader reads one and no texture is one.
    uint32_t samples = 1;
    uint32_t faces = 1;
    std::vector<Subresource> subresources;          // face * levels + level
    std::vector<std::unique_ptr<Surface>> surfaces; // GetSurfaceLevel views, same index
    std::string name;
    // SYSTEMMEM and SCRATCH textures: what the next UpdateTexture copies, per
    // face, in level 0's texels. The boxes do not overlap; past
    // kDirtyBoxLimit of them the whole face is dirty.
    struct Dirty
    {
        bool all = true; // D3D9 counts a new texture dirty all over
        std::vector<Box> boxes;
    };
    std::vector<Dirty> dirty;
};
constexpr size_t kDirtyBoxLimit = 128;

struct Surface
{
    Texture *texture = nullptr;
    uint32_t face = 0;
    uint32_t level = 0;
    bool standalone = false; // CreateRenderTarget and friends: owns its one-level texture
};

struct Backing
{
    Buffer *buffer = nullptr;
    uint64_t lastUse = 0; // submission serial of the last draw that read it; 0: none
};

struct GpuBuffer
{
    uint32_t size = 0;
    uint32_t usage = 0;
    d3d::Pool pool = d3d::POOL_DEFAULT;
    bool index = false;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    std::string name;
    // Dynamic (and SYSTEMMEM) buffers: host-visible backings; DISCARD renames
    // to another one when the GPU may still read the current one.
    bool dynamic = false;
    std::vector<Backing> backings;
    uint32_t current = 0;
    // Static buffers: device memory, written from the CPU copy at Unlock.
    Buffer *device = nullptr;
    std::vector<uint8_t> shadow;
    bool locked = false;
    bool lockReadOnly = false;
    uint32_t lockOffset = 0;
    uint32_t lockBytes = 0;
};
struct VertexBuffer : GpuBuffer
{
};
struct IndexBuffer : GpuBuffer
{
};

struct Shader
{
    ls::Shader translated;
    uint32_t id = 0;
    ls::Stage stage = ls::Stage::Vertex;
    // Vertex shaders: their module. Pixel shaders: the module for vertex
    // shaders whose outputs it reads as they stand (built on first use).
    VkShaderModule module = VK_NULL_HANDLE;
    // Pixel shaders: the other modules, by the key PixelModule makes (the
    // vertex shader they are linked to, the depth samplers where those are
    // fixed, and the alpha test's compare where that is).
    std::unordered_map<uint64_t, VkShaderModule> linked;
    // 64-bit FNV-1a of the bytecode, by 8-byte words (ContentHash): names the
    // shader in the pipeline list, across launches.
    uint64_t contentHash = 0;
    // The id pipeline keys use: that of the oldest live shader translated to
    // the same code (Device::sameShaders), so shader objects created again
    // from the same bytecode share their pipelines and pixel modules.
    uint32_t pipelineId = 0;
};

struct VertexDecl
{
    uint32_t id = 0;
    std::vector<VertexElement> elements;
    uint64_t contentHash = 0; // of the elements, as Shader::contentHash
    uint32_t pipelineId = 0;  // as Shader::pipelineId, for the same elements
};

struct Query
{
    QueryKind kind = QueryKind::Event;
    uint64_t serial = 0; // submission that contains its end; 0: never issued
    // Issued between frames with nothing recorded since Present: it marks the
    // end of the frame just presented (the engine's swap fence). See
    // GetQueryData.
    bool frameFence = false;
    bool flushedAhead = false; // a poll has submitted the work recorded after it
    // Occlusion queries, where the core counts samples
    // (OptionalFeatures::occlusionCounts). A query is a sum of the core's
    // counts: one for each run of draws between its BEGIN and its END that
    // no pass end, submission or other query's BEGIN or END interrupts.
    bool counting = false;        // BEGIN issued, END not yet
    std::vector<uint32_t> counts; // not added up yet
    bool counted = false;         // samples holds the result
    uint32_t samples = 0;
};

// ---- Pipelines (vk_legacy_draw.cpp) --------------------------------------------------

struct PipelineKey
{
    uint32_t vs = 0, ps = 0, decl = 0;
    uint32_t colorFormats[kMaxRenderTargets] = {}; // VkFormat
    uint32_t depthFormat = 0;                      // VkFormat
    uint8_t colorCount = 0;
    uint8_t topologyClass = 0; // 0 points, 1 lines, 2 triangles
    uint8_t polygonMode = 0;   // VkPolygonMode
    uint8_t blendEnable = 0;
    uint8_t srcColor = 0, dstColor = 0, colorOp = 0; // VkBlendFactor, VkBlendOp
    uint8_t srcAlpha = 0, dstAlpha = 0, alphaOp = 0;
    uint8_t writeMask[kMaxRenderTargets] = {};
    // Pixel samplers bound to depth textures, where the device fixes depth
    // compare in the shader (OptionalFeatures::fixedDepthCompare); else 0.
    uint16_t depthSamplers = 0;
    // Each stream's stride when the device has no dynamic vertex stride
    // (OptionalFeatures::dynamicVertexStride); all zero otherwise.
    uint16_t strides[kMaxStreams] = {};
    // The samples of the pass's targets. The key is compared and hashed as
    // bytes, in 8-byte words: the rest of this word stays zero.
    uint8_t samples = 1;
    // The alpha test's compare (ls::AlphaCompare) plus 1 where the pixel
    // shader has it fixed (OptionalFeatures::fixedAlphaTest) and reads the
    // alpha test block; else 0.
    uint8_t alphaCompare = 0;
    uint8_t unused[6] = {};
    bool operator==(const PipelineKey &other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
};
struct PipelineKeyHash
{
    size_t operator()(const PipelineKey &key) const;
};

// Descriptor and pipeline layouts for one combination of what a shader pair
// reads: uniform blocks (set 0) and samplers (set 1).
struct LayoutKey
{
    uint32_t uniforms = 0; // bit per set 0 binding (vertex 0-8, pixel 16-24)
    uint32_t samplers = 0; // pixel samplers in bits 0-15, vertex samplers in bits 16-19
    bool operator==(const LayoutKey &other) const
    {
        return uniforms == other.uniforms && samplers == other.samplers;
    }
};
struct LayoutKeyHash
{
    size_t operator()(const LayoutKey &key) const
    {
        return std::hash<uint64_t>()(uint64_t(key.uniforms) << 32 | key.samplers);
    }
};
struct Layout
{
    LayoutKey key{};
    VkDescriptorSetLayout uniformSet = VK_NULL_HANDLE; // null when no uniforms
    bool dynamicUniforms = false;                      // set 0 holds dynamic uniform buffers
    VkDescriptorSetLayout textureSet = VK_NULL_HANDLE; // null when no samplers
    VkPipelineLayout pipeline = VK_NULL_HANDLE;
};

// One uniform block a pipeline's shaders read: its set 0 binding
// (ls::UniformBinding) and its size in bytes.
struct UniformBlock
{
    uint32_t binding = 0;
    uint32_t size = 0;
};
constexpr uint32_t kMaxUniformBlocks = 2 * ls::kUniformCount;
// Transient buffers a pipeline keeps a lasting set 0 for (the frame slots'
// chunks a frame's uniforms go to).
constexpr uint32_t kLastingUniformSets = 6;

struct Pipeline
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    Layout *layout = nullptr;
    uint32_t bindings = 0; // bit per vertex binding it reads (streams 0-15, kZeroBinding)
    // The blocks of set 0 in binding order. A pipeline fixes the shader pair,
    // so their sizes are fixed too.
    UniformBlock blocks[kMaxUniformBlocks];
    uint32_t blockCount = 0;
    // With dynamic uniforms: the set 0 written for this frame and transient
    // buffer (without the kept draw state).
    uint64_t uniformFrame = 0; // Device::frameSerial
    VkBuffer uniformBuffer = VK_NULL_HANDLE;
    VkDescriptorSet uniformSet = VK_NULL_HANDLE;
    // With dynamic uniforms and the kept draw state: sets 0 that last across
    // frames, one per transient buffer the blocks were in, while their
    // generation is Device::lastingGeneration. Replaced in turn.
    struct LastingUniformSet
    {
        uint64_t generation = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    } lastingUniforms[kLastingUniformSets];
    uint32_t lastingUniformNext = 0;
    // The contents of its shaders and declaration (Shader::contentHash), for
    // the pipeline list, and whether a draw has used it yet (AheadDrawn).
    uint64_t vsHash = 0, psHash = 0, declHash = 0;
    bool drawn = false;
    bool ahead = false; // built by a worker (vk_legacy_ahead.cpp)
};

// A pixel shader module a pipeline needs and nobody has built yet: how to
// build it (PixelModule in vk_legacy_draw.cpp) and where the shader keeps it.
struct PixelModuleRecipe
{
    bool direct = false;          // the stages link as they stand: no vertex shader passed
    bool plain = false;           // nothing fixed either: Shader::module, else Shader::linked
    uint64_t moduleKey = 0;       // Shader::linked's key
    bool fixDepth = false;        // depthCompare is passed
    uint32_t depthCompare = 0;
    bool fixAlpha = false;        // alphaCompare is passed
    uint32_t alphaCompare = 0;
};

// The create info of a graphics pipeline and everything it points at
// (MakePipelineInfo in vk_legacy_draw.cpp). It points into itself: neither
// copied nor moved.
struct PipelineInfo
{
    PipelineInfo() = default;
    PipelineInfo(const PipelineInfo &) = delete;
    PipelineInfo &operator=(const PipelineInfo &) = delete;

    VkPipelineShaderStageCreateInfo stages[2]{};
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkVertexInputBindingDescription> bindings;
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    VkPipelineViewportStateCreateInfo viewport{};
    VkPipelineRasterizationStateCreateInfo raster{};
    VkPipelineMultisampleStateCreateInfo multisample{};
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    VkPipelineColorBlendAttachmentState blends[kMaxRenderTargets]{};
    VkPipelineColorBlendStateCreateInfo blend{};
    VkPipelineDynamicStateCreateInfo dynamic{};
    VkFormat colorFormats[kMaxRenderTargets]{};
    VkPipelineRenderingCreateInfo rendering{};
    VkGraphicsPipelineCreateInfo info{};
    uint32_t bindingMask = 0; // Pipeline::bindings
};
// ---- The device ------------------------------------------------------------------------

struct Ahead; // the pipeline list and its workers (vk_legacy_ahead.cpp)

constexpr uint32_t kVsFloatRegisters = 256;
constexpr uint32_t kPsFloatRegisters = 224;
constexpr uint32_t kVertexSamplers = 4;
// Device::textures and samplerStates: pixel samplers 0-15, then vertex samplers 0-3.
constexpr uint32_t kSamplerSlots = ls::kSamplerCount + kVertexSamplers;
// The vertex binding that feeds shader inputs the declaration lacks.
constexpr uint32_t kZeroBinding = kMaxStreams;
constexpr uint32_t kZeroBufferBytes = 4096;

struct StreamBinding
{
    VertexBuffer *buffer = nullptr;
    uint32_t offset = 0;
    uint32_t stride = 0;
    // DrawPrimitiveUP: transient memory instead of a buffer.
    VkBuffer transient = VK_NULL_HANDLE;
    VkDeviceSize transientOffset = 0;
};

// What the frame's memory holds for one set 0 binding. A draw whose block
// would get the same bytes points at them again instead of writing a copy:
// most draws change the vertex shader's constants and nothing else.
struct UniformSlot
{
    uint64_t frame = 0;                // Device::frameSerial of the upload
    VkBuffer buffer = VK_NULL_HANDLE;  // the transient buffer it is in
    VkDeviceSize offset = 0;
    const uint8_t *data = nullptr;     // the bytes, for Device::verifyShortcuts
    uint32_t size = 0;                 // bytes held; 0: nothing to use again
    // What the bytes were built from, for the blocks that are not a copy of
    // the float constants (those are let go by SetVertexShaderConstantF and
    // SetPixelShaderConstantF instead): UniformKey in vk_legacy_draw.cpp.
    uint64_t key = 0;
    uint32_t shader = 0;               // Shader::id the key belongs to, or 0
};

// A set 0 bound with its dynamic offsets (none for plain uniform buffers).
struct UniformBind
{
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t offsetCount = 0;
    uint32_t offsets[kMaxUniformBlocks] = {};
};

// A vertex buffer bound to one binding of the command buffer.
struct VertexBind
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize stride = 0;
};

// Pipeline state set with vkCmdSet*, remembered to skip repeats.
struct DynamicState
{
    VkViewport viewport{};
    VkRect2D scissor{};
    float depthBias = 0.0f, depthSlope = 0.0f;
    uint32_t depthBiasEnable = 0;
    uint32_t cullMode = 0;
    uint32_t topology = 0;
    uint32_t depthTest = 0, depthWrite = 0, depthCompare = 0;
    uint32_t stencilTest = 0;
    uint32_t frontFail = 0, frontPass = 0, frontDepthFail = 0, frontCompare = 0;
    uint32_t backFail = 0, backPass = 0, backDepthFail = 0, backCompare = 0;
    uint32_t stencilRef = 0, stencilMask = 0, stencilWriteMask = 0;
    float blendConstants[4] = {};
    bool operator==(const DynamicState &other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
};

struct Device
{
    Core *core = nullptr;
    const Dispatch *vk = nullptr;
    VkDevice dev = VK_NULL_HANDLE;
    DeviceDesc desc{};
    std::string shaderCachePath; // desc.shaderCachePath, kept
    std::string error;
    DeviceStats stats{};
    uint32_t nextId = 1;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED; // the core's depth-stencil format
    float depthBiasScale = 1.0f;                 // D3DRS_DEPTHBIAS to Vulkan's constant factor
    uint32_t uniformAlignment = 16;
    bool dynamicUniforms = true; // !OptionalFeatures::staticUniforms

    VkDescriptorSetLayout emptySet = VK_NULL_HANDLE; // fills set 0 when a pair reads textures only
    std::unordered_map<LayoutKey, std::unique_ptr<Layout>, LayoutKeyHash> layouts;
    std::unordered_map<PipelineKey, Pipeline, PipelineKeyHash> pipelines;
    // Live shaders and declarations by content hash, oldest first: those with
    // the same code or elements share one pipeline id (Shader::pipelineId),
    // and the pipelines keyed by it go when the last of them does.
    std::unordered_map<uint64_t, std::vector<Shader *>> sameShaders;
    std::unordered_map<uint64_t, std::vector<VertexDecl *>> sameDecls;
    // The last draw's pipeline and key: a draw with the same key skips the
    // hash lookup. Pipelines erased from the map clear it.
    Pipeline *lastPipeline = nullptr;
    PipelineKey lastPipelineKey{};

    // Stand-ins for missing vertex inputs and unbound textures.
    Buffer *zeroVertices = nullptr;
    Texture *nullTexture[3] = {}; // 2D, cube, volume (TextureKind order)

    // Present swaps the two textures; the surfaces keep their addresses.
    // With a multisampled back buffer (backBufferSamples over 1) it resolves
    // the back buffer into the front buffer instead, which has one sample,
    // and the two stay what they are.
    Texture *backBuffer = nullptr;
    Surface backBufferSurface;
    Texture *frontBuffer = nullptr;
    Surface frontBufferSurface;
    uint32_t backBufferSamples = 1;
    // A one-sample render target of a multisampled surface's size and
    // format: what StretchRect resolves into before it scales or converts,
    // and what GetRenderTargetData reads. Made at first need, replaced when
    // another size or format is needed.
    Texture *resolveScratch = nullptr;

    // D3D9 state.
    uint32_t renderStates[d3d::RS_COUNT] = {};
    uint32_t samplerStates[kSamplerSlots][d3d::SAMP_COUNT] = {};
    Texture *textures[kSamplerSlots] = {};
    // Built from samplerStates and what of the texture it was built for
    // matters (SamplerTraits); null: stale. A texture with the same traits
    // takes the slot's sampler as it is.
    VkSampler samplers[kSamplerSlots] = {};
    uint8_t samplerTraits[kSamplerSlots] = {};
    Shader *vertexShader = nullptr;
    Shader *pixelShader = nullptr;
    VertexDecl *decl = nullptr;
    StreamBinding streams[kMaxStreams];
    IndexBuffer *indices = nullptr;
    float vsConstants[kVsFloatRegisters][4] = {};
    float psConstants[kPsFloatRegisters][4] = {};
    Viewport viewport{};
    Rect scissor{};
    Surface *renderTargets[kMaxRenderTargets] = {};
    Surface *depthStencil = nullptr;

    // Recording.
    bool frameOpen = false;    // core BeginFrame done, Present not yet
    uint64_t frameSerial = 0;  // counts opened frames; tells cached descriptor sets apart
    bool frameSkipped = false; // BeginFrame skipped: drop drawing until Present
    bool passOpen = false;
    // Nothing recorded in the open pass yet, and the clears its begin does: a
    // clear of the whole target before any draw begins the pass again with
    // it (see Clear).
    bool passEmpty = false;
    struct PassLoads
    {
        bool clearColor = false;
        float color[4] = {};
        bool clearDepth = false; // depth and stencil
        float depth = 1.0f;
        uint32_t stencil = 0;
    } passLoads;
    Surface *passTargets[kMaxRenderTargets] = {};
    Surface *passDepth = nullptr;
    uint32_t passWidth = 0, passHeight = 0;
    uint32_t passSamples = 1; // of every target of the open pass
    // What the command buffer being recorded (stateRecording, the core's
    // Recording()) has bound and set. A draw records only what differs, and a
    // new command buffer starts with nothing (ForgetRecordingState).
    uint64_t stateRecording = 0;
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    DynamicState dynamic{};
    bool dynamicValid = false;
    VertexBind boundVertices[kMaxStreams + 1]; // by binding; a null buffer: nothing bound
    VkBuffer boundIndices = VK_NULL_HANDLE;
    VkIndexType boundIndexType = VK_INDEX_TYPE_UINT16;
    UniformBind boundUniforms{};               // a null set: nothing bound
    // The texture set and the pipeline layout it was bound with. Binding set
    // 0 with another layout can unbind set 1, so both are forgotten together
    // when a draw's layout is not boundLayout.
    VkPipelineLayout boundLayout = VK_NULL_HANDLE;
    VkDescriptorSet boundTextureSet = VK_NULL_HANDLE;
    // What the last draw worked out from the D3D9 state, and whether that
    // state has changed since. A draw whose inputs are all as they were takes
    // lastPipeline and `dynamic` as they are instead of building and
    // comparing them again. Every function that changes an input says so:
    // render states stale both, shaders and the declaration the key, the
    // viewport and scissor the dynamic state, a pass both (its targets are in
    // the key, its size and depth surface in the dynamic state).
    bool keyStale = true;
    bool dynamicStale = true;
    // Checks every shortcut a draw takes (these two, and uniform blocks kept
    // from an earlier draw) against the full work, and fails the device when
    // they differ. On whenever the validation layer is, so the check
    // programs and any run made to hunt a drawing bug have it.
    bool verifyShortcuts = false;
    // SetKeepDrawState. Off: every draw starts with keyStale and dynamicStale
    // set, no uniform block and no vertex buffer, index buffer or descriptor
    // set bound (DropKeptDrawState), so it takes none of the shortcuts.
    bool keepDrawState = true;
    // Uniform blocks in this frame's memory, by set 0 binding, and the
    // transient buffer the newest of them went to. With dynamic uniforms a
    // set 0 points at one buffer, so only blocks in that buffer are used again.
    UniformSlot uniformSlots[32];
    VkBuffer uniformBuffer = VK_NULL_HANDLE;
    // Feeding the GPU (FeedGpu): the setting, the draws recorded in command
    // buffer feedRecording, and the count at which to look at the GPU again.
    GpuFeed feed{};
    uint64_t feedRecording = 0;
    uint32_t feedDraws = 0;
    uint32_t feedCheckAt = 0;
    // The idle rule: the draws its next part of frame feedIdleFrame needs.
    uint64_t feedIdleFrame = 0;
    uint32_t feedIdleNeed = 0;
    // Occlusion queries between BEGIN and END: the draws recorded now count
    // for each of them (CountDraw).
    std::vector<Query *> countingQueries;
    // The texture descriptor set the last draw wrote, and what it holds. A
    // draw that needs the same views and samplers in a set of the same layout
    // binds it again instead of allocating and writing a new one. Sets come
    // from the frame's descriptor pool, so it is forgotten when a frame opens.
    struct TextureSetCache
    {
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        uint32_t mask = 0;
        VkImageView views[kSamplerSlots] = {};
        VkSampler samplers[kSamplerSlots] = {};
    } lastTextureSet;
    // With the kept draw state, texture sets last across frames: found by
    // their contents in this open-addressed table (slots hold an index + 1
    // into entries, 0 when empty). A set holds view handles, which can be
    // handed out again once their image is destroyed, so every lasting set,
    // texture and uniform, is given up when the core's ImagesDestroyed has
    // moved on from imagesDestroyed, or when there are too many; pipelines'
    // lasting sets 0 count only while their generation is lastingGeneration.
    struct LastingTextureSets
    {
        std::vector<TextureSetCache> entries;
        std::vector<uint32_t> slots;
        uint64_t imagesDestroyed = 0;
    } lastingTextures;
    uint64_t lastingGeneration = 1;
    uint32_t warnedOnce = 0; // bit per one-time warning
    // The pipeline list and the workers that build pipelines ahead
    // (vk_legacy_ahead.cpp). Null until StartAhead.
    Ahead *ahead = nullptr;
};

// vk_legacy_device.cpp
bool Fail(Device &device, const char *format, ...);
void Log(Device &device, const char *format, ...);
// "off", "after every draw", "every 128 draws" or "every 128 draws (16 when
// the GPU has nothing left)": a feed setting as the log lines word it.
void GpuFeedText(const GpuFeed &feed, char *text, size_t size);
// One-time warnings (Device::warnedOnce bits).
enum Warning : uint32_t
{
    WarnFixedFunction,
    WarnFeedback,
    WarnNoTargets,
    WarnUnboundStream,
    WarnSystemMemTexture,
    WarnRenderTargetGap,
    WarnStretchRect,
    WarnReadBackLayout,
    WarnSampleMismatch,
    WarnStretchToSamples,
};
// One message per kind for the device's life (bit 0-31).
void WarnOnce(Device &device, uint32_t bit, const char *format, ...);
bool Usable(Device &device);

// vk_legacy_resources.cpp
// samples over 1 makes a multisampled render target or depth surface; the
// count is one the device has (UsableSamples).
Texture *CreateTextureInternal(Device &device, const TextureDesc &desc, uint32_t extraUsage, uint32_t samples = 1);
void DestroyTextureNow(Device &device, Texture *texture);
// The GPU buffer and offset a draw reads, marking it used by this submission.
VkBuffer DrawBuffer(Device &device, GpuBuffer &buffer);
uint32_t LevelSize(uint32_t size, uint32_t level);
// Bytes of one level of one face, as Subresource::cpu holds it: rows of
// blocks tightly packed, rows a slice, depth slices.
void LevelLayout(const Texture &texture, uint32_t level, uint32_t &rowBytes, uint32_t &rows, uint32_t &depth);
// Copies regions of one level from data, the whole level laid out as
// Subresource::cpu, to the texture's image in one copy command (one texture
// upload in the statistics). The regions are in texels, cover whole blocks of
// a compressed format, and do not overlap.
bool UploadRegions(Device &device, Texture &texture, uint32_t face, uint32_t level, const Box *boxes,
                   uint32_t count, const uint8_t *data);
// Adds add to boxes so that none overlaps: a box it overlaps is merged into
// it, again until none does.
void AddBoxApart(std::vector<Box> &boxes, Box add);

// vk_legacy_draw.cpp
// Begins the core's frame on first use; false while the frame is skipped.
bool EnsureFrame(Device &device);
void EndPassIfOpen(Device &device);
// The command buffer the bound state belonged to is gone: the next draw
// binds and sets everything.
void ForgetRecordingState(Device &device);
// Forgets every reference to a surface or texture that goes away.
void ForgetTexture(Device &device, Texture *texture);
// Destroys the pipelines built with a shader or declaration that goes away.
void ForgetPipelines(Device &device, uint32_t id);
bool CreatePipelineObjects(Device &device);
void DestroyPipelineObjects(Device &device);
// Index into Device::textures and samplerStates for a D3D9 sampler number; -1 for none.
int SamplerSlot(uint32_t sampler);
void ResetState(Device &device);

// Pipelines (vk_legacy_draw.cpp), shared with the workers that build them
// ahead. LayoutFor and GetLayout run on the device's thread.
LayoutKey LayoutFor(const Shader &vs, const Shader &ps);
Layout *GetLayout(Device &device, const LayoutKey &key);
// The pixel module a pipeline of this key needs: the one the shader keeps, or
// null with how to build it in recipe.
VkShaderModule FindPixelModule(Device &device, Shader &ps, const Shader &vs, uint32_t depthSamplers,
                               uint32_t alphaCompare, PixelModuleRecipe &recipe);
// Builds that module's SPIR-V. Reads only the two shaders' translations, which
// never change: safe on any thread.
bool BuildPixelSpirv(const PixelModuleRecipe &recipe, const Shader &ps, const Shader &vs,
                     std::vector<uint32_t> &spirv, std::string &error);
// Gives the shader a module built from recipe to keep; false (and the module
// is left to the caller) when it already keeps one there.
bool KeepPixelModule(Device &device, Shader &ps, const PixelModuleRecipe &recipe, VkShaderModule module);
// Fills info for a pipeline of key. Reads only its arguments: safe on any
// thread while they live.
void MakePipelineInfo(PipelineInfo &info, const PipelineKey &key, const Shader &vs, VkShaderModule fragment,
                      const VertexDecl &decl, const Layout &layout, bool dynamicStride);
// Adds a built pipeline to Device::pipelines (it must not be there yet).
Pipeline *AddPipeline(Device &device, const PipelineKey &key, VkPipeline handle, Layout *layout,
                      uint32_t bindings, const Shader &vs, const Shader &ps, const VertexDecl &decl);

// vk_legacy_ahead.cpp: the pipeline list and the workers that build pipelines
// ahead (vk_legacy.h, "Pipelines built ahead"). Everything here runs on the
// device's thread; the workers take only what these hand them.
void StartAhead(Device &device, const char *listPath);
// Stops the workers, takes what they built and writes the list.
void StopAhead(Device &device);
void AheadShaderCreated(Device &device, Shader &shader);
void AheadDeclCreated(Device &device, VertexDecl &decl);
// Before a shader or declaration goes: no worker uses it from here on, and
// what they built with it is in Device::pipelines (for ForgetPipelines).
void AheadForget(Device &device, uint32_t id, uint64_t contentHash, bool shader);
// A draw found no pipeline for key in Device::pipelines: the one a worker has
// built or is building for it (waiting for that), else null.
Pipeline *AheadFind(Device &device, const PipelineKey &key);
// The first draw to use a pipeline: lists it, or marks its entry drawn.
void AheadDrawn(Device &device, const PipelineKey &key, Pipeline &pipeline);
uint64_t ContentHash(const void *data, size_t bytes, uint64_t hash = 14695981039346656037ull);

// vk_legacy_shader_cache.cpp: translated shaders by their bytecode, shared by
// every device in the process (the file side is public, in vk_legacy.h).
struct CachedShader
{
    ls::Shader translated;
    std::vector<uint32_t> spirv; // a vertex shader's module (depth compare fixed off); empty for pixel shaders
};
bool FindCachedShader(const void *bytecode, size_t bytes, CachedShader &out);
void AddCachedShader(const void *bytecode, size_t bytes, CachedShader shader);
bool ShaderCacheLoadedFrom(const char *path);
} // namespace gfx_vk::legacy

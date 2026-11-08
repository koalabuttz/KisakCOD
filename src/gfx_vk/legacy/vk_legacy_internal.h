#pragma once

// Vulkan Legacy adapter internals shared by its .cpp files. Users include
// vk_legacy.h only.

#include "vk_legacy.h"

#include <gfx_vk/shader/vk_legacy_shader.h>

#include <algorithm>
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
    Box box{};                // the locked region, in texels
};

struct Texture
{
    TextureDesc desc{};
    const FormatInfo *format = nullptr;
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    Image *image = nullptr; // null for SYSTEMMEM and SCRATCH textures
    uint32_t faces = 1;
    std::vector<Subresource> subresources;          // face * levels + level
    std::vector<std::unique_ptr<Surface>> surfaces; // GetSurfaceLevel views, same index
    std::string name;
};

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
    std::unordered_map<uint32_t, VkShaderModule> linked; // pixel shader modules by vertex shader id
};

struct VertexDecl
{
    uint32_t id = 0;
    std::vector<VertexElement> elements;
};

struct Query
{
    QueryKind kind = QueryKind::Event;
    uint64_t serial = 0; // submission that contains its end; 0: never issued
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
    uint8_t pad[2] = {};
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
    VkDescriptorSetLayout textureSet = VK_NULL_HANDLE; // null when no samplers
    VkPipelineLayout pipeline = VK_NULL_HANDLE;
};

struct Pipeline
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    Layout *layout = nullptr;
    uint32_t bindings = 0; // bit per vertex binding it reads (streams 0-15, kZeroBinding)
};

// ---- The device ------------------------------------------------------------------------

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
    std::string error;
    DeviceStats stats{};
    uint32_t nextId = 1;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED; // the core's depth-stencil format
    float depthBiasScale = 1.0f;                 // D3DRS_DEPTHBIAS to Vulkan's constant factor
    uint32_t uniformAlignment = 16;

    VkDescriptorSetLayout emptySet = VK_NULL_HANDLE; // fills set 0 when a pair reads textures only
    std::unordered_map<LayoutKey, std::unique_ptr<Layout>, LayoutKeyHash> layouts;
    std::unordered_map<PipelineKey, Pipeline, PipelineKeyHash> pipelines;

    // Stand-ins for missing vertex inputs and unbound textures.
    Buffer *zeroVertices = nullptr;
    Texture *nullTexture[3] = {}; // 2D, cube, volume (TextureKind order)

    Texture *backBuffer = nullptr;
    Surface backBufferSurface;

    // D3D9 state.
    uint32_t renderStates[d3d::RS_COUNT] = {};
    uint32_t samplerStates[kSamplerSlots][d3d::SAMP_COUNT] = {};
    Texture *textures[kSamplerSlots] = {};
    VkSampler samplers[kSamplerSlots] = {}; // built from samplerStates and the bound texture; null: stale
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
    bool frameSkipped = false; // BeginFrame skipped: drop drawing until Present
    bool passOpen = false;
    Surface *passTargets[kMaxRenderTargets] = {};
    Surface *passDepth = nullptr;
    uint32_t passWidth = 0, passHeight = 0;
    VkCommandBuffer stateCommands = VK_NULL_HANDLE; // the command buffer boundPipeline and dynamic belong to
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    DynamicState dynamic{};
    bool dynamicValid = false;
    uint32_t warnedOnce = 0; // bit per one-time warning
};

// vk_legacy_device.cpp
bool Fail(Device &device, const char *format, ...);
void Log(Device &device, const char *format, ...);
// One message per kind for the device's life (bit 0-31).
void WarnOnce(Device &device, uint32_t bit, const char *format, ...);
bool Usable(Device &device);

// vk_legacy_resources.cpp
Texture *CreateTextureInternal(Device &device, const TextureDesc &desc, uint32_t extraUsage);
void DestroyTextureNow(Device &device, Texture *texture);
// The GPU buffer and offset a draw reads, marking it used by this submission.
VkBuffer DrawBuffer(Device &device, GpuBuffer &buffer);
uint32_t LevelSize(uint32_t size, uint32_t level);

// vk_legacy_draw.cpp
// Begins the core's frame on first use; false while the frame is skipped.
bool EnsureFrame(Device &device);
void EndPassIfOpen(Device &device);
// Forgets every reference to a surface or texture that goes away.
void ForgetTexture(Device &device, Texture *texture);
// Destroys the pipelines built with a shader or declaration that goes away.
void ForgetPipelines(Device &device, uint32_t id);
bool CreatePipelineObjects(Device &device);
void DestroyPipelineObjects(Device &device);
// Index into Device::textures and samplerStates for a D3D9 sampler number; -1 for none.
int SamplerSlot(uint32_t sampler);
void ResetState(Device &device);
} // namespace gfx_vk::legacy

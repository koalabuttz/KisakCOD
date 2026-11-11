// Vulkan Legacy adapter: the device, messages and the back buffer. See vk_legacy.h.

#include "vk_legacy_internal.h"

#include <cstdio>
#include <cstring>

namespace gfx_vk::legacy
{
namespace
{
void VLog(Device &device, const char *prefix, const char *format, va_list args)
{
    if (!device.desc.log)
        return;
    char line[1024];
    const int used = std::snprintf(line, sizeof(line), "%s", prefix);
    std::vsnprintf(line + used, sizeof(line) - used, format, args);
    device.desc.log(device.desc.logUser, line);
}

// The back buffer and the front buffer: two render targets that Present
// swaps (see FrontBuffer in vk_legacy.h), or with a multisampled back buffer
// a multisampled one that Present resolves into a one-sample front buffer.
bool CreateBackBuffer(Device &device, uint32_t width, uint32_t height)
{
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.levels = 1;
    desc.usage = d3d::USAGE_RENDERTARGET;
    desc.format = d3d::FMT_A8R8G8B8;
    desc.pool = d3d::POOL_DEFAULT;
    desc.name = "back buffer";
    device.backBuffer = CreateTextureInternal(device, desc, 0, device.backBufferSamples);
    if (!device.backBuffer)
        return Fail(device, "cannot create the %ux%u back buffer with %u samples", width, height,
                    device.backBufferSamples);
    desc.name = "front buffer";
    device.frontBuffer = CreateTextureInternal(device, desc, 0);
    if (!device.frontBuffer)
        return Fail(device, "cannot create the %ux%u front buffer", width, height);
    device.backBufferSurface = Surface{device.backBuffer, 0, 0, false};
    device.frontBufferSurface = Surface{device.frontBuffer, 0, 0, false};
    return true;
}

void DestroyBackBuffer(Device &device)
{
    for (Texture **texture : {&device.backBuffer, &device.frontBuffer, &device.resolveScratch})
    {
        ForgetTexture(device, *texture);
        DestroyTextureNow(device, *texture);
        *texture = nullptr;
    }
    device.backBufferSurface.texture = nullptr;
    device.frontBufferSurface.texture = nullptr;
}

// 1x1 textures of each kind, (0, 0, 0, 1) as D3D9 samples nothing bound, for
// samplers with nothing bound. Shaders translated with the legacy state fixed
// sample them as they are (no isNull test).
bool CreateNullTextures(Device &device)
{
    static const char *const names[3] = {"null 2D texture", "null cube texture", "null volume texture"};
    for (uint32_t kind = 0; kind < 3; ++kind)
    {
        TextureDesc desc;
        desc.kind = static_cast<TextureKind>(kind);
        desc.levels = 1;
        desc.format = d3d::FMT_A8R8G8B8;
        desc.pool = d3d::POOL_MANAGED;
        desc.name = names[kind];
        Texture *texture = CreateTextureInternal(device, desc, 0);
        if (!texture)
            return Fail(device, "cannot create the %s", names[kind]);
        device.nullTexture[kind] = texture;
        for (uint32_t face = 0; face < texture->faces; ++face)
        {
            ImageRegion region;
            region.layer = face;
            region.width = region.height = region.depth = 1;
            void *data = StageImageWrite(device.core, texture->image, region, nullptr, nullptr);
            if (!data)
                return Fail(device, "cannot clear the %s: %s", names[kind], LastError(device.core));
            const uint8_t black[4] = {0, 0, 0, 0xff}; // A8R8G8B8: B, G, R, A
            std::memcpy(data, black, sizeof(black));
        }
    }
    return true;
}

void DefaultStates(Device &device)
{
    // d3d9's defaults for the states the adapter reads.
    uint32_t *rs = device.renderStates;
    rs[d3d::RS_ZENABLE] = 1;
    rs[d3d::RS_FILLMODE] = d3d::FILL_SOLID;
    rs[d3d::RS_ZWRITEENABLE] = 1;
    rs[d3d::RS_SRCBLEND] = d3d::BLEND_ONE;
    rs[d3d::RS_DESTBLEND] = d3d::BLEND_ZERO;
    rs[d3d::RS_CULLMODE] = d3d::CULL_CCW;
    rs[d3d::RS_ZFUNC] = d3d::CMP_LESSEQUAL;
    rs[d3d::RS_ALPHAFUNC] = d3d::CMP_ALWAYS;
    rs[d3d::RS_STENCILFAIL] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_STENCILZFAIL] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_STENCILPASS] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_STENCILFUNC] = d3d::CMP_ALWAYS;
    rs[d3d::RS_STENCILMASK] = 0xffffffffu;
    rs[d3d::RS_STENCILWRITEMASK] = 0xffffffffu;
    rs[d3d::RS_TEXTUREFACTOR] = 0xffffffffu;
    rs[d3d::RS_MULTISAMPLEANTIALIAS] = 1;
    rs[d3d::RS_COLORWRITEENABLE] = 0xf;
    rs[d3d::RS_BLENDOP] = d3d::BLENDOP_ADD;
    rs[d3d::RS_CCW_STENCILFAIL] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_CCW_STENCILZFAIL] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_CCW_STENCILPASS] = d3d::STENCILOP_KEEP;
    rs[d3d::RS_CCW_STENCILFUNC] = d3d::CMP_ALWAYS;
    rs[d3d::RS_COLORWRITEENABLE1] = 0xf;
    rs[d3d::RS_COLORWRITEENABLE2] = 0xf;
    rs[d3d::RS_COLORWRITEENABLE3] = 0xf;
    rs[d3d::RS_BLENDFACTOR] = 0xffffffffu;
    rs[d3d::RS_SRCBLENDALPHA] = d3d::BLEND_ONE;
    rs[d3d::RS_DESTBLENDALPHA] = d3d::BLEND_ZERO;
    rs[d3d::RS_BLENDOPALPHA] = d3d::BLENDOP_ADD;
    for (uint32_t slot = 0; slot < kSamplerSlots; ++slot)
    {
        uint32_t *ss = device.samplerStates[slot];
        ss[d3d::SAMP_ADDRESSU] = d3d::TADDRESS_WRAP;
        ss[d3d::SAMP_ADDRESSV] = d3d::TADDRESS_WRAP;
        ss[d3d::SAMP_ADDRESSW] = d3d::TADDRESS_WRAP;
        ss[d3d::SAMP_MAGFILTER] = d3d::TEXF_POINT;
        ss[d3d::SAMP_MINFILTER] = d3d::TEXF_POINT;
        ss[d3d::SAMP_MIPFILTER] = d3d::TEXF_NONE;
        ss[d3d::SAMP_MAXANISOTROPY] = 1;
    }
}
} // namespace

bool Fail(Device &device, const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (device.error.empty())
        device.error = line;
    if (device.desc.log)
    {
        std::string message = std::string("legacy: error: ") + line;
        device.desc.log(device.desc.logUser, message.c_str());
    }
    return false;
}

void Log(Device &device, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    VLog(device, "legacy: ", format, args);
    va_end(args);
}

void GpuFeedText(const GpuFeed &feed, char *text, size_t size)
{
    if (!feed.on)
        std::snprintf(text, size, "off");
    else if (feed.always)
        std::snprintf(text, size, "after every draw");
    else if (feed.idleDraws && feed.idleDraws < feed.draws)
        std::snprintf(text, size, "every %u draws (%u when the GPU has nothing left)", feed.draws, feed.idleDraws);
    else
        std::snprintf(text, size, "every %u draws", feed.draws);
}

void WarnOnce(Device &device, uint32_t bit, const char *format, ...)
{
    if (device.warnedOnce & (1u << bit))
        return;
    device.warnedOnce |= 1u << bit;
    va_list args;
    va_start(args, format);
    VLog(device, "legacy: warning: ", format, args);
    va_end(args);
}

bool Usable(Device &device)
{
    if (!device.error.empty())
        return false;
    const char *coreError = LastError(device.core);
    if (coreError && coreError[0])
        return Fail(device, "core: %s", coreError);
    return true;
}

Device *CreateDevice(Core *core, const DeviceDesc &desc, std::string &error)
{
    std::unique_ptr<Device> device(new Device());
    device->core = core;
    device->vk = &Functions(core);
    device->dev = DeviceHandle(core);
    device->desc = desc;
    // Before any shader is translated or the cache is read, which follows it.
    ls::SetStrictFloats(Features(core).strictFloats);
    ls::SetDynamicLegacyState(Features(core).dynamicLegacyState);
    if (desc.shaderCachePath && *desc.shaderCachePath)
    {
        device->shaderCachePath = desc.shaderCachePath;
        // Pointers in desc are the caller's; keep none of them.
        device->desc.shaderCachePath = nullptr;
        if (!ShaderCacheLoadedFrom(desc.shaderCachePath))
        {
            std::string message;
            LoadShaderCache(desc.shaderCachePath, message);
            Log(*device, "shader cache %s: %s", desc.shaderCachePath, message.c_str());
        }
    }
    device->depthFormat = DepthStencilFormat(core);
    // D3DRS_DEPTHBIAS is in units of the depth range; Vulkan's constant factor
    // is in units of the format's smallest step.
    device->depthBiasScale = device->depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ? float(1u << 24) : float(1u << 23);
    device->dynamicUniforms = !Features(core).staticUniforms;
    device->uniformAlignment = static_cast<uint32_t>(std::max<VkDeviceSize>(16, Limits(core).minUniformBufferOffsetAlignment));
    device->verifyShortcuts = ValidationEnabled(core);
    device->backBufferSamples = UsableSamples(device.get(), desc.backBufferSamples);
    if (device->backBufferSamples != std::max(desc.backBufferSamples, 1u))
        Log(*device, "a back buffer of %u samples is made with %u: the device has no %u", desc.backBufferSamples,
            device->backBufferSamples, desc.backBufferSamples);
    DefaultStates(*device);

    uint32_t width = desc.width ? desc.width : Swapchain(core).width;
    uint32_t height = desc.height ? desc.height : Swapchain(core).height;
    if (!width || !height)
    {
        width = 640;
        height = 480;
    }

    BufferDesc zero;
    zero.size = kZeroBufferBytes;
    zero.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    zero.name = "legacy zero vertices";
    device->zeroVertices = CreateBuffer(core, zero);
    void *zeroData = device->zeroVertices ? StageBufferWrite(core, device->zeroVertices, 0, kZeroBufferBytes) : nullptr;
    if (zeroData)
        std::memset(zeroData, 0, kZeroBufferBytes);
    else
        Fail(*device, "cannot create the zero vertex buffer: %s", LastError(core));

    if (device->error.empty() && CreatePipelineObjects(*device) && CreateNullTextures(*device)
        && CreateBackBuffer(*device, width, height))
    {
        ResetState(*device);
        if (Flush(core, false))
        {
            const bool packedTargets = !Features(device->core).forceWideRenderTargets
                                       && FormatSupports(device->core, VK_FORMAT_R5G6B5_UNORM_PACK16,
                                                         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                                                             | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
            // How a frame reaches the GPU (FeedGpu in vk_legacy_draw.cpp).
            const OptionalFeatures &features = Features(device->core);
            device->feed.on = features.gpuFeed;
            device->feed.always = features.gpuFeedAlways;
            device->feed.draws = std::max(1u, features.gpuFeedDraws);
            device->feed.idleDraws = 0;
            char feed[64];
            GpuFeedText(device->feed, feed, sizeof(feed));
            Log(*device,
                "device ready: back buffer %ux%u, %u %s, depth %s, vertex stride %s, depth compare %s, "
                "16-bit targets %s, occlusion queries %s, clears %s, passes %s, floats %s, alpha test %s, "
                "legacy state %s, GPU feed %s",
                width, height, device->backBufferSamples, device->backBufferSamples == 1 ? "sample" : "samples",
                FormatName(device->depthFormat), features.dynamicVertexStride ? "dynamic" : "static",
                features.fixedDepthCompare ? "fixed" : "dynamic", packedTargets ? "packed" : "wide",
                features.occlusionCounts ? "counted" : "not counted",
                features.passClears ? "at pass start" : "in the pass",
                features.passTimestamps ? "timed" : "not timed", features.strictFloats ? "strict" : "fast",
                features.fixedAlphaTest ? "fixed" : "dynamic", features.dynamicLegacyState ? "dynamic" : "fixed",
                feed);
            return device.release();
        }
        Fail(*device, "core: %s", LastError(core));
    }
    error = device->error.empty() ? "device creation failed" : device->error;
    DestroyDevice(device.release());
    return nullptr;
}

void DestroyDevice(Device *device)
{
    if (!device)
        return;
    Core *core = device->core;
    if (device->passOpen && InPass(core))
        EndPass(core);
    device->passOpen = false;
    if (device->frameOpen && InFrame(core))
        gfx_vk::Present(core); // the core needs its frame closed; nothing new is shown
    device->frameOpen = false;
    gfx_vk::WaitIdle(core);
    DestroyPipelineObjects(*device);
    for (Texture *&texture : device->nullTexture)
    {
        DestroyTextureNow(*device, texture);
        texture = nullptr;
    }
    DestroyBackBuffer(*device);
    DestroyBuffer(core, device->zeroVertices);
    gfx_vk::WaitIdle(core);
    if (!device->shaderCachePath.empty())
    {
        std::string message;
        SaveShaderCache(device->shaderCachePath.c_str(), message);
        Log(*device, "shader cache %s: %s", device->shaderCachePath.c_str(), message.c_str());
    }
    delete device;
}

const char *DeviceError(const Device *device) { return device->error.c_str(); }

const DeviceStats &Stats(const Device *device) { return device->stats; }

GpuFeed GetGpuFeed(const Device *device) { return device->feed; }

void SetGpuFeed(Device *device, const GpuFeed &feed)
{
    GpuFeed set = feed;
    set.draws = std::max(1u, set.draws);
    const GpuFeed &old = device->feed;
    if (set.on == old.on && set.always == old.always && set.draws == old.draws && set.idleDraws == old.idleDraws)
        return;
    device->feed = set;
    // The draws counted so far were counted for the old setting.
    device->feedRecording = 0;
    device->feedIdleFrame = 0;
    char text[64];
    GpuFeedText(set, text, sizeof(text));
    Log(*device, "GPU feed now %s", text);
}

bool GetKeepDrawState(const Device *device) { return device->keepDrawState; }

void SetKeepDrawState(Device *device, bool keep)
{
    if (device->keepDrawState == keep)
        return;
    // Nothing to let go of either way: what a draw leaves is right for the
    // state it was made from whether or not the next draw looks at it.
    device->keepDrawState = keep;
    Log(*device, "draw state kept from one draw to the next: now %s", keep ? "on" : "off");
}

Surface *BackBuffer(Device *device) { return &device->backBufferSurface; }

Surface *FrontBuffer(Device *device) { return &device->frontBufferSurface; }

uint32_t SetBackBufferSamples(Device *device, uint32_t samples)
{
    const uint32_t used = UsableSamples(device, samples);
    if (used != std::max(samples, 1u))
        Log(*device, "a back buffer of %u samples is made with %u: the device has no %u", samples, used, samples);
    device->backBufferSamples = used;
    return used;
}

bool ResizeBackBuffer(Device *device, uint32_t width, uint32_t height)
{
    if (!Usable(*device) || !width || !height)
        return false;
    EndPassIfOpen(*device);
    const bool wasTarget = device->renderTargets[0] == &device->backBufferSurface;
    DestroyBackBuffer(*device);
    if (!CreateBackBuffer(*device, width, height))
        return false;
    if (wasTarget)
        SetRenderTarget(device, 0, &device->backBufferSurface);
    return true;
}

void ResetState(Device *device)
{
    EndPassIfOpen(*device);
    std::memset(device->renderStates, 0, sizeof(device->renderStates));
    std::memset(device->samplerStates, 0, sizeof(device->samplerStates));
    DefaultStates(*device);
    for (uint32_t slot = 0; slot < kSamplerSlots; ++slot)
    {
        device->textures[slot] = nullptr;
        device->samplers[slot] = VK_NULL_HANDLE;
    }
    device->vertexShader = nullptr;
    device->pixelShader = nullptr;
    device->decl = nullptr;
    for (StreamBinding &stream : device->streams)
        stream = StreamBinding{};
    device->indices = nullptr;
    std::memset(device->vsConstants, 0, sizeof(device->vsConstants));
    std::memset(device->psConstants, 0, sizeof(device->psConstants));
    // The frame's uniform blocks were built from the old values.
    for (UniformSlot &slot : device->uniformSlots)
        slot.size = 0;
    device->keyStale = true;
    device->dynamicStale = true;
    for (Surface *&target : device->renderTargets)
        target = nullptr;
    device->depthStencil = nullptr;
    device->dynamicValid = false;
    SetRenderTarget(device, 0, &device->backBufferSurface);
}

void WaitIdle(Device *device)
{
    EndPassIfOpen(*device);
    if (Usable(*device))
        Flush(device->core, true);
}
} // namespace gfx_vk::legacy

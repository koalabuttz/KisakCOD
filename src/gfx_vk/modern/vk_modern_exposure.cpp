// The Modern path's auto exposure: the linear-light scene's average
// luminance, measured on a small grid each scene, and an exposure that moves
// towards the one that brings it to a key value. See vk_modern.h and
// docs/vulkan-modern.md; the shaders are src/gfx_vk/modern/shaders/exposure_*.frag.

#include "vk_modern.h"
#include "vk_modern_internal.h"

#include "vk_modern_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gfx_vk::modern
{
namespace
{
// exposure_measure.frag's push constants (std430).
struct MeasurePush
{
    float from[4];
    float invSceneSize[2];
    float gridSize[2];
    float minLog2;
    float maxLog2;
};
static_assert(sizeof(MeasurePush) == 40, "exposure_measure.frag's push constant block is 40 bytes");

// exposure_reduce.frag's.
struct ReducePush
{
    int32_t block[2];
};

// exposure_adapt.frag's.
struct AdaptPush
{
    int32_t sumsSize[2];
    float log2Key;
    float minEv;
    float maxEv;
    float brighterRate;
    float darkerRate;
    uint32_t reset;
};
static_assert(sizeof(AdaptPush) == 32, "exposure_adapt.frag's push constant block is 32 bytes");

constexpr uint32_t kPushBytes = sizeof(MeasurePush);

// The measured grid, its sums and the 1x1 results.
constexpr uint32_t kGrid = 64;
constexpr uint32_t kBlock = 8;
constexpr uint32_t kSums = kGrid / kBlock;
constexpr VkFormat kGridFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kSumsFormat = VK_FORMAT_R32G32_SFLOAT;
constexpr VkFormat kResultFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

// Each grid tap's log2 luminance is clamped to this, so a patch of sky or a
// flash weighs no more than a patch 16 times brighter than white.
constexpr float kMinLog2 = -12.0f;
constexpr float kMaxLog2 = 4.0f;

// One result in every this many scenes is read back for the statistics.
constexpr uint64_t kReadEvery = 16;
} // namespace

struct Exposure
{
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule measure = VK_NULL_HANDLE;
    VkShaderModule reduce = VK_NULL_HANDLE;
    VkShaderModule adapt = VK_NULL_HANDLE;
    VkPipeline measurePipeline = VK_NULL_HANDLE;
    VkPipeline reducePipeline = VK_NULL_HANDLE;
    VkPipeline adaptPipeline = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    Image *grid = nullptr;
    Image *sums = nullptr;
    // Ping-pong: the scene reads one and writes the other.
    Image *results[2] = {};
    uint32_t current = 0; // the result the last scene wrote
    bool haveResult = false;
    // A result being read back for the statistics, and the submission it is in.
    Readback *reading = nullptr;
    uint64_t readingSerial = 0;
    uint64_t sinceRead = kReadEvery - 1; // the first scene's result is read
    ExposureStats stats{};
};

namespace
{
bool Pass(Core *core, Exposure &exposure, VkPipeline pipeline, Image *source, Image *second, VkSampler sampler,
          Image *target, const void *push, uint32_t pushSize, std::string &error)
{
    const VkDescriptorSet set = AllocateDescriptorSet(core, exposure.setLayout);
    if (!set)
    {
        error = LastError(core);
        return false;
    }
    // Binding 1 is read by the adapt step only; the others get the source again.
    const VkDescriptorImageInfo images[2] = {
        {sampler, ImageView(source), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL},
        {exposure.nearest, ImageView(second ? second : source), VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    Functions(core).vkUpdateDescriptorSets(DeviceHandle(core), 2, writes, 0, nullptr);

    UseImage(core, source, ImageUse::Sampled);
    if (second)
        UseImage(core, second, ImageUse::Sampled);
    const ImageDesc &targetInfo = ImageInfo(target);
    const VkRect2D area = {{0, 0}, {targetInfo.width, targetInfo.height}};
    PassDesc desc;
    desc.name = "modern exposure";
    desc.colorCount = 1;
    desc.color[0].image = target;
    desc.color[0].load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    desc.area = area;
    if (!BeginPass(core, desc))
    {
        error = LastError(core);
        return false;
    }
    uint8_t constants[kPushBytes] = {};
    std::memcpy(constants, push, pushSize);
    const VkPipelineLayout layout = exposure.layout;
    Record(core, [=](VkCommandBuffer cmd, const Dispatch &vk) {
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(area.extent.width),
                                  static_cast<float>(area.extent.height), 0.0f, 1.0f};
        vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vk.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes, constants);
        vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
        vk.vkCmdSetScissor(cmd, 0, 1, &area);
        vk.vkCmdDraw(cmd, 3, 1, 0, 0);
    });
    EndPass(core);
    ++exposure.stats.passes;
    return true;
}

Image *MakeImage(Core *core, VkFormat format, uint32_t size, const char *name, bool readable)
{
    ImageDesc desc;
    desc.format = format;
    desc.width = desc.height = size;
    desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (readable)
        desc.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    desc.name = name;
    return CreateImage(core, desc);
}

// A finished read back of a result goes into the statistics.
void CollectReading(Core *core, Exposure &exposure, bool wait)
{
    if (!exposure.reading)
        return;
    if (!wait && !SerialFinished(core, exposure.readingSerial))
        return;
    ReadbackData data{};
    if (WaitReadback(core, exposure.reading, &data) && data.pixels && data.format == kResultFormat)
    {
        float texel[4];
        std::memcpy(texel, data.pixels, sizeof(texel));
        ExposureStats &stats = exposure.stats;
        if (!stats.samples)
        {
            stats.evMin = stats.evMax = texel[0];
        }
        stats.evMin = std::min(stats.evMin, static_cast<double>(texel[0]));
        stats.evMax = std::max(stats.evMax, static_cast<double>(texel[0]));
        stats.evSum += texel[0];
        stats.log2LuminanceSum += texel[1];
        stats.wantedEvSum += texel[2];
        ++stats.samples;
    }
    DestroyReadback(core, exposure.reading);
    exposure.reading = nullptr;
}
} // namespace

Exposure *CreateExposure(Core *core, std::string &error)
{
    const VkFormatFeatureFlags wanted =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (!FormatSupports(core, kGridFormat, wanted) || !FormatSupports(core, kSumsFormat, wanted)
        || !FormatSupports(core, kResultFormat, wanted))
    {
        error = "the device cannot draw into and sample R16G16_SFLOAT, R32G32_SFLOAT and R32G32B32A32_SFLOAT";
        return nullptr;
    }
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    Exposure *exposure = new Exposure();
    const auto fail = [&](const std::string &reason) {
        error = reason;
        DestroyExposure(core, exposure);
        return nullptr;
    };

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &exposure->setLayout) != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout failed");
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &exposure->setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &exposure->layout) != VK_SUCCESS)
        return fail("vkCreatePipelineLayout failed");
    exposure->vertex = CreateShaderModule(core, shaders::kFullscreenVert, sizeof(shaders::kFullscreenVert));
    exposure->measure = CreateShaderModule(core, shaders::kExposureMeasureFrag, sizeof(shaders::kExposureMeasureFrag));
    exposure->reduce = CreateShaderModule(core, shaders::kExposureReduceFrag, sizeof(shaders::kExposureReduceFrag));
    exposure->adapt = CreateShaderModule(core, shaders::kExposureAdaptFrag, sizeof(shaders::kExposureAdaptFrag));
    if (!exposure->vertex || !exposure->measure || !exposure->reduce || !exposure->adapt)
        return fail(LastError(core));
    std::string reason;
    exposure->measurePipeline =
        CreateFullscreenPipeline(core, exposure->layout, exposure->vertex, exposure->measure, kGridFormat, false, reason);
    if (exposure->measurePipeline)
        exposure->reducePipeline = CreateFullscreenPipeline(core, exposure->layout, exposure->vertex, exposure->reduce,
                                                            kSumsFormat, false, reason);
    if (exposure->reducePipeline)
        exposure->adaptPipeline = CreateFullscreenPipeline(core, exposure->layout, exposure->vertex, exposure->adapt,
                                                           kResultFormat, false, reason);
    if (!exposure->adaptPipeline)
        return fail(reason);

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    exposure->linear = GetSampler(core, sampler);
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    exposure->nearest = GetSampler(core, sampler);
    if (!exposure->linear || !exposure->nearest)
        return fail(LastError(core));

    exposure->grid = MakeImage(core, kGridFormat, kGrid, "modern exposure grid", false);
    exposure->sums = MakeImage(core, kSumsFormat, kSums, "modern exposure sums", false);
    exposure->results[0] = MakeImage(core, kResultFormat, 1, "modern exposure 0", true);
    exposure->results[1] = MakeImage(core, kResultFormat, 1, "modern exposure 1", true);
    if (!exposure->grid || !exposure->sums || !exposure->results[0] || !exposure->results[1])
        return fail(LastError(core));
    return exposure;
}

void DestroyExposure(Core *core, Exposure *exposure)
{
    if (!exposure)
        return;
    if (exposure->reading)
        DestroyReadback(core, exposure->reading);
    for (Image *image : {exposure->grid, exposure->sums, exposure->results[0], exposure->results[1]})
        if (image)
            DestroyImage(core, image);
    DestroyPipeline(core, exposure->measurePipeline);
    DestroyPipeline(core, exposure->reducePipeline);
    DestroyPipeline(core, exposure->adaptPipeline);
    DestroyShaderModule(core, exposure->vertex);
    DestroyShaderModule(core, exposure->measure);
    DestroyShaderModule(core, exposure->reduce);
    DestroyShaderModule(core, exposure->adapt);
    // The layouts are not tracked by the core: wait until no recorded work uses them.
    WaitIdle(core);
    const Dispatch &vk = Functions(core);
    const VkDevice device = DeviceHandle(core);
    if (exposure->layout)
        vk.vkDestroyPipelineLayout(device, exposure->layout, nullptr);
    if (exposure->setLayout)
        vk.vkDestroyDescriptorSetLayout(device, exposure->setLayout, nullptr);
    delete exposure;
}

ExposureStats Stats(Core *core, Exposure *exposure, bool waitForReading)
{
    if (!exposure)
        return ExposureStats{};
    if (core)
        CollectReading(core, *exposure, waitForReading);
    return exposure->stats;
}

void ResetStats(Exposure *exposure)
{
    if (exposure)
        exposure->stats = ExposureStats{};
}

Image *MeasureExposure(Core *core, Exposure *exposure, Image *scene, const VkRect2D &area,
                       const ExposureParams &params, std::string &error)
{
    if (!exposure || !scene)
    {
        error = "no exposure or scene image";
        return nullptr;
    }
    const ImageDesc &sceneInfo = ImageInfo(scene);
    if (sceneInfo.samples != 1 || !(sceneInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
    {
        error = "the scene is multisampled or cannot be sampled";
        return nullptr;
    }
    if (!Inside(area, sceneInfo))
    {
        error = "the area is not inside the scene image";
        return nullptr;
    }
    if (!InFrame(core) || InPass(core))
    {
        error = "not between passes of a frame";
        return nullptr;
    }
    CollectReading(core, *exposure, false);

    MeasurePush measure{};
    measure.from[0] = static_cast<float>(area.offset.x);
    measure.from[1] = static_cast<float>(area.offset.y);
    measure.from[2] = static_cast<float>(area.extent.width);
    measure.from[3] = static_cast<float>(area.extent.height);
    measure.invSceneSize[0] = 1.0f / sceneInfo.width;
    measure.invSceneSize[1] = 1.0f / sceneInfo.height;
    measure.gridSize[0] = measure.gridSize[1] = static_cast<float>(kGrid);
    measure.minLog2 = kMinLog2;
    measure.maxLog2 = kMaxLog2;
    if (!Pass(core, *exposure, exposure->measurePipeline, scene, nullptr, exposure->linear, exposure->grid, &measure,
              sizeof(measure), error))
        return nullptr;

    ReducePush reduce{};
    reduce.block[0] = reduce.block[1] = static_cast<int32_t>(kBlock);
    if (!Pass(core, *exposure, exposure->reducePipeline, exposure->grid, nullptr, exposure->nearest, exposure->sums,
              &reduce, sizeof(reduce), error))
        return nullptr;

    // How far the exposure moves this scene: towards the target with a time
    // constant per direction, so a frame twice as long moves it as two would.
    const float dt = std::clamp(params.seconds, 0.0f, 1.0f);
    const auto rate = [&](float tau) { return tau > 0.0f ? 1.0f - std::exp(-dt / tau) : 1.0f; };
    const bool reset = params.reset || !exposure->haveResult;
    AdaptPush adapt{};
    adapt.sumsSize[0] = adapt.sumsSize[1] = static_cast<int32_t>(kSums);
    adapt.log2Key = std::log2(std::max(params.key, 1e-6f));
    adapt.minEv = std::min(params.minEv, params.maxEv);
    adapt.maxEv = std::max(params.minEv, params.maxEv);
    adapt.brighterRate = rate(params.brighterSeconds);
    adapt.darkerRate = rate(params.darkerSeconds);
    adapt.reset = reset ? 1u : 0u;
    Image *previous = exposure->results[exposure->current];
    Image *next = exposure->results[exposure->current ^ 1u];
    if (!Pass(core, *exposure, exposure->adaptPipeline, exposure->sums, previous, exposure->nearest, next, &adapt,
              sizeof(adapt), error))
        return nullptr;
    exposure->current ^= 1u;
    exposure->haveResult = true;
    ++exposure->stats.scenes;
    if (reset)
        ++exposure->stats.resets;

    // Now and then, a copy of the result for the statistics, collected once
    // its submission has finished (nothing waits for it during play).
    if (!exposure->reading && ++exposure->sinceRead >= kReadEvery)
    {
        exposure->reading = ReadImage(core, next, 0, 0);
        exposure->readingSerial = RecordedSerial(core);
        exposure->sinceRead = 0;
    }
    return next;
}

Image *CurrentExposure(const Exposure *exposure)
{
    return exposure && exposure->haveResult ? exposure->results[exposure->current] : nullptr;
}
} // namespace gfx_vk::modern

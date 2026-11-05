// gfx_vk shared core: instance, device, swapchain and a cleared, presented frame.
// See vk_core.h for the contract and docs/vulkan-core.md for how it is checked.

#include "vk_core.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gfx_vk
{
namespace
{
constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;
constexpr uint32_t kFramesInFlight = 2;
constexpr const char *kValidationLayer = "VK_LAYER_KHRONOS_validation";
// Optional extensions. Portability enumeration lists MoltenVK (macOS) devices;
// the spec requires enabling the portability subset wherever a device offers it.
constexpr const char *kPortabilityEnumeration = "VK_KHR_portability_enumeration";
constexpr const char *kPortabilitySubset = "VK_KHR_portability_subset";
// The stage that hands a swapchain image to the present engine: the last
// barrier's destination and the render-done semaphore's signal stage. They must
// match, or the layout transition to PRESENT_SRC falls outside the semaphore's
// scope and is not ordered before the present (synchronization validation
// reports SYNC-HAZARD-PRESENT-AFTER-WRITE; a destination of NONE does that).
constexpr VkPipelineStageFlags2 kPresentHandOffStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
// Validation messages printed per severity; the rest are only counted.
constexpr uint32_t kValidationMessagesLogged = 20;

struct FrameSlot
{
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    bool timestampsWritten = false;
};

enum class BuildResult
{
    Built,
    Minimised,
    Failed,
};
} // namespace

class Core
{
public:
    CoreDesc desc{};
    WindowSystem window{};
    Dispatch vk{};
    char error[512] = {};
    bool failed = false;

    VkInstance instance = VK_NULL_HANDLE;
    uint32_t instanceApiVersion = 0;
    bool validation = false;
    bool debugUtils = false;
    bool syncValidation = false;
    bool portabilityEnumeration = false;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    DeviceInfo device{};
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t queueFamily = 0;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    SwapchainInfo swapInfo{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> renderDone; // one per swapchain image
    // Rebuild before the next frame: the swapchain is out of date or suboptimal,
    // or there is none yet.
    bool rebuildPending = false;
    // The window may have changed size: rebuild only if the size differs.
    bool resizePending = false;

    FrameSlot frames[kFramesInFlight];
    uint32_t frameIndex = 0;

    VkQueryPool timestamps = VK_NULL_HANDLE;
    double timestampPeriodNs = 0.0;
    uint64_t timestampMask = 0;

    bool captureRequested = false;
    VkBuffer captureBuffer = VK_NULL_HANDLE;
    VkDeviceMemory captureMemory = VK_NULL_HANDLE;
    VkDeviceSize captureSize = 0;
    bool captureCoherent = false;
    void *captureMapped = nullptr;
    std::vector<uint8_t> capturePixels;
    Capture capture{};

    CoreStats stats{};
};

namespace
{
void Log(const Core &core, const char *format, ...)
{
    if (!core.desc.log)
        return;
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    core.desc.log(core.desc.logUser, line);
}

bool Fail(Core &core, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vsnprintf(core.error, sizeof(core.error), format, args);
    va_end(args);
    core.failed = true;
    return false;
}

const char *ResultName(VkResult result)
{
    switch (result)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VkResult (other)";
    }
}

bool Check(Core &core, VkResult result, const char *what)
{
    if (result == VK_SUCCESS)
        return true;
    return Fail(core, "%s failed: %s (%d)", what, ResultName(result), static_cast<int>(result));
}

void FormatVersion(uint32_t version, char *out, size_t size)
{
    std::snprintf(out, size, "%u.%u.%u", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                  VK_API_VERSION_PATCH(version));
}

bool HasExtension(const std::vector<VkExtensionProperties> &list, const char *name)
{
    for (const VkExtensionProperties &extension : list)
    {
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL OnDebugMessage(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user)
{
    Core &core = *static_cast<Core *>(user);
    const char *text = data && data->pMessage ? data->pMessage : "(no message)";
    if (!(types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)))
    {
        // General messages come from the loader (an installed driver that does
        // not load for this architecture, say): worth a line, not a failure.
        Log(core, "loader: %s", text);
        return VK_FALSE;
    }
    const char *kind = "info";
    uint32_t seen = 0;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        seen = ++core.stats.validationErrors;
        kind = "error";
    }
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        seen = ++core.stats.validationWarnings;
        kind = "warning";
    }
    if (seen <= kValidationMessagesLogged)
        Log(core, "validation %s: %s", kind, text);
    if (seen == kValidationMessagesLogged)
        Log(core, "validation: further %s messages are counted, not printed", kind);
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT MessengerInfo(Core &core)
{
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = OnDebugMessage;
    info.pUserData = &core;
    return info;
}

// ---- Instance and surface ------------------------------------------------------

bool CreateInstance(Core &core)
{
    Dispatch &vk = core.vk;
    vk.vkGetInstanceProcAddr = core.window.getInstanceProcAddr;
    if (!vk.vkGetInstanceProcAddr)
        return Fail(core, "no Vulkan loader: the window system gave no vkGetInstanceProcAddr");

#define GFX_VK_LOAD_GLOBAL(name) \
    vk.name = reinterpret_cast<PFN_##name>(vk.vkGetInstanceProcAddr(VK_NULL_HANDLE, #name));
    GFX_VK_GLOBAL_FUNCTIONS(GFX_VK_LOAD_GLOBAL)
#undef GFX_VK_LOAD_GLOBAL
    if (!vk.vkCreateInstance || !vk.vkEnumerateInstanceExtensionProperties || !vk.vkEnumerateInstanceLayerProperties)
        return Fail(core, "the Vulkan loader does not export the global functions");

    // vkEnumerateInstanceVersion is missing on a Vulkan 1.0 loader.
    core.instanceApiVersion = VK_API_VERSION_1_0;
    if (vk.vkEnumerateInstanceVersion && !Check(core, vk.vkEnumerateInstanceVersion(&core.instanceApiVersion),
                                                 "vkEnumerateInstanceVersion"))
        return false;
    if (core.instanceApiVersion < kRequiredApiVersion)
    {
        char have[32];
        FormatVersion(core.instanceApiVersion, have, sizeof(have));
        return Fail(core, "the Vulkan loader is version %s; Vulkan 1.3 is required", have);
    }

    uint32_t count = 0;
    if (!Check(core, vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
               "vkEnumerateInstanceExtensionProperties"))
        return false;
    std::vector<VkExtensionProperties> available(count);
    if (!Check(core, vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()),
               "vkEnumerateInstanceExtensionProperties"))
        return false;
    available.resize(count);

    std::vector<const char *> extensions;
    for (uint32_t i = 0; i < core.window.instanceExtensionCount; ++i)
    {
        const char *name = core.window.instanceExtensions[i];
        if (!HasExtension(available, name))
            return Fail(core, "the window system needs instance extension %s, which the loader does not offer", name);
        extensions.push_back(name);
    }

    std::vector<const char *> layers;
    if (core.desc.validation != Validation::Off)
    {
        count = 0;
        if (!Check(core, vk.vkEnumerateInstanceLayerProperties(&count, nullptr), "vkEnumerateInstanceLayerProperties"))
            return false;
        std::vector<VkLayerProperties> layerList(count);
        if (!Check(core, vk.vkEnumerateInstanceLayerProperties(&count, layerList.data()),
                   "vkEnumerateInstanceLayerProperties"))
            return false;
        layerList.resize(count);
        for (const VkLayerProperties &layer : layerList)
        {
            if (std::strcmp(layer.layerName, kValidationLayer) == 0)
                core.validation = true;
        }
        if (!core.validation && core.desc.validation == Validation::Required)
            return Fail(core, "validation was required but %s is not installed", kValidationLayer);
        if (core.validation)
        {
            layers.push_back(kValidationLayer);
            // The layer provides debug utils itself, so this does not need the
            // loader's list.
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            core.debugUtils = true;
            // Synchronisation validation too: barrier and semaphore mistakes are
            // the bugs a clear-only frame can still have, and the default
            // validation does not look for them.
            count = 0;
            vk.vkEnumerateInstanceExtensionProperties(kValidationLayer, &count, nullptr);
            std::vector<VkExtensionProperties> layerExtensions(count);
            vk.vkEnumerateInstanceExtensionProperties(kValidationLayer, &count, layerExtensions.data());
            layerExtensions.resize(count);
            if (HasExtension(layerExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME))
            {
                extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                core.syncValidation = true;
            }
            else
            {
                Log(core, "the validation layer has no %s; synchronisation validation is off",
                    VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            }
        }
        else
        {
            Log(core, "validation layer %s not installed; running without it", kValidationLayer);
        }
    }
    if (HasExtension(available, kPortabilityEnumeration))
    {
        extensions.push_back(kPortabilityEnumeration);
        core.portabilityEnumeration = true;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = core.desc.applicationName;
    app.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    app.pEngineName = "KisakCOD gfx_vk";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = kRequiredApiVersion;

    VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MessengerInfo(core);
    const VkValidationFeatureEnableEXT enabledFeatures[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT validationFeatures{};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = enabledFeatures;
    if (core.syncValidation)
        messengerInfo.pNext = &validationFeatures;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    // Chained so instance creation and destruction are validated too.
    info.pNext = core.debugUtils ? &messengerInfo : nullptr;
    info.flags = core.portabilityEnumeration ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    VkResult created = vk.vkCreateInstance(&info, nullptr, &core.instance);
    if (created == VK_ERROR_LAYER_NOT_PRESENT && core.validation && core.desc.validation == Validation::IfFound)
    {
        // The layer's manifest is installed but its library does not load: the
        // usual case for a 32-bit process on a distribution that ships the
        // layer for 64-bit only. Optional validation, so carry on without it.
        Log(core, "validation layer %s is listed but does not load for this process; running without it",
            kValidationLayer);
        core.validation = core.debugUtils = core.syncValidation = false;
        layers.clear();
        extensions.erase(std::remove_if(extensions.begin(), extensions.end(),
                                        [](const char *name) {
                                            return std::strcmp(name, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0
                                                || std::strcmp(name, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0;
                                        }),
                         extensions.end());
        info.pNext = nullptr;
        info.enabledLayerCount = 0;
        info.ppEnabledLayerNames = nullptr;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        created = vk.vkCreateInstance(&info, nullptr, &core.instance);
    }
    if (!Check(core, created, "vkCreateInstance"))
        return false;

    const char *missing = nullptr;
#define GFX_VK_LOAD_INSTANCE(name) \
    vk.name = reinterpret_cast<PFN_##name>(vk.vkGetInstanceProcAddr(core.instance, #name)); \
    if (!vk.name && !missing) \
        missing = #name;
    GFX_VK_INSTANCE_FUNCTIONS(GFX_VK_LOAD_INSTANCE)
    if (core.debugUtils)
    {
        GFX_VK_DEBUG_FUNCTIONS(GFX_VK_LOAD_INSTANCE)
    }
#undef GFX_VK_LOAD_INSTANCE
    if (missing)
        return Fail(core, "the Vulkan instance does not provide %s", missing);

    messengerInfo.pNext = nullptr; // the validation features belong to the instance only
    if (core.debugUtils
        && !Check(core, vk.vkCreateDebugUtilsMessengerEXT(core.instance, &messengerInfo, nullptr, &core.messenger),
                  "vkCreateDebugUtilsMessengerEXT"))
        return false;

    if (!core.window.createSurface(core.window.user, core.instance, &core.surface) || !core.surface)
        return Fail(core, "the window system could not create a Vulkan surface");
    return true;
}

// ---- Physical device -------------------------------------------------------------

struct Candidate
{
    VkPhysicalDevice physical;
    uint32_t queueFamily;
    bool usable;
    char why[160];
};

Candidate Evaluate(Core &core, VkPhysicalDevice physical)
{
    Dispatch &vk = core.vk;
    Candidate candidate{physical, 0, false, ""};

    VkPhysicalDeviceProperties props{};
    vk.vkGetPhysicalDeviceProperties(physical, &props);
    if (props.apiVersion < kRequiredApiVersion)
    {
        char have[32];
        FormatVersion(props.apiVersion, have, sizeof(have));
        std::snprintf(candidate.why, sizeof(candidate.why), "Vulkan %s, needs 1.3", have);
        return candidate;
    }

    uint32_t count = 0;
    vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
    extensions.resize(count);
    if (!HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
    {
        std::snprintf(candidate.why, sizeof(candidate.why), "no %s", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        return candidate;
    }

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features13;
    vk.vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!features13.dynamicRendering || !features13.synchronization2)
    {
        std::snprintf(candidate.why, sizeof(candidate.why), "no dynamic rendering or synchronization2");
        return candidate;
    }

    vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    bool found = false;
    for (uint32_t i = 0; i < count && !found; ++i)
    {
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            continue;
        VkBool32 present = VK_FALSE;
        if (vk.vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, core.surface, &present) == VK_SUCCESS && present)
        {
            candidate.queueFamily = i;
            found = true;
        }
    }
    if (!found)
    {
        std::snprintf(candidate.why, sizeof(candidate.why), "no queue family both draws and presents to this window");
        return candidate;
    }

    uint32_t formats = 0;
    uint32_t modes = 0;
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(physical, core.surface, &formats, nullptr);
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(physical, core.surface, &modes, nullptr);
    if (!formats || !modes)
    {
        std::snprintf(candidate.why, sizeof(candidate.why), "the surface lists no formats or present modes");
        return candidate;
    }

    candidate.usable = true;
    std::snprintf(candidate.why, sizeof(candidate.why), "meets the requirements (queue family %u)", candidate.queueFamily);
    return candidate;
}

int TypeRank(VkPhysicalDeviceType type)
{
    switch (type)
    {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
    default: return 0;
    }
}

bool EnumerateDevices(Core &core, std::vector<VkPhysicalDevice> &out)
{
    uint32_t count = 0;
    if (!Check(core, core.vk.vkEnumeratePhysicalDevices(core.instance, &count, nullptr), "vkEnumeratePhysicalDevices"))
        return false;
    out.resize(count);
    if (count && !Check(core, core.vk.vkEnumeratePhysicalDevices(core.instance, &count, out.data()),
                        "vkEnumeratePhysicalDevices"))
        return false;
    out.resize(count);
    if (out.empty())
        return Fail(core, "the Vulkan loader found no physical device (no driver installed?)");
    return true;
}

void FillDeviceInfo(Core &core, VkPhysicalDevice physical, DeviceInfo &info)
{
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &driver;
    core.vk.vkGetPhysicalDeviceProperties2(physical, &props);
    std::memset(&info, 0, sizeof(info));
    std::snprintf(info.name, sizeof(info.name), "%s", props.properties.deviceName);
    info.apiVersion = props.properties.apiVersion;
    info.driverVersion = props.properties.driverVersion;
    info.vendorId = props.properties.vendorID;
    info.deviceId = props.properties.deviceID;
    info.type = props.properties.deviceType;
    std::snprintf(info.driverName, sizeof(info.driverName), "%s", driver.driverName);
    std::snprintf(info.driverInfo, sizeof(info.driverInfo), "%s", driver.driverInfo);
}

bool PickDevice(Core &core)
{
    std::vector<VkPhysicalDevice> devices;
    if (!EnumerateDevices(core, devices))
        return false;

    int best = -1;
    int bestRank = -1;
    uint32_t bestFamily = 0;
    for (size_t i = 0; i < devices.size(); ++i)
    {
        Candidate candidate = Evaluate(core, devices[i]);
        if (core.desc.deviceIndex >= 0)
        {
            if (static_cast<int>(i) != core.desc.deviceIndex)
                continue;
            if (!candidate.usable)
                return Fail(core, "device %zu cannot be used: %s", i, candidate.why);
            best = static_cast<int>(i);
            bestFamily = candidate.queueFamily;
            break;
        }
        if (!candidate.usable)
            continue;
        VkPhysicalDeviceProperties props{};
        core.vk.vkGetPhysicalDeviceProperties(devices[i], &props);
        const int rank = TypeRank(props.deviceType);
        if (rank > bestRank)
        {
            best = static_cast<int>(i);
            bestRank = rank;
            bestFamily = candidate.queueFamily;
        }
    }
    if (core.desc.deviceIndex >= static_cast<int>(devices.size()))
        return Fail(core, "device index %d is out of range: %zu devices", core.desc.deviceIndex, devices.size());
    if (best < 0)
        return Fail(core, "no Vulkan device meets the requirements (Vulkan 1.3, swapchain, a queue that presents here)");

    core.physical = devices[static_cast<size_t>(best)];
    core.queueFamily = bestFamily;
    FillDeviceInfo(core, core.physical, core.device);
    core.vk.vkGetPhysicalDeviceMemoryProperties(core.physical, &core.memory);
    return true;
}

// ---- Logical device, frames and timestamps --------------------------------------------

bool CreateDevice(Core &core)
{
    Dispatch &vk = core.vk;
    uint32_t count = 0;
    vk.vkEnumerateDeviceExtensionProperties(core.physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vk.vkEnumerateDeviceExtensionProperties(core.physical, nullptr, &count, available.data());
    available.resize(count);
    std::vector<const char *> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (HasExtension(available, kPortabilitySubset))
        extensions.push_back(kPortabilitySubset);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = core.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features13;

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    if (!Check(core, vk.vkCreateDevice(core.physical, &info, nullptr, &core.dev), "vkCreateDevice"))
        return false;

    const char *missing = nullptr;
#define GFX_VK_LOAD_DEVICE(name) \
    vk.name = reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(core.dev, #name)); \
    if (!vk.name && !missing) \
        missing = #name;
    GFX_VK_DEVICE_FUNCTIONS(GFX_VK_LOAD_DEVICE)
#undef GFX_VK_LOAD_DEVICE
    if (missing)
        return Fail(core, "the Vulkan device does not provide %s", missing);
    vk.vkGetDeviceQueue(core.dev, core.queueFamily, 0, &core.queue);

    for (FrameSlot &frame : core.frames)
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = core.queueFamily;
        if (!Check(core, vk.vkCreateCommandPool(core.dev, &poolInfo, nullptr, &frame.pool), "vkCreateCommandPool"))
            return false;
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frame.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (!Check(core, vk.vkAllocateCommandBuffers(core.dev, &allocInfo, &frame.cmd), "vkAllocateCommandBuffers"))
            return false;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!Check(core, vk.vkCreateSemaphore(core.dev, &semaphoreInfo, nullptr, &frame.acquired), "vkCreateSemaphore"))
            return false;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!Check(core, vk.vkCreateFence(core.dev, &fenceInfo, nullptr, &frame.done), "vkCreateFence"))
            return false;
    }

    // GPU timestamps around every pass, from the first commit (renderer plan 3.2).
    vk.vkGetPhysicalDeviceQueueFamilyProperties(core.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(core.physical, &count, families.data());
    VkPhysicalDeviceProperties props{};
    vk.vkGetPhysicalDeviceProperties(core.physical, &props);
    const uint32_t bits = families[core.queueFamily].timestampValidBits;
    if (bits && props.limits.timestampPeriod > 0.0f)
    {
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2 * kFramesInFlight;
        if (!Check(core, vk.vkCreateQueryPool(core.dev, &queryInfo, nullptr, &core.timestamps), "vkCreateQueryPool"))
            return false;
        core.timestampPeriodNs = props.limits.timestampPeriod;
        core.timestampMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
        core.stats.clearPass.supported = true;
    }
    else
    {
        Log(core, "the queue has no timestamps; pass timings are off");
    }
    return true;
}

// ---- Swapchain and capture buffer ---------------------------------------------------

void DestroyCaptureBuffer(Core &core)
{
    if (core.captureMapped)
        core.vk.vkUnmapMemory(core.dev, core.captureMemory);
    if (core.captureBuffer)
        core.vk.vkDestroyBuffer(core.dev, core.captureBuffer, nullptr);
    if (core.captureMemory)
        core.vk.vkFreeMemory(core.dev, core.captureMemory, nullptr);
    core.captureMapped = nullptr;
    core.captureBuffer = VK_NULL_HANDLE;
    core.captureMemory = VK_NULL_HANDLE;
    core.captureSize = 0;
}

bool CreateCaptureBuffer(Core &core, VkDeviceSize size)
{
    Dispatch &vk = core.vk;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(core, vk.vkCreateBuffer(core.dev, &info, nullptr, &core.captureBuffer), "vkCreateBuffer"))
        return false;
    VkMemoryRequirements requirements{};
    vk.vkGetBufferMemoryRequirements(core.dev, core.captureBuffer, &requirements);

    // Host-visible memory; cached reads first, since the CPU reads all of it.
    uint32_t chosen = UINT32_MAX;
    const VkMemoryPropertyFlags wanted[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    };
    for (VkMemoryPropertyFlags flags : wanted)
    {
        for (uint32_t i = 0; i < core.memory.memoryTypeCount && chosen == UINT32_MAX; ++i)
        {
            if ((requirements.memoryTypeBits & (1u << i)) && (core.memory.memoryTypes[i].propertyFlags & flags) == flags)
                chosen = i;
        }
        if (chosen != UINT32_MAX)
            break;
    }
    if (chosen == UINT32_MAX)
        return Fail(core, "no host-visible memory type for the capture buffer");
    core.captureCoherent = (core.memory.memoryTypes[chosen].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = chosen;
    if (!Check(core, vk.vkAllocateMemory(core.dev, &alloc, nullptr, &core.captureMemory), "vkAllocateMemory"))
        return false;
    if (!Check(core, vk.vkBindBufferMemory(core.dev, core.captureBuffer, core.captureMemory, 0), "vkBindBufferMemory"))
        return false;
    if (!Check(core, vk.vkMapMemory(core.dev, core.captureMemory, 0, VK_WHOLE_SIZE, 0, &core.captureMapped),
               "vkMapMemory"))
        return false;
    core.captureSize = size;
    return true;
}

void DestroySwapchainResources(Core &core)
{
    for (VkImageView view : core.views)
        core.vk.vkDestroyImageView(core.dev, view, nullptr);
    for (VkSemaphore semaphore : core.renderDone)
        core.vk.vkDestroySemaphore(core.dev, semaphore, nullptr);
    core.views.clear();
    core.renderDone.clear();
    core.images.clear();
}

VkSurfaceFormatKHR ChooseFormat(const std::vector<VkSurfaceFormatKHR> &formats)
{
    // UNORM, not sRGB: the retail renderer writes gamma-space values and never
    // asks for sRGB conversion, so the Legacy path must store them as-is.
    const VkFormat preferred[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
        return {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (VkFormat format : preferred)
    {
        for (const VkSurfaceFormatKHR &candidate : formats)
        {
            if (candidate.format == format && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return candidate;
        }
    }
    return formats[0];
}

VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR> &modes, bool vsync)
{
    if (vsync)
        return VK_PRESENT_MODE_FIFO_KHR; // always available
    const VkPresentModeKHR preferred[] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    for (VkPresentModeKHR mode : preferred)
    {
        if (std::find(modes.begin(), modes.end(), mode) != modes.end())
            return mode;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

bool SurfaceExtent(Core &core, VkSurfaceCapabilitiesKHR &caps, VkExtent2D &extent)
{
    if (!Check(core, core.vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(core.physical, core.surface, &caps),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return false;
    extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
    {
        // The surface takes its size from the swapchain (Wayland, headless).
        uint32_t width = 0;
        uint32_t height = 0;
        core.window.getPixelSize(core.window.user, &width, &height);
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
        if (!width || !height)
            extent = {0, 0};
    }
    return true;
}

BuildResult BuildSwapchain(Core &core)
{
    Dispatch &vk = core.vk;
    VkSurfaceCapabilitiesKHR caps{};
    VkExtent2D extent{};
    if (!SurfaceExtent(core, caps, extent))
        return BuildResult::Failed;
    if (!extent.width || !extent.height)
        return BuildResult::Minimised;

    uint32_t count = 0;
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(core.physical, core.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(core.physical, core.surface, &count, formats.data());
    formats.resize(count);
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(core.physical, core.surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(core.physical, core.surface, &count, modes.data());
    modes.resize(count);
    if (formats.empty() || modes.empty())
    {
        Fail(core, "the surface lists no formats or present modes");
        return BuildResult::Failed;
    }

    const VkSurfaceFormatKHR format = ChooseFormat(formats);
    const VkPresentModeKHR mode = ChoosePresentMode(modes, core.desc.vsync);
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    const bool fourBytes = format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM;
    const bool canCapture = core.desc.allowCapture && fourBytes
        && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (canCapture)
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
    {
        const VkCompositeAlphaFlagBitsKHR others[] = {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                                      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                                      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR};
        for (VkCompositeAlphaFlagBitsKHR other : others)
        {
            if (caps.supportedCompositeAlpha & other)
            {
                alpha = other;
                break;
            }
        }
    }

    // Rebuilding: nothing in flight may still use the old images.
    if (core.swapchain && !Check(core, vk.vkDeviceWaitIdle(core.dev), "vkDeviceWaitIdle"))
        return BuildResult::Failed;

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = core.surface;
    info.minImageCount = imageCount;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = usage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = core.swapchain;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult created = vk.vkCreateSwapchainKHR(core.dev, &info, nullptr, &swapchain);
    DestroySwapchainResources(core);
    if (core.swapchain)
        vk.vkDestroySwapchainKHR(core.dev, core.swapchain, nullptr);
    core.swapchain = swapchain;
    if (!Check(core, created, "vkCreateSwapchainKHR"))
        return BuildResult::Failed;

    if (!Check(core, vk.vkGetSwapchainImagesKHR(core.dev, core.swapchain, &count, nullptr), "vkGetSwapchainImagesKHR"))
        return BuildResult::Failed;
    core.images.resize(count);
    if (!Check(core, vk.vkGetSwapchainImagesKHR(core.dev, core.swapchain, &count, core.images.data()),
               "vkGetSwapchainImagesKHR"))
        return BuildResult::Failed;
    core.images.resize(count);
    for (VkImage image : core.images)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (!Check(core, vk.vkCreateImageView(core.dev, &viewInfo, nullptr, &view), "vkCreateImageView"))
            return BuildResult::Failed;
        core.views.push_back(view);
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (!Check(core, vk.vkCreateSemaphore(core.dev, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore"))
            return BuildResult::Failed;
        core.renderDone.push_back(semaphore);
    }

    if (canCapture)
    {
        const VkDeviceSize size = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
        if (size != core.captureSize)
        {
            DestroyCaptureBuffer(core);
            if (!CreateCaptureBuffer(core, size))
                return BuildResult::Failed;
        }
    }
    else
    {
        DestroyCaptureBuffer(core);
    }

    core.swapInfo = {extent.width, extent.height, format.format, format.colorSpace, mode,
                     static_cast<uint32_t>(core.images.size()), canCapture};
    core.rebuildPending = false;
    core.resizePending = false;
    ++core.stats.swapchainBuilds;
    Log(core, "swapchain %ux%u %s %s, %u images%s", extent.width, extent.height, FormatName(format.format),
        PresentModeName(mode), core.swapInfo.imageCount, canCapture ? ", capturable" : "");
    return BuildResult::Built;
}

// ---- Frame ----------------------------------------------------------------------------

VkImageMemoryBarrier2 ImageBarrier(VkImage image, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, VkImageLayout from,
                                   VkImageLayout to)
{
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

void Barrier(Core &core, VkCommandBuffer cmd, const VkImageMemoryBarrier2 *images, uint32_t imageCount,
             const VkBufferMemoryBarrier2 *buffers = nullptr, uint32_t bufferCount = 0)
{
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = imageCount;
    dependency.pImageMemoryBarriers = images;
    dependency.bufferMemoryBarrierCount = bufferCount;
    dependency.pBufferMemoryBarriers = buffers;
    core.vk.vkCmdPipelineBarrier2(cmd, &dependency);
}

void CollectTimestamps(Core &core, uint32_t slot)
{
    FrameSlot &frame = core.frames[slot];
    if (!frame.timestampsWritten)
        return;
    frame.timestampsWritten = false;
    uint64_t ticks[2] = {};
    // The slot's fence has signalled, so the results are there; no wait flag.
    if (core.vk.vkGetQueryPoolResults(core.dev, core.timestamps, 2 * slot, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT)
        != VK_SUCCESS)
        return;
    const uint64_t elapsed = (ticks[1] - ticks[0]) & core.timestampMask;
    const double ms = static_cast<double>(elapsed) * core.timestampPeriodNs / 1.0e6;
    PassTimings &timings = core.stats.clearPass;
    timings.lastMs = ms;
    timings.minMs = timings.samples ? std::min(timings.minMs, ms) : ms;
    timings.maxMs = timings.samples ? std::max(timings.maxMs, ms) : ms;
    timings.totalMs += ms;
    ++timings.samples;
}

void ReadCapture(Core &core, uint32_t width, uint32_t height, VkFormat format)
{
    if (!core.captureCoherent)
    {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = core.captureMemory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        core.vk.vkInvalidateMappedMemoryRanges(core.dev, 1, &range);
    }
    const size_t size = static_cast<size_t>(width) * height * 4;
    core.capturePixels.assign(static_cast<const uint8_t *>(core.captureMapped),
                              static_cast<const uint8_t *>(core.captureMapped) + size);
    core.capture = {core.capturePixels.data(), width, height, format};
}
} // namespace

// ---- Public API ---------------------------------------------------------------------------

Core *CreateCore(const CoreDesc &desc, const WindowSystem &window, char *error, size_t errorSize)
{
    Core *core = new Core();
    core->desc = desc;
    core->window = window;
    bool ok = CreateInstance(*core) && PickDevice(*core) && CreateDevice(*core);
    if (ok)
    {
        const BuildResult built = BuildSwapchain(*core);
        if (built == BuildResult::Failed)
            ok = false;
        else if (built == BuildResult::Minimised)
            core->rebuildPending = true; // built on the first frame the window has a size
    }
    if (!ok)
    {
        std::snprintf(error, errorSize, "%s", core->error[0] ? core->error : "unknown Vulkan start-up failure");
        DestroyCore(core);
        return nullptr;
    }
    return core;
}

void DestroyCore(Core *core, CoreStats *finalStats)
{
    if (!core)
        return;
    Dispatch &vk = core->vk;
    if (core->dev)
    {
        vk.vkDeviceWaitIdle(core->dev);
        DestroyCaptureBuffer(*core);
        DestroySwapchainResources(*core);
        if (core->swapchain)
            vk.vkDestroySwapchainKHR(core->dev, core->swapchain, nullptr);
        if (core->timestamps)
            vk.vkDestroyQueryPool(core->dev, core->timestamps, nullptr);
        for (FrameSlot &frame : core->frames)
        {
            if (frame.done)
                vk.vkDestroyFence(core->dev, frame.done, nullptr);
            if (frame.acquired)
                vk.vkDestroySemaphore(core->dev, frame.acquired, nullptr);
            if (frame.pool)
                vk.vkDestroyCommandPool(core->dev, frame.pool, nullptr);
        }
        vk.vkDestroyDevice(core->dev, nullptr);
    }
    if (core->surface && vk.vkDestroySurfaceKHR)
        vk.vkDestroySurfaceKHR(core->instance, core->surface, nullptr);
    if (core->messenger && vk.vkDestroyDebugUtilsMessengerEXT)
        vk.vkDestroyDebugUtilsMessengerEXT(core->instance, core->messenger, nullptr);
    if (core->instance && vk.vkDestroyInstance)
        vk.vkDestroyInstance(core->instance, nullptr);
    if (finalStats)
        *finalStats = core->stats;
    delete core;
}

bool LogDevices(const CoreDesc &desc, const WindowSystem &window, char *error, size_t errorSize)
{
    Core core;
    core.desc = desc;
    core.window = window;
    std::vector<VkPhysicalDevice> devices;
    bool ok = CreateInstance(core) && EnumerateDevices(core, devices);
    for (size_t i = 0; ok && i < devices.size(); ++i)
    {
        DeviceInfo info{};
        FillDeviceInfo(core, devices[i], info);
        const Candidate candidate = Evaluate(core, devices[i]);
        char api[32];
        FormatVersion(info.apiVersion, api, sizeof(api));
        Log(core, "device %zu: %s (%s, Vulkan %s, %s %s, %04x:%04x): %s%s", i, info.name, DeviceTypeName(info.type),
            api, info.driverName, info.driverInfo, info.vendorId, info.deviceId, candidate.usable ? "usable, " : "",
            candidate.why);
    }
    if (!ok)
        std::snprintf(error, errorSize, "%s", core.error);
    if (core.surface)
        core.vk.vkDestroySurfaceKHR(core.instance, core.surface, nullptr);
    if (core.messenger)
        core.vk.vkDestroyDebugUtilsMessengerEXT(core.instance, core.messenger, nullptr);
    if (core.instance)
        core.vk.vkDestroyInstance(core.instance, nullptr);
    return ok;
}

const char *LastError(const Core *core) { return core->error; }
uint32_t InstanceApiVersion(const Core *core) { return core->instanceApiVersion; }
bool ValidationEnabled(const Core *core) { return core->validation; }
bool SynchronizationValidationEnabled(const Core *core) { return core->syncValidation; }
const DeviceInfo &Device(const Core *core) { return core->device; }
const SwapchainInfo &Swapchain(const Core *core) { return core->swapInfo; }
const CoreStats &Stats(const Core *core) { return core->stats; }
void NotifyResize(Core *core) { core->resizePending = true; }

bool CaptureNextFrame(Core *core)
{
    if (core->rebuildPending || core->resizePending)
    {
        // The swapchain about to be built decides; ask for it if allowed at all.
        core->captureRequested = core->desc.allowCapture;
        return core->desc.allowCapture;
    }
    core->captureRequested = core->swapInfo.canCapture;
    return core->swapInfo.canCapture;
}

Capture LastCapture(const Core *core) { return core->capture; }

void WaitIdle(Core *core)
{
    if (core->dev)
        core->vk.vkDeviceWaitIdle(core->dev);
    for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
        CollectTimestamps(*core, slot);
}

FrameResult ClearAndPresent(Core *core_, const float rgba[4])
{
    Core &core = *core_;
    Dispatch &vk = core.vk;
    if (core.failed)
        return FrameResult::Failed;

    if (core.resizePending && !core.rebuildPending)
    {
        // A size event: rebuild only when the size really changed (or the
        // window was minimised, which skips frames until it comes back).
        VkSurfaceCapabilitiesKHR caps{};
        VkExtent2D extent{};
        if (!SurfaceExtent(core, caps, extent))
            return FrameResult::Failed;
        if (extent.width != core.swapInfo.width || extent.height != core.swapInfo.height)
            core.rebuildPending = true;
        core.resizePending = false;
    }
    if (core.rebuildPending)
    {
        const BuildResult built = BuildSwapchain(core);
        if (built == BuildResult::Failed)
            return FrameResult::Failed;
        if (built == BuildResult::Minimised)
        {
            ++core.stats.framesSkipped;
            return FrameResult::Skipped;
        }
    }

    const uint32_t slot = core.frameIndex;
    FrameSlot &frame = core.frames[slot];
    if (!Check(core, vk.vkWaitForFences(core.dev, 1, &frame.done, VK_TRUE, UINT64_MAX), "vkWaitForFences"))
        return FrameResult::Failed;
    CollectTimestamps(core, slot);

    uint32_t imageIndex = 0;
    VkResult result = vk.vkAcquireNextImageKHR(core.dev, core.swapchain, UINT64_MAX, frame.acquired, VK_NULL_HANDLE,
                                               &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // Nothing was signalled; the fence is still signalled too. Rebuild next time.
        core.rebuildPending = true;
        ++core.stats.framesSkipped;
        return FrameResult::Skipped;
    }
    if (result == VK_SUBOPTIMAL_KHR)
        core.rebuildPending = true; // still usable: draw this one, rebuild after
    else if (!Check(core, result, "vkAcquireNextImageKHR"))
        return FrameResult::Failed;

    if (!Check(core, vk.vkResetFences(core.dev, 1, &frame.done), "vkResetFences")
        || !Check(core, vk.vkResetCommandPool(core.dev, frame.pool, 0), "vkResetCommandPool"))
        return FrameResult::Failed;

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer cmd = frame.cmd;
    if (!Check(core, vk.vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer"))
        return FrameResult::Failed;

    const VkImage image = core.images[imageIndex];
    const bool timed = core.timestamps != VK_NULL_HANDLE;
    if (timed)
        vk.vkCmdResetQueryPool(cmd, core.timestamps, 2 * slot, 2);

    // The previous contents are not needed: the pass clears everything.
    VkImageMemoryBarrier2 toAttachment = ImageBarrier(
        image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    Barrier(core, cmd, &toAttachment, 1);

    if (timed)
        vk.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, core.timestamps, 2 * slot);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = core.views[imageIndex];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(color.clearValue.color.float32, rgba, 4 * sizeof(float));
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {core.swapInfo.width, core.swapInfo.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vk.vkCmdBeginRendering(cmd, &rendering);
    vk.vkCmdEndRendering(cmd);

    if (timed)
        vk.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, core.timestamps, 2 * slot + 1);

    const bool capturing = core.captureRequested && core.swapInfo.canCapture;
    core.captureRequested = false;
    if (capturing)
    {
        VkImageMemoryBarrier2 toCopy = ImageBarrier(
            image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Barrier(core, cmd, &toCopy, 1);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {core.swapInfo.width, core.swapInfo.height, 1};
        vk.vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, core.captureBuffer, 1, &region);
        VkBufferMemoryBarrier2 toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = core.captureBuffer;
        toHost.offset = 0;
        toHost.size = VK_WHOLE_SIZE;
        VkImageMemoryBarrier2 toPresent = ImageBarrier(
            image, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE, kPresentHandOffStage, VK_ACCESS_2_NONE,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        Barrier(core, cmd, &toPresent, 1, &toHost, 1);
    }
    else
    {
        VkImageMemoryBarrier2 toPresent = ImageBarrier(
            image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            kPresentHandOffStage, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        Barrier(core, cmd, &toPresent, 1);
    }
    if (!Check(core, vk.vkEndCommandBuffer(cmd), "vkEndCommandBuffer"))
        return FrameResult::Failed;

    VkSemaphoreSubmitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = frame.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = core.renderDone[imageIndex];
    signal.stageMask = kPresentHandOffStage;
    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    if (!Check(core, vk.vkQueueSubmit2(core.queue, 1, &submit, frame.done), "vkQueueSubmit2"))
        return FrameResult::Failed;
    frame.timestampsWritten = timed;

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &core.renderDone[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &core.swapchain;
    present.pImageIndices = &imageIndex;
    result = vk.vkQueuePresentKHR(core.queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        core.rebuildPending = true;
    else if (!Check(core, result, "vkQueuePresentKHR"))
        return FrameResult::Failed;

    if (capturing)
    {
        if (!Check(core, vk.vkWaitForFences(core.dev, 1, &frame.done, VK_TRUE, UINT64_MAX), "vkWaitForFences"))
            return FrameResult::Failed;
        ReadCapture(core, core.swapInfo.width, core.swapInfo.height, core.swapInfo.format);
    }

    core.frameIndex = (core.frameIndex + 1) % kFramesInFlight;
    ++core.stats.framesPresented;
    return FrameResult::Ok;
}

const char *PresentModeName(VkPresentModeKHR mode)
{
    switch (mode)
    {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "OTHER";
    }
}

const char *FormatName(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_UNORM";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    default: return "OTHER";
    }
}

const char *DeviceTypeName(VkPhysicalDeviceType type)
{
    switch (type)
    {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
    default: return "other";
    }
}
} // namespace gfx_vk

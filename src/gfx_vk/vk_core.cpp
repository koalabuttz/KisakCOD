// gfx_vk shared core, start-up: instance, device, queue and swapchain.
// See vk_core.h for the contract and docs/vulkan-core.md for how it is checked.

#include "vk_core_internal.h"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gfx_vk
{
namespace
{
constexpr uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;
constexpr const char *kValidationLayer = "VK_LAYER_KHRONOS_validation";
// Optional extensions. Portability enumeration lists MoltenVK (macOS) devices;
// the spec requires enabling the portability subset wherever a device offers it.
constexpr const char *kPortabilityEnumeration = "VK_KHR_portability_enumeration";
constexpr const char *kPortabilitySubset = "VK_KHR_portability_subset";
// Validation messages printed per severity; the rest are only counted.
constexpr uint32_t kValidationMessagesLogged = 20;

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
} // namespace

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
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
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

namespace
{
// The validation layer reports calls the record thread makes on that thread:
// the message waits for the owning thread (SyncRecorder), which logs it and
// counts it. The draw rule OnDebugMessage counts apart shows up here.
VkBool32 OnRecordThreadMessage(Core &core, VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                               VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT *data,
                               const char *text)
{
    Recorder &recorder = core.recorder;
    std::lock_guard<std::mutex> lock(recorder.messageMutex);
    if (!(types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)))
    {
        recorder.messages.push_back(std::string("loader: ") + text);
        return VK_FALSE;
    }
    const char *id = data && data->pMessageIdName ? data->pMessageIdName : "";
    const size_t idLength = std::strlen(id);
    if (idLength > 11 && std::strcmp(id + idLength - 11, "-None-06479") == 0 && std::strncmp(id, "VUID-vkCmdDraw", 14) == 0)
    {
        ++recorder.validationIgnored;
        return VK_FALSE;
    }
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (error)
        ++recorder.validationErrors;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        ++recorder.validationWarnings;
    else
        return VK_FALSE;
    if (++recorder.messagesSeen <= kValidationMessagesLogged)
        recorder.messages.push_back(std::string(error ? "validation error" : "validation warning")
                                    + " (record thread): " + text);
    if (recorder.messagesSeen == kValidationMessagesLogged)
        recorder.messages.push_back("validation: further record thread messages are counted, not printed");
    return VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL OnDebugMessage(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user)
{
    Core &core = *static_cast<Core *>(user);
    const char *text = data && data->pMessage ? data->pMessage : "(no message)";
    if (OnRecordThread())
        return OnRecordThreadMessage(core, severity, types, data, text);
    if (!(types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)))
    {
        // General messages come from the loader (an installed driver that does
        // not load for this architecture, say): worth a line, not a failure.
        Log(core, "loader: %s", text);
        return VK_FALSE;
    }
    // The Legacy shaders sample every 2D texture through a branch on a
    // uniform (SamplerStateBlock::useDepthCompare): a depth-compare sample for
    // depth textures, a plain one otherwise. The layer checks the compare
    // instruction's format rule statically, against colour textures the
    // branch never compares, so it reports every textured draw. Counted
    // apart, not as errors.
    const char *id = data && data->pMessageIdName ? data->pMessageIdName : "";
    const size_t idLength = std::strlen(id);
    if (idLength > 11 && std::strcmp(id + idLength - 11, "-None-06479") == 0 && std::strncmp(id, "VUID-vkCmdDraw", 14) == 0)
    {
        if (++core.stats.validationIgnored == 1)
            Log(core, "validation: %s (depth-compare format rule, checked statically) is counted apart from errors", id);
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
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features12;
    vk.vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!features13.dynamicRendering || !features13.synchronization2)
    {
        std::snprintf(candidate.why, sizeof(candidate.why), "no dynamic rendering or synchronization2");
        return candidate;
    }
    if (!features12.timelineSemaphore)
    {
        std::snprintf(candidate.why, sizeof(candidate.why), "no timeline semaphores");
        return candidate;
    }
    // The Legacy shader translator lays uniform blocks out tightly (dxbc-spirv
    // emits scalar block layout, as DXVK requires).
    if (!features12.scalarBlockLayout)
    {
        std::snprintf(candidate.why, sizeof(candidate.why), "no scalar block layout");
        return candidate;
    }
    // Its SPIR-V also uses the Vulkan memory model, buffer device addresses and
    // demote-to-helper (all required by Vulkan 1.3, checked for a clear message).
    if (!features12.vulkanMemoryModel || !features12.bufferDeviceAddress
        || !features13.shaderDemoteToHelperInvocation)
    {
        std::snprintf(candidate.why, sizeof(candidate.why),
                      "no Vulkan memory model, buffer device address or demote to helper invocation");
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
        return Fail(core, "no Vulkan device meets the requirements (Vulkan 1.3, timeline semaphores, swapchain, "
                          "a queue that presents here)");

    core.physical = devices[static_cast<size_t>(best)];
    core.queueFamily = bestFamily;
    FillDeviceInfo(core, core.physical, core.device);
    core.vk.vkGetPhysicalDeviceProperties(core.physical, &core.properties);
    core.vk.vkGetPhysicalDeviceMemoryProperties(core.physical, &core.memory);
    return true;
}

// ---- Logical device and timestamps ------------------------------------------------------

VkFormat PickDepthStencilFormat(Core &core)
{
    const VkFormatFeatureFlags needed =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    const VkFormat candidates[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    for (VkFormat format : candidates)
    {
        VkFormatProperties props{};
        core.vk.vkGetPhysicalDeviceFormatProperties(core.physical, format, &props);
        if ((props.optimalTilingFeatures & needed) == needed)
            return format;
    }
    return VK_FORMAT_UNDEFINED;
}

// VK_KHR_portability_subset's feature struct (vulkan_beta.h, which the build
// does not enable): fifteen VkBool32 features after the header.
struct PortabilitySubsetFeatures
{
    VkStructureType sType;
    void *pNext;
    VkBool32 features[15];
};
constexpr VkStructureType kPortabilitySubsetFeaturesType = static_cast<VkStructureType>(1000163000);

bool EnvEquals(const char *name, const char *value)
{
#if defined(_MSC_VER)
    char *text = nullptr;
    size_t length = 0;
    const bool equal = _dupenv_s(&text, &length, name) == 0 && text && std::strcmp(text, value) == 0;
    std::free(text);
    return equal;
#else
    const char *text = std::getenv(name);
    return text && std::strcmp(text, value) == 0;
#endif
}

// A count from 1 to 65535 in the variable, or fallback when it is not set or
// is anything else.
uint32_t EnvCount(const char *name, uint32_t fallback)
{
#if defined(_MSC_VER)
    char *owned = nullptr;
    size_t length = 0;
    const char *text = _dupenv_s(&owned, &length, name) == 0 ? owned : nullptr;
#else
    const char *text = std::getenv(name);
#endif
    uint32_t count = fallback;
    if (text && *text)
    {
        char *end = nullptr;
        const unsigned long value = std::strtoul(text, &end, 10);
        if (!*end && value >= 1 && value <= 65535)
            count = static_cast<uint32_t>(value);
    }
#if defined(_MSC_VER)
    std::free(owned);
#endif
    return count;
}

bool CreateDevice(Core &core)
{
    Dispatch &vk = core.vk;
    uint32_t count = 0;
    vk.vkEnumerateDeviceExtensionProperties(core.physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vk.vkEnumerateDeviceExtensionProperties(core.physical, nullptr, &count, available.data());
    available.resize(count);
    std::vector<const char *> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const bool portability = HasExtension(available, kPortabilitySubset);
    if (portability)
        extensions.push_back(kPortabilitySubset);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = core.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    // Optional features D3D9 content uses where the device has them:
    // anisotropic filtering, wireframe fill, independent blend for MRT, clip
    // distances (user clip planes), 16-bit floats in translated shaders, and
    // occlusion queries that count samples.
    VkPhysicalDeviceFeatures supported{};
    VkPhysicalDeviceVulkan12Features supported12{};
    // On a portability device (MoltenVK) every subset feature it has is
    // enabled: the D3D9 formats need swizzled image views
    // (imageViewFormatSwizzle), and the validation layer holds the device to
    // what was enabled.
    PortabilitySubsetFeatures portabilityFeatures{};
    portabilityFeatures.sType = kPortabilitySubsetFeaturesType;
    {
        supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        supported12.pNext = portability ? &portabilityFeatures : nullptr;
        VkPhysicalDeviceFeatures2 query{};
        query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        query.pNext = &supported12;
        vk.vkGetPhysicalDeviceFeatures2(core.physical, &query);
        supported = query.features;
    }
    core.optional.samplerAnisotropy = supported.samplerAnisotropy;
    core.optional.fillModeNonSolid = supported.fillModeNonSolid;
    core.optional.independentBlend = supported.independentBlend;
    core.optional.shaderClipDistance = supported.shaderClipDistance;
    core.optional.shaderFloat16 = supported12.shaderFloat16;
    // MoltenVK's paths (see OptionalFeatures), or forced for testing.
    core.optional.dynamicVertexStride = !portability && !EnvEquals("KISAK_VK_VERTEX_STRIDE", "static");
    core.optional.fixedDepthCompare = portability || EnvEquals("KISAK_VK_DEPTH_COMPARE", "fixed");
    core.optional.forceWideRenderTargets = EnvEquals("KISAK_VK_RENDER_TARGETS", "wide");
    core.optional.staticUniforms = EnvEquals("KISAK_VK_UNIFORMS", "static");
    core.optional.fenceFlush = !EnvEquals("KISAK_VK_FENCE_FLUSH", "0");
    // Tile-based GPUs pay for every part with a store and a load of the
    // targets (see OptionalFeatures): Apple, Arm, Qualcomm, Imagination,
    // Broadcom, and MoltenVK on any GPU.
    const uint32_t vendor = core.properties.vendorID;
    const bool tiled = portability || vendor == 0x106b || vendor == 0x13b5 || vendor == 0x5143 || vendor == 0x1010
        || vendor == 0x14e4;
    core.optional.gpuFeedAlways = EnvEquals("KISAK_VK_GPU_FEED", "always");
    core.optional.gpuFeed =
        core.optional.gpuFeedAlways || EnvEquals("KISAK_VK_GPU_FEED", "1") || (!tiled && !EnvEquals("KISAK_VK_GPU_FEED", "0"));
    core.optional.gpuFeedDraws = EnvCount("KISAK_VK_GPU_FEED_DRAWS", 128);
    core.optional.occlusionCounts = supported.occlusionQueryPrecise && supported12.hostQueryReset
        && !EnvEquals("KISAK_VK_OCCLUSION_QUERIES", "0");

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    features13.shaderDemoteToHelperInvocation = VK_TRUE;
    portabilityFeatures.pNext = nullptr;
    features13.pNext = portability ? &portabilityFeatures : nullptr;
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;
    features12.timelineSemaphore = VK_TRUE;
    features12.scalarBlockLayout = VK_TRUE;
    features12.vulkanMemoryModel = VK_TRUE;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.shaderFloat16 = supported12.shaderFloat16;
    features12.hostQueryReset = core.optional.occlusionCounts ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features12;
    features.features.samplerAnisotropy = supported.samplerAnisotropy;
    features.features.fillModeNonSolid = supported.fillModeNonSolid;
    features.features.independentBlend = supported.independentBlend;
    features.features.shaderClipDistance = supported.shaderClipDistance;
    features.features.occlusionQueryPrecise = core.optional.occlusionCounts ? VK_TRUE : VK_FALSE;

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

    core.depthStencilFormat = PickDepthStencilFormat(core);
    if (core.depthStencilFormat == VK_FORMAT_UNDEFINED)
        return Fail(core, "the device has no sampled depth-stencil format (D24S8 or D32S8)");

    VkSemaphoreTypeCreateInfo timelineType{};
    timelineType.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineType.initialValue = 0;
    VkSemaphoreCreateInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timelineInfo.pNext = &timelineType;
    if (!Check(core, vk.vkCreateSemaphore(core.dev, &timelineInfo, nullptr, &core.timeline), "vkCreateSemaphore"))
        return false;

    // GPU timestamps around every pass, from the first commit (renderer plan 3.2).
    vk.vkGetPhysicalDeviceQueueFamilyProperties(core.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(core.physical, &count, families.data());
    const uint32_t bits = families[core.queueFamily].timestampValidBits;
    if (bits && core.properties.limits.timestampPeriod > 0.0f)
    {
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = kTimestampsPerSlot * kFramesInFlight;
        if (!Check(core, vk.vkCreateQueryPool(core.dev, &queryInfo, nullptr, &core.timestamps), "vkCreateQueryPool"))
            return false;
        core.timestampPeriodNs = core.properties.limits.timestampPeriod;
        core.timestampMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    }
    else
    {
        Log(core, "the queue has no timestamps; pass timings are off");
    }
    return true;
}

// ---- Swapchain -----------------------------------------------------------------------

void DestroySwapchainResources(Core &core)
{
    for (Image *image : core.swapImages)
    {
        core.vk.vkDestroyImageView(core.dev, image->view, nullptr);
        delete image;
    }
    for (VkSemaphore semaphore : core.renderDone)
        core.vk.vkDestroySemaphore(core.dev, semaphore, nullptr);
    core.swapImages.clear();
    core.renderDone.clear();
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
} // namespace

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
    // Blits into the swapchain (a scene drawn offscreen, then shown).
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

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

    // Rebuilding: nothing in flight may still use the old images, and the
    // record thread is done with the old swapchain and the queue.
    if (core.swapchain && (!DrainRecorder(core) || !Check(core, vk.vkDeviceWaitIdle(core.dev), "vkDeviceWaitIdle")))
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
    std::vector<VkImage> images(count);
    if (!Check(core, vk.vkGetSwapchainImagesKHR(core.dev, core.swapchain, &count, images.data()),
               "vkGetSwapchainImagesKHR"))
        return BuildResult::Failed;
    images.resize(count);
    for (VkImage handle : images)
    {
        Image *image = new Image();
        image->image = handle;
        image->swapchain = true;
        image->name = "swapchain";
        image->desc.kind = ImageKind::Tex2D;
        image->desc.format = format.format;
        image->desc.width = extent.width;
        image->desc.height = extent.height;
        image->desc.usage = usage;
        image->desc.name = "swapchain";
        core.swapImages.push_back(image);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = handle;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!Check(core, vk.vkCreateImageView(core.dev, &viewInfo, nullptr, &image->view), "vkCreateImageView"))
            return BuildResult::Failed;
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (!Check(core, vk.vkCreateSemaphore(core.dev, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore"))
            return BuildResult::Failed;
        core.renderDone.push_back(semaphore);
    }

    core.swapInfo = {extent.width, extent.height, format.format, format.colorSpace, mode,
                     static_cast<uint32_t>(core.swapImages.size()), canCapture};
    core.rebuildPending = false;
    core.resizePending = false;
    ++core.stats.swapchainBuilds;
    Log(core, "swapchain %ux%u %s %s, %u images%s", extent.width, extent.height, FormatName(format.format),
        PresentModeName(mode), core.swapInfo.imageCount, canCapture ? ", capturable" : "");
    return BuildResult::Built;
}

// ---- Public API ---------------------------------------------------------------------------

Core *CreateCore(const CoreDesc &desc, const WindowSystem &window, char *error, size_t errorSize)
{
    Core *core = new Core();
    core->desc = desc;
    core->window = window;
    if (EnvEquals("KISAK_VK_RECORD_THREAD", "0"))
        core->desc.recordThread = false;
    else if (EnvEquals("KISAK_VK_RECORD_THREAD", "1"))
        core->desc.recordThread = true;
    bool ok = CreateInstance(*core) && PickDevice(*core) && CreateDevice(*core) && CreateFrameResources(*core)
        && StartRecorder(*core) && CreatePipelineCache(*core);
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
    // Everything recorded runs (an open frame's commands are never submitted:
    // the pool goes with them), then the thread ends.
    StopRecorder(*core);
    if (core->dev)
    {
        vk.vkDeviceWaitIdle(core->dev);
        if (core->pipelineCache)
            SavePipelineCache(core);
        RunDeferred(*core, true);
        DestroyFrameResources(*core);
        DestroyPipelineObjects(*core);
        DestroySwapchainResources(*core);
        if (core->swapchain)
            vk.vkDestroySwapchainKHR(core->dev, core->swapchain, nullptr);
        if (core->timestamps)
            vk.vkDestroyQueryPool(core->dev, core->timestamps, nullptr);
        for (VkQueryPool pool : core->occlusionPools)
            vk.vkDestroyQueryPool(core->dev, pool, nullptr);
        if (core->timeline)
            vk.vkDestroySemaphore(core->dev, core->timeline, nullptr);
        DestroyMemoryPools(*core);
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

void DetachWindow(Core *core_)
{
    if (!core_ || core_->windowDetached)
        return;
    Core &core = *core_;
    Dispatch &vk = core.vk;
    core.windowDetached = true;
    if (core.dev)
    {
        // The acquired image is never presented.
        AbandonFrame(core);
        DrainRecorder(core);
        vk.vkDeviceWaitIdle(core.dev);
        DestroySwapchainResources(core);
        if (core.swapchain)
            vk.vkDestroySwapchainKHR(core.dev, core.swapchain, nullptr);
        core.swapchain = VK_NULL_HANDLE;
    }
    if (core.surface && vk.vkDestroySurfaceKHR)
        vk.vkDestroySurfaceKHR(core.instance, core.surface, nullptr);
    core.surface = VK_NULL_HANDLE;
    core.resizePending = false;
    core.rebuildPending = false;
    Log(core, "window detached: swapchain and surface destroyed before their window");
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

const Dispatch &Functions(const Core *core) { return core->vk; }
VkDevice DeviceHandle(const Core *core) { return core->dev; }
VkPhysicalDevice PhysicalDeviceHandle(const Core *core) { return core->physical; }
const VkPhysicalDeviceLimits &Limits(const Core *core) { return core->properties.limits; }
VkFormat DepthStencilFormat(const Core *core) { return core->depthStencilFormat; }

const OptionalFeatures &Features(const Core *core) { return core->optional; }

bool FormatSupports(const Core *core, VkFormat format, VkFormatFeatureFlags features)
{
    VkFormatProperties props{};
    core->vk.vkGetPhysicalDeviceFormatProperties(core->physical, format, &props);
    return (props.optimalTilingFeatures & features) == features;
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
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32_SFLOAT_S8_UINT";
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

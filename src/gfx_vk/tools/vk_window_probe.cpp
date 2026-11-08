// KisakCOD-vulkan-probe: opens an SDL3 window and clears and presents it through
// the shared Vulkan core (src/gfx_vk/vk_core.h).
//
// This is the visible test of the Vulkan renderer back end's shared core
// (renderer plan M2): instance, device, swapchain, a cleared frame presented
// every refresh, swapchain rebuilds when the window changes size, and GPU
// timestamps per pass. With --draw every frame instead draws a small textured
// scene offscreen through the core's resource paths (vk_probe_draw.h) and blits
// it into the window. Nothing of the engine is linked and no game file is read.
//
// Checks, each one a stage whose failure ends the run:
//   sdl       SDL's video subsystem starts
//   loader    SDL finds a Vulkan loader
//   window    a Vulkan-capable window opens
//   core      instance, device and first swapchain (gfx_vk::CreateCore)
//   frames    the requested number of frames are presented, or the window is
//             closed by hand after at least one
//   resize    with --resize: the window is resized halfway and the swapchain
//             follows it to the new size
//   draw      with --draw: a still frame of the scene is read back and known
//             points compared (also in the swapchain with --check-capture),
//             and no pipeline was built during a frame
//   capture   with --check-capture: the last frame is cleared to a known colour,
//             copied back and compared in memory (no image is written)
//   validation  with the validation layer loaded: it reported no errors
//
// Output is key=value lines on stdout. Exit status 0 means every check passed,
// 1 a failed stage (the last line names it), 2 a usage error.
// See docs/vulkan-core.md.

#include <gfx_vk/vk_core.h>

#include "vk_probe_draw.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
constexpr int kMinimumSide = 64;
constexpr int kMaximumSide = 8192;
constexpr int kMaximumFrames = 100000;
// The capture frame's colour, and the bytes it must read back as (round(x * 255)).
constexpr float kCaptureColor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
constexpr int kCaptureBytes[3] = {64, 128, 191};

struct Options
{
    int frames = 300;
    int width = 960;
    int height = 540;
    gfx_vk::Validation validation = gfx_vk::Validation::IfFound;
    int device = -1;
    bool vsync = true;
    bool resize = false;
    bool checkCapture = false;
    bool listDevices = false;
    bool draw = false;
    std::string pipelineCache;
};

void Usage(const char *argv0)
{
    std::fprintf(stderr,
                 "usage: %s [--frames N] [--size WxH] [--validation off|auto|require] [--device N]\n"
                 "          [--no-vsync] [--resize] [--check-capture] [--list-devices] [--draw]\n"
                 "          [--pipeline-cache PATH]\n",
                 argv0);
}

bool ParseInt(const char *text, int low, int high, int *out)
{
    char *end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (!text[0] || *end || value < low || value > high)
        return false;
    *out = static_cast<int>(value);
    return true;
}

bool ParseOptions(int argc, char **argv, Options *options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (arg == "--frames" && next)
        {
            if (!ParseInt(next, 1, kMaximumFrames, &options->frames))
                return false;
            ++i;
        }
        else if (arg == "--size" && next)
        {
            const char *x = std::strchr(next, 'x');
            if (!x)
                return false;
            const std::string width(next, x);
            if (!ParseInt(width.c_str(), kMinimumSide, kMaximumSide, &options->width)
                || !ParseInt(x + 1, kMinimumSide, kMaximumSide, &options->height))
                return false;
            ++i;
        }
        else if (arg == "--validation" && next)
        {
            const std::string mode = next;
            if (mode == "off")
                options->validation = gfx_vk::Validation::Off;
            else if (mode == "auto")
                options->validation = gfx_vk::Validation::IfFound;
            else if (mode == "require")
                options->validation = gfx_vk::Validation::Required;
            else
                return false;
            ++i;
        }
        else if (arg == "--device" && next)
        {
            if (!ParseInt(next, 0, 63, &options->device))
                return false;
            ++i;
        }
        else if (arg == "--no-vsync")
            options->vsync = false;
        else if (arg == "--resize")
            options->resize = true;
        else if (arg == "--check-capture")
            options->checkCapture = true;
        else if (arg == "--list-devices")
            options->listDevices = true;
        else if (arg == "--draw")
            options->draw = true;
        else if (arg == "--pipeline-cache" && next && next[0])
        {
            options->pipelineCache = next;
            ++i;
        }
        else
            return false;
    }
    if (options->resize && options->frames < 4)
        return false;
    return true;
}

int Failed(const char *stage, const char *reason)
{
    std::printf("vulkan_probe=failed stage=%s\n", stage);
    std::printf("vulkan_probe.reason=%s\n", reason);
    std::fflush(stdout);
    return 1;
}

void LogLine(void *, const char *line)
{
    std::printf("vulkan_probe.log=%s\n", line);
    std::fflush(stdout);
}

// SDL3 as the core's window system.
struct SdlWindow
{
    SDL_Window *window = nullptr;
};

bool CreateSurface(void *user, VkInstance instance, VkSurfaceKHR *surface)
{
    SdlWindow *sdl = static_cast<SdlWindow *>(user);
    if (SDL_Vulkan_CreateSurface(sdl->window, instance, nullptr, surface))
        return true;
    std::printf("vulkan_probe.log=SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    return false;
}

void GetPixelSize(void *user, uint32_t *width, uint32_t *height)
{
    SdlWindow *sdl = static_cast<SdlWindow *>(user);
    int w = 0;
    int h = 0;
    if (!SDL_GetWindowSizeInPixels(sdl->window, &w, &h) || (SDL_GetWindowFlags(sdl->window) & SDL_WINDOW_MINIMIZED))
        w = h = 0;
    *width = static_cast<uint32_t>(w > 0 ? w : 0);
    *height = static_cast<uint32_t>(h > 0 ? h : 0);
}

// A slow colour cycle, so a person watching sees frames are live.
void FrameColor(int frame, float rgba[4])
{
    const double t = frame / 120.0 * 6.283185307179586;
    rgba[0] = static_cast<float>(0.5 + 0.4 * std::sin(t));
    rgba[1] = static_cast<float>(0.5 + 0.4 * std::sin(t + 2.0943951023931953));
    rgba[2] = static_cast<float>(0.5 + 0.4 * std::sin(t + 4.1887902047863905));
    rgba[3] = 1.0f;
}

const char *VersionString(uint32_t version, char *buffer, size_t size)
{
    std::snprintf(buffer, size, "%u.%u.%u", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                  VK_API_VERSION_PATCH(version));
    return buffer;
}

// Pumps SDL events; false once the window was closed.
bool PumpEvents(SDL_Window *window, gfx_vk::Core *core)
{
    bool open = true;
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            open = false;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.key == SDLK_ESCAPE)
                open = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESTORED:
            if (core && event.window.windowID == SDL_GetWindowID(window))
                gfx_vk::NotifyResize(core);
            break;
        default:
            break;
        }
    }
    return open;
}

int Run(const Options &options)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
        return Failed("sdl", SDL_GetError());
    std::printf("vulkan_probe.sdl_video_driver=%s\n", SDL_GetCurrentVideoDriver());
    if (!SDL_Vulkan_LoadLibrary(nullptr))
        return Failed("loader", SDL_GetError());

    SdlWindow sdl;
    sdl.window = SDL_CreateWindow("KisakCOD Vulkan window probe", options.width, options.height,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!sdl.window)
        return Failed("window", SDL_GetError());

    Uint32 extensionCount = 0;
    const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (!extensions)
        return Failed("loader", SDL_GetError());
    gfx_vk::WindowSystem window{};
    window.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    window.instanceExtensions = extensions;
    window.instanceExtensionCount = extensionCount;
    window.user = &sdl;
    window.createSurface = CreateSurface;
    window.getPixelSize = GetPixelSize;
    if (!window.getInstanceProcAddr)
        return Failed("loader", SDL_GetError());

    gfx_vk::CoreDesc desc{};
    desc.applicationName = "KisakCOD Vulkan window probe";
    desc.validation = options.validation;
    desc.deviceIndex = options.device;
    desc.vsync = options.vsync;
    desc.allowCapture = options.checkCapture;
    desc.pipelineCachePath = options.pipelineCache.empty() ? nullptr : options.pipelineCache.c_str();
    desc.log = LogLine;

    char error[512] = {};
    if (options.listDevices)
    {
        if (!gfx_vk::LogDevices(desc, window, error, sizeof(error)))
            return Failed("core", error);
        std::printf("vulkan_probe=listed\n");
        return 0;
    }

    gfx_vk::Core *core = gfx_vk::CreateCore(desc, window, error, sizeof(error));
    if (!core)
        return Failed("core", error);

    char version[32];
    const gfx_vk::DeviceInfo &device = gfx_vk::Device(core);
    std::printf("vulkan_probe.instance_api=%s\n", VersionString(gfx_vk::InstanceApiVersion(core), version, sizeof(version)));
    std::printf("vulkan_probe.validation=%s\n", !gfx_vk::ValidationEnabled(core) ? "off"
                : gfx_vk::SynchronizationValidationEnabled(core)                ? "on+sync"
                                                                                  : "on");
    std::printf("vulkan_probe.device=%s\n", device.name);
    std::printf("vulkan_probe.device_type=%s\n", gfx_vk::DeviceTypeName(device.type));
    std::printf("vulkan_probe.device_api=%s\n", VersionString(device.apiVersion, version, sizeof(version)));
    std::printf("vulkan_probe.driver=%s %s\n", device.driverName, device.driverInfo);
    std::printf("vulkan_probe.device_id=%04x:%04x\n", device.vendorId, device.deviceId);
    // A copy: the core updates its own on every rebuild.
    const gfx_vk::SwapchainInfo first = gfx_vk::Swapchain(core);
    std::printf("vulkan_probe.swapchain=%ux%u %s %s images=%u\n", first.width, first.height,
                gfx_vk::FormatName(first.format), gfx_vk::PresentModeName(first.presentMode), first.imageCount);
    std::fflush(stdout);

    vk_probe::DrawScene *scene = nullptr;
    if (options.draw)
    {
        std::string reason;
        scene = vk_probe::CreateDrawScene(core, reason);
        if (!scene)
        {
            gfx_vk::DestroyCore(core);
            return Failed("draw", reason.c_str());
        }
    }

    int resizeWidth = 0;
    int resizeHeight = 0;
    bool open = true;
    int presented = 0;
    const auto start = std::chrono::steady_clock::now();
    // Skipped frames (minimised window, rebuilt swapchain) do not count, but a
    // run that skips this many in a row has stalled.
    int skippedInARow = 0;
    while (open && presented < options.frames)
    {
        open = PumpEvents(sdl.window, core);
        if (!open)
            break;
        if (options.resize && presented == options.frames / 2 && !resizeWidth)
        {
            resizeWidth = options.width + 128;
            resizeHeight = options.height + 72;
            SDL_SetWindowSize(sdl.window, resizeWidth, resizeHeight);
            SDL_SyncWindow(sdl.window);
            // Some window systems report the new size only through the surface
            // (out of date); others only through the event. Either way the
            // core rebuilds before the next frame.
            gfx_vk::NotifyResize(core);
        }
        gfx_vk::FrameResult result = gfx_vk::FrameResult::Failed;
        std::string drawError;
        if (scene)
        {
            result = gfx_vk::BeginFrame(core);
            if (result == gfx_vk::FrameResult::Ok)
            {
                result = vk_probe::RecordDrawFrame(core, scene, presented + 1, drawError) ? gfx_vk::Present(core)
                                                                                          : gfx_vk::FrameResult::Failed;
            }
        }
        else
        {
            float rgba[4];
            FrameColor(presented, rgba);
            result = gfx_vk::ClearAndPresent(core, rgba);
        }
        if (result == gfx_vk::FrameResult::Failed)
        {
            const std::string reason = !drawError.empty() ? drawError : std::string(gfx_vk::LastError(core));
            vk_probe::DestroyDrawScene(core, scene);
            gfx_vk::DestroyCore(core);
            return Failed("frames", reason.c_str());
        }
        if (result == gfx_vk::FrameResult::Ok)
        {
            ++presented;
            skippedInARow = 0;
        }
        else if (++skippedInARow > 1000)
        {
            vk_probe::DestroyDrawScene(core, scene);
            gfx_vk::DestroyCore(core);
            return Failed("frames", "no frame could be presented 1000 times in a row");
        }
        else
        {
            SDL_Delay(5);
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!presented)
    {
        vk_probe::DestroyDrawScene(core, scene);
        gfx_vk::DestroyCore(core);
        return Failed("frames", "the window was closed before any frame was presented");
    }
    if (!open)
        std::printf("vulkan_probe.closed_by_user=1\n");

    int status = 0;
    const char *failedStage = nullptr;
    std::string failedReason;

    if (options.resize && open)
    {
        const gfx_vk::SwapchainInfo &now = gfx_vk::Swapchain(core);
        uint32_t pixelWidth = 0;
        uint32_t pixelHeight = 0;
        GetPixelSize(&sdl, &pixelWidth, &pixelHeight);
        std::printf("vulkan_probe.resize=%dx%d window_pixels=%ux%u swapchain=%ux%u builds=%u\n", resizeWidth,
                    resizeHeight, pixelWidth, pixelHeight, now.width, now.height, gfx_vk::Stats(core).swapchainBuilds);
        if (gfx_vk::Stats(core).swapchainBuilds < 2 || now.width != pixelWidth || now.height != pixelHeight
            || pixelWidth == first.width)
        {
            failedStage = "resize";
            failedReason = "the swapchain did not follow the window to its new size";
        }
    }

    if (!failedStage && scene)
    {
        // A still frame of the scene, read back from the target (and from the
        // swapchain image the blit wrote, when the surface can be copied from).
        vk_probe::DrawCheck target;
        vk_probe::DrawCheck shown;
        std::string reason;
        gfx_vk::FrameResult result = gfx_vk::FrameResult::Skipped;
        for (int attempt = 0; attempt < 100 && result == gfx_vk::FrameResult::Skipped; ++attempt)
        {
            PumpEvents(sdl.window, core);
            result = gfx_vk::BeginFrame(core);
        }
        bool ok = result == gfx_vk::FrameResult::Ok && vk_probe::RecordDrawFrame(core, scene, 0, reason)
            && vk_probe::StartDrawCheck(core, scene, options.checkCapture, reason)
            && gfx_vk::Present(core) == gfx_vk::FrameResult::Ok
            && vk_probe::FinishDrawCheck(core, scene, target, shown, reason);
        if (!ok)
        {
            failedStage = "draw";
            failedReason = !reason.empty() ? reason : std::string(gfx_vk::LastError(core));
            if (failedReason.empty())
                failedReason = "no frame could be drawn for the check";
        }
        else
        {
            std::printf("vulkan_probe.draw_target=points %d mismatched %d%s%s\n", target.points, target.mismatched,
                        target.detail.empty() ? "" : " ", target.detail.c_str());
            if (shown.points)
                std::printf("vulkan_probe.draw_swapchain=points %d mismatched %d%s%s\n", shown.points,
                            shown.mismatched, shown.detail.empty() ? "" : " ", shown.detail.c_str());
            else
                std::printf("vulkan_probe.draw_swapchain=not read (needs --check-capture and a copyable surface)\n");
            const gfx_vk::CoreStats &now = gfx_vk::Stats(core);
            if (target.mismatched || shown.mismatched)
            {
                failedStage = "draw";
                failedReason = "the drawn scene does not have the expected colours";
            }
            else if (now.pipelinesCreatedInFrame)
            {
                failedStage = "draw";
                failedReason = "a pipeline was built during a frame";
            }
        }
    }

    if (!failedStage && options.checkCapture)
    {
        if (!gfx_vk::CaptureNextFrame(core))
        {
            failedStage = "capture";
            failedReason = "the swapchain images cannot be copied from on this surface";
        }
        else
        {
            gfx_vk::FrameResult result = gfx_vk::FrameResult::Skipped;
            for (int attempt = 0; attempt < 100 && result == gfx_vk::FrameResult::Skipped; ++attempt)
            {
                PumpEvents(sdl.window, core);
                gfx_vk::CaptureNextFrame(core);
                result = gfx_vk::ClearAndPresent(core, kCaptureColor);
            }
            const gfx_vk::Capture capture = gfx_vk::LastCapture(core);
            if (result != gfx_vk::FrameResult::Ok || !capture.pixels)
            {
                failedStage = "capture";
                failedReason = result == gfx_vk::FrameResult::Failed ? gfx_vk::LastError(core)
                                                                     : "no frame was captured";
            }
            else
            {
                const bool bgra = capture.format == VK_FORMAT_B8G8R8A8_UNORM;
                uint32_t wrong = 0;
                int firstPixel[4] = {-1, -1, -1, -1};
                const size_t count = static_cast<size_t>(capture.width) * capture.height;
                for (size_t i = 0; i < count; ++i)
                {
                    const uint8_t *p = capture.pixels + 4 * i;
                    const int r = bgra ? p[2] : p[0];
                    const int g = p[1];
                    const int b = bgra ? p[0] : p[2];
                    if (i == 0)
                    {
                        firstPixel[0] = r;
                        firstPixel[1] = g;
                        firstPixel[2] = b;
                        firstPixel[3] = p[3];
                    }
                    if (std::abs(r - kCaptureBytes[0]) > 1 || std::abs(g - kCaptureBytes[1]) > 1
                        || std::abs(b - kCaptureBytes[2]) > 1)
                        ++wrong;
                }
                std::printf("vulkan_probe.capture=%ux%u %s first_pixel_rgba=%d,%d,%d,%d expected_rgb=%d,%d,%d "
                            "mismatched_pixels=%u\n",
                            capture.width, capture.height, gfx_vk::FormatName(capture.format), firstPixel[0], firstPixel[1],
                            firstPixel[2], firstPixel[3], kCaptureBytes[0], kCaptureBytes[1], kCaptureBytes[2], wrong);
                if (wrong)
                {
                    failedStage = "capture";
                    failedReason = "the captured frame is not the cleared colour";
                }
            }
        }
    }

    vk_probe::DestroyDrawScene(core, scene);
    gfx_vk::WaitIdle(core);
    const gfx_vk::CoreStats &stats = gfx_vk::Stats(core);
    std::printf("vulkan_probe.frames_presented=%llu\n", static_cast<unsigned long long>(stats.framesPresented));
    std::printf("vulkan_probe.frames_skipped=%llu\n", static_cast<unsigned long long>(stats.framesSkipped));
    std::printf("vulkan_probe.swapchain_builds=%u\n", stats.swapchainBuilds);
    std::printf("vulkan_probe.wall_fps=%.1f\n", seconds > 0.0 ? presented / seconds : 0.0);
    if (stats.clearPass.supported && stats.clearPass.samples)
    {
        std::printf("vulkan_probe.clear_pass_gpu_ms=avg %.4f min %.4f max %.4f samples %llu\n",
                    stats.clearPass.totalMs / static_cast<double>(stats.clearPass.samples), stats.clearPass.minMs,
                    stats.clearPass.maxMs, static_cast<unsigned long long>(stats.clearPass.samples));
    }
    else
    {
        std::printf("vulkan_probe.clear_pass_gpu_ms=unsupported\n");
    }
    if (scene)
    {
        const gfx_vk::PassTimings timings = gfx_vk::PassTimingsFor(core, "scene");
        if (timings.supported && timings.samples)
            std::printf("vulkan_probe.scene_pass_gpu_ms=avg %.4f min %.4f max %.4f samples %llu\n",
                        timings.totalMs / static_cast<double>(timings.samples), timings.minMs, timings.maxMs,
                        static_cast<unsigned long long>(timings.samples));
        else
            std::printf("vulkan_probe.scene_pass_gpu_ms=unsupported\n");
    }
    if (stats.frame.supported && stats.frame.samples)
        std::printf("vulkan_probe.frame_gpu_ms=avg %.4f min %.4f max %.4f samples %llu\n",
                    stats.frame.totalMs / static_cast<double>(stats.frame.samples), stats.frame.minMs,
                    stats.frame.maxMs, static_cast<unsigned long long>(stats.frame.samples));
    std::printf("vulkan_probe.submissions=%llu flushes=%llu\n", static_cast<unsigned long long>(stats.submissions),
                static_cast<unsigned long long>(stats.flushes));
    std::printf("vulkan_probe.memory=device %llu host %llu blocks %u buffers %u images %u\n",
                static_cast<unsigned long long>(stats.deviceMemoryBytes),
                static_cast<unsigned long long>(stats.hostMemoryBytes), stats.memoryBlocks, stats.buffersAlive,
                stats.imagesAlive);
    std::printf("vulkan_probe.uploads=staging_bytes %llu transient_bytes %llu descriptor_sets %llu barriers %u "
                "deferred_destroyed %llu\n",
                static_cast<unsigned long long>(stats.stagingBytes),
                static_cast<unsigned long long>(stats.transientBytes),
                static_cast<unsigned long long>(stats.descriptorSets), stats.barriers,
                static_cast<unsigned long long>(stats.deferredDestroyed));
    std::printf("vulkan_probe.pipelines=created %u in_frame %u cache_hits %u cache_loaded %d create_ms %.3f "
                "samplers %u\n",
                stats.pipelinesCreated, stats.pipelinesCreatedInFrame, stats.pipelineCacheHits,
                stats.pipelineCacheLoaded ? 1 : 0, stats.pipelineCreateMs, stats.samplersCreated);
    const bool validation = gfx_vk::ValidationEnabled(core);
    gfx_vk::CoreStats ending{};
    gfx_vk::DestroyCore(core, &ending);
    SDL_DestroyWindow(sdl.window);
    const uint32_t validationErrors = ending.validationErrors;
    std::printf("vulkan_probe.validation_errors=%u\n", ending.validationErrors);
    std::printf("vulkan_probe.validation_warnings=%u\n", ending.validationWarnings);
    std::printf("vulkan_probe.validation_ignored=%u\n", ending.validationIgnored);

    if (!failedStage && validation && validationErrors)
    {
        failedStage = "validation";
        failedReason = "the validation layer reported errors (see the log lines)";
    }
    if (failedStage)
        status = Failed(failedStage, failedReason.c_str());
    else
        std::printf("vulkan_probe=passed\n");
    std::fflush(stdout);
    return status;
}
} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!ParseOptions(argc, argv, &options))
    {
        Usage(argv[0]);
        return 2;
    }
    const int status = Run(options);
    SDL_Quit();
    return status;
}

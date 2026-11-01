// Standalone D3D9 device smoke test for DXVK Native 2.7.1 with the SDL3 WSI.
//
// Scope: an initial device/present experiment for the 32-bit DXVK Native build.
// There are no engine headers, no assets and no world renderer here: every frame
// is one full-screen Clear() followed by Present().
//
// Contracts this file exercises on purpose:
//   * window created on the main thread, visible, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
//   * DXVK_WSI_DRIVER must select the SDL3 WSI ("SDL3") before the device exists
//   * device created with D3DDEVTYPE_HAL and D3DCREATE_HARDWARE_VERTEXPROCESSING
//   * X8R8G8B8 back buffer, D3DSWAPEFFECT_DISCARD, one back buffer, vsync present
//   * window resize is asynchronous: the run waits a bounded number of polls for
//     SDL to report the new *pixel* size, then resets the device for that size
//   * a second, explicit Reset follows at --reset-at, at the unchanged size
//   * every Reset builds D3DPRESENT_PARAMETERS from scratch, then compares the swap
//     chain parameters and the new back buffer description with the pixel size SDL
//     reports
//   * every failing HRESULT stops the run with a non-zero exit code and a stage label
//   * teardown order is IDirect3DDevice9 -> IDirect3D9 -> window -> SDL_Quit
//
// The DXVK native headers define __stdcall/WINAPI/STDMETHODCALLTYPE as empty, so
// this uses the default Linux ABI throughout; never add WINAPI/__stdcall here.
//
// Build and run instructions: see README.md next to this file.

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vulkan/vulkan.h>

#include <d3d9.h>
#include <windows.h>
#include <wsi/native_sdl3.h>

namespace {

constexpr int kDefaultFrames       = 180;
constexpr int kDefaultResizeAt     = 60;
constexpr int kDefaultResetAt      = 120;
constexpr int kDefaultWidth        = 640;
constexpr int kDefaultHeight       = 360;
constexpr int kDefaultResizeWidth  = 800;
constexpr int kDefaultResizeHeight = 480;

// Bounded waits. A window manager may take a few frames to acknowledge a resize;
// the run never blocks forever waiting for one.
constexpr int kResizeConfirmPolls = 60;
constexpr int kResizePollDelayMs  = 5;

constexpr int kMaxFrames = 100000;
constexpr int kMaxSize   = 8192;

// DXVK registers its WSI backends by name; the SDL3 bootstrap is "SDL3".
constexpr const char* kExpectedWsiDriver = "SDL3";

// One clear colour per stage so a human can tell the stages apart on screen.
constexpr D3DCOLOR kColorInitial   = D3DCOLOR_XRGB(24, 150, 60);
constexpr D3DCOLOR kColorResized   = D3DCOLOR_XRGB(210, 150, 32);
constexpr D3DCOLOR kColorPostReset = D3DCOLOR_XRGB(40, 96, 220);

enum ExitCode {
  ExitPass              = 0,
  ExitFailure           = 1,
  ExitUsage             = 2,
  ExitVulkanUnsupported = 3,
  ExitInterrupted       = 4,
};

enum class AwaitResult {
  Confirmed,
  Timeout,
  Failed,
  Interrupted,
};

enum class FrameResult {
  Presented,
  Failed,
};

#if defined(__GNUC__)
#define SMOKE_PRINTF_FORMAT(first, second) __attribute__((format(printf, first, second)))
#else
#define SMOKE_PRINTF_FORMAT(first, second)
#endif

std::string format(const char* fmt, ...) SMOKE_PRINTF_FORMAT(1, 2);

std::string format(const char* fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  return std::string(buffer);
}

std::string hrText(HRESULT hr) {
  return format("0x%08X", static_cast<unsigned>(hr));
}

struct Options {
  int  frames       = kDefaultFrames;
  int  resizeAt     = kDefaultResizeAt;
  int  resetAt      = kDefaultResetAt;
  int  width        = kDefaultWidth;
  int  height       = kDefaultHeight;
  int  resizeWidth  = kDefaultResizeWidth;
  int  resizeHeight = kDefaultResizeHeight;
  // Substring of the Vulkan deviceName that must be present. Empty means "first
  // non-CPU device", which is reported explicitly at startup.
  std::string expectGpu;
};

struct RunStats {
  int frames              = 0;  // frames that were cleared and presented
  int clears              = 0;
  int presents            = 0;
  int resets              = 0;
  int resizeRequests      = 0;
  int resizeConfirmations = 0;
  int backBufferChecks    = 0;
  int checksPassed        = 0;
  int checksFailed        = 0;
  int quitEvents          = 0;
  int closeEvents         = 0;
  int pixelSizeEvents     = 0;
  int keyEvents           = 0;
  int otherEvents         = 0;
};

void printUsage(std::FILE* out) {
  std::fprintf(out,
    "usage: d3d9_sdl3_smoke [options]\n"
    "\n"
    "  --frames N         frames to present before exiting (default %d)\n"
    "  --resize-at N      frame index that resizes the window and resets the device (default %d)\n"
    "  --reset-at N       frame index that resets the device again at the unchanged size (default %d)\n"
    "  --width N          initial window width in points (default %d)\n"
    "  --height N         initial window height in points (default %d)\n"
    "  --resize-width N   requested width after the resize (default %d)\n"
    "  --resize-height N  requested height after the resize (default %d)\n"
    "  --expect-gpu TEXT  require this substring in the Vulkan deviceName; CPU devices\n"
    "                     are always rejected (default: first non-CPU device)\n"
    "  --help             print this help text\n"
    "\n"
    "Requirements: --resize-at >= 2, --resize-at < --reset-at <= --frames, and the\n"
    "resize target must differ from the initial size.\n"
    "Exit codes: 0 pass, 1 failure, 2 usage, 3 Vulkan unsupported, 4 interrupted.\n",
    kDefaultFrames, kDefaultResizeAt, kDefaultResetAt, kDefaultWidth, kDefaultHeight,
    kDefaultResizeWidth, kDefaultResizeHeight);
}

bool parsePositiveInt(const char* text, int maxValue, int* value) {
  if (text == nullptr || text[0] == '\0')
    return false;

  char* end = nullptr;
  const long parsed = std::strtol(text, &end, 10);
  if (end == nullptr || *end != '\0' || parsed <= 0 || parsed > maxValue)
    return false;

  *value = static_cast<int>(parsed);
  return true;
}

// Returns ExitPass when the arguments are usable, ExitUsage otherwise.
// --help prints the usage text and is reported back as ExitUsage by the caller.
int parseArgs(int argc, char** argv, Options& options, bool* helpRequested) {
  *helpRequested = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    std::string value;

    const std::string::size_type equals = arg.find('=');
    if (equals != std::string::npos) {
      value = arg.substr(equals + 1);
      arg   = arg.substr(0, equals);
    }

    if (arg == "--help" || arg == "-h") {
      *helpRequested = true;
      return ExitUsage;
    }

    if (arg == "--expect-gpu") {
      if (value.empty()) {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "missing value for %s\n", arg.c_str());
          return ExitUsage;
        }
        value = argv[++i];
      }
      options.expectGpu = value;
      continue;
    }

    int* target = nullptr;
    int  limit  = kMaxFrames;

    if (arg == "--frames") {
      target = &options.frames;
    } else if (arg == "--resize-at") {
      target = &options.resizeAt;
    } else if (arg == "--reset-at") {
      target = &options.resetAt;
    } else if (arg == "--width") {
      target = &options.width;
      limit  = kMaxSize;
    } else if (arg == "--height") {
      target = &options.height;
      limit  = kMaxSize;
    } else if (arg == "--resize-width") {
      target = &options.resizeWidth;
      limit  = kMaxSize;
    } else if (arg == "--resize-height") {
      target = &options.resizeHeight;
      limit  = kMaxSize;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return ExitUsage;
    }

    if (value.empty()) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", arg.c_str());
        return ExitUsage;
      }
      value = argv[++i];
    }

    if (!parsePositiveInt(value.c_str(), limit, target)) {
      std::fprintf(stderr, "invalid value for %s: %s\n", arg.c_str(), value.c_str());
      return ExitUsage;
    }
  }

  if (!(options.resizeAt < options.resetAt && options.resetAt <= options.frames)) {
    std::fprintf(stderr,
      "frame indices must satisfy --resize-at (%d) < --reset-at (%d) <= --frames (%d)\n",
      options.resizeAt, options.resetAt, options.frames);
    return ExitUsage;
  }

  // At least one initial-stage frame has to be presented before the resize, otherwise
  // the run never exercises the stage-0 colour path.
  if (options.resizeAt < 2) {
    std::fprintf(stderr, "--resize-at must be >= 2 so at least one initial frame is presented\n");
    return ExitUsage;
  }

  // A resize to the size the window already has proves nothing.
  if (options.resizeWidth == options.width && options.resizeHeight == options.height) {
    std::fprintf(stderr,
      "resize target %dx%d equals the initial size; pass a different --resize-width/--resize-height\n",
      options.resizeWidth, options.resizeHeight);
    return ExitUsage;
  }

  return ExitPass;
}

std::string toLowerAscii(std::string value) {
  for (char& character : value)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return value;
}

const char* physicalDeviceTypeName(VkPhysicalDeviceType type) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:          return "other";
    default:                                     return "unknown";
  }
}

struct VulkanDeviceInfo {
  std::string          name;
  VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
};

// Physical devices enumerated through the loader SDL already opened. No extensions are
// enabled beyond the ones SDL reports, and the instance is destroyed again before DXVK
// creates its own.
struct VulkanProbe {
  bool                          loaderReady = false;
  std::vector<VulkanDeviceInfo> devices;
  int                           selectedIndex = -1;
  std::string                   classification = "no-loader";
};

VulkanProbe probeVulkanDevices() {
  VulkanProbe probe;

  const SDL_FunctionPointer rawGetProc = SDL_Vulkan_GetVkGetInstanceProcAddr();
  if (rawGetProc == nullptr)
    return probe;

  const auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(rawGetProc);
  const auto createInstance =
    reinterpret_cast<PFN_vkCreateInstance>(getProc(VK_NULL_HANDLE, "vkCreateInstance"));
  if (createInstance == nullptr)
    return probe;

  Uint32 extensionCount = 0;
  const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);

  VkApplicationInfo applicationInfo{};
  applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  applicationInfo.pApplicationName = "kisakcod-d3d9-sdl3-smoke";
  applicationInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  applicationInfo.pEngineName = "none";
  applicationInfo.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &applicationInfo;
  createInfo.enabledExtensionCount = extensionCount;
  createInfo.ppEnabledExtensionNames = extensions;

  VkInstance instance = VK_NULL_HANDLE;
  if (createInstance(&createInfo, nullptr, &instance) != VK_SUCCESS || instance == VK_NULL_HANDLE) {
    probe.classification = "no-instance";
    return probe;
  }
  probe.loaderReady = true;

  const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
    getProc(instance, "vkEnumeratePhysicalDevices"));
  const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
    getProc(instance, "vkGetPhysicalDeviceProperties"));
  const auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
    getProc(instance, "vkDestroyInstance"));

  if (enumerate != nullptr && getProperties != nullptr) {
    uint32_t deviceCount = 0;
    if (enumerate(instance, &deviceCount, nullptr) == VK_SUCCESS && deviceCount > 0) {
      std::vector<VkPhysicalDevice> handles(deviceCount);
      if (enumerate(instance, &deviceCount, handles.data()) == VK_SUCCESS) {
        for (uint32_t i = 0; i < deviceCount; ++i) {
          VkPhysicalDeviceProperties properties{};
          getProperties(handles[i], &properties);
          VulkanDeviceInfo info;
          info.name = std::string(properties.deviceName);
          info.type = properties.deviceType;
          probe.devices.push_back(info);
        }
      }
    }
  }

  if (destroyInstance != nullptr)
    destroyInstance(instance, nullptr);

  probe.classification = probe.devices.empty() ? "no-devices" : "unknown";
  return probe;
}

// Applies the explicit GPU filter and rejects CPU devices. An empty filter selects the
// first non-CPU device, and the selection is always reported.
void classifyVulkanProbe(VulkanProbe& probe, const std::string& expectGpu) {
  if (!probe.loaderReady || probe.devices.empty())
    return;

  const std::string wanted = toLowerAscii(expectGpu);
  int fallback = -1;

  for (size_t i = 0; i < probe.devices.size(); ++i) {
    if (probe.devices[i].type == VK_PHYSICAL_DEVICE_TYPE_CPU)
      continue;

    if (fallback < 0)
      fallback = static_cast<int>(i);

    if (!wanted.empty() && toLowerAscii(probe.devices[i].name).find(wanted) != std::string::npos) {
      probe.selectedIndex = static_cast<int>(i);
      break;
    }
  }

  if (!wanted.empty() && probe.selectedIndex < 0) {
    probe.classification = "expect-mismatch";
    return;
  }

  if (probe.selectedIndex < 0)
    probe.selectedIndex = fallback;

  probe.classification = probe.selectedIndex >= 0 ? "ok" : "software-only";
}

bool check(RunStats& stats, const std::string& name, bool condition, const std::string& detail) {
  std::printf("[check] %-24s %-4s %s\n", name.c_str(), condition ? "OK" : "FAIL", detail.c_str());
  if (condition) {
    ++stats.checksPassed;
  } else {
    ++stats.checksFailed;
  }
  return condition;
}

std::string describePresentParameters(const D3DPRESENT_PARAMETERS& params) {
  return format("backbuffer=%ux%u format=%d count=%u swap=%d windowed=%d interval=0x%08X",
                params.BackBufferWidth, params.BackBufferHeight,
                static_cast<int>(params.BackBufferFormat), params.BackBufferCount,
                static_cast<int>(params.SwapEffect), static_cast<int>(params.Windowed),
                static_cast<unsigned>(params.PresentationInterval));
}

// Built fresh for CreateDevice and again for Reset: D3D9 rewrites the struct it is
// given, so reusing one instance across a resize is how stale sizes creep in.
D3DPRESENT_PARAMETERS makePresentParameters(HWND window, int width, int height) {
  D3DPRESENT_PARAMETERS params;
  std::memset(&params, 0, sizeof(params));

  params.BackBufferWidth  = static_cast<UINT>(width);
  params.BackBufferHeight = static_cast<UINT>(height);
  params.BackBufferFormat = D3DFMT_X8R8G8B8;
  params.BackBufferCount  = 1;                     // one back buffer, discard semantics
  params.MultiSampleType  = D3DMULTISAMPLE_NONE;
  params.MultiSampleQuality = 0;
  params.SwapEffect       = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow    = window;
  params.Windowed         = TRUE;
  params.EnableAutoDepthStencil = FALSE;
  params.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
  params.Flags            = 0;
  params.FullScreen_RefreshRateInHz = 0;
  params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;   // vsync

  return params;
}

const char* stageName(int stage) {
  switch (stage) {
    case 0:  return "initial";
    case 1:  return "resized";
    case 2:  return "post-reset";
    default: return "unknown";
  }
}

D3DCOLOR stageColor(int stage) {
  switch (stage) {
    case 0:  return kColorInitial;
    case 1:  return kColorResized;
    case 2:  return kColorPostReset;
    default: return kColorInitial;
  }
}

bool readPixelSize(SDL_Window* window, int* width, int* height, std::string* error) {
  int pixelWidth = 0;
  int pixelHeight = 0;

  if (!SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight)) {
    *error = format("SDL_GetWindowSizeInPixels failed: %s", SDL_GetError());
    return false;
  }

  if (pixelWidth <= 0 || pixelHeight <= 0) {
    *error = format("SDL reported a non-positive pixel size %dx%d", pixelWidth, pixelHeight);
    return false;
  }

  *width  = pixelWidth;
  *height = pixelHeight;
  return true;
}

struct BackBufferInfo {
  bool           valid = false;
  std::string    error;
  D3DSURFACE_DESC desc = {};
};

// Reads the back buffer description through GetBackBuffer + GetDesc and releases
// the surface again, so no reference is held across Reset().
BackBufferInfo readBackBufferInfo(IDirect3DDevice9* device) {
  BackBufferInfo info;

  IDirect3DSurface9* surface = nullptr;
  HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface);
  if (FAILED(hr) || surface == nullptr) {
    info.error = format("GetBackBuffer(0, 0, MONO) failed, hr=%s", hrText(hr).c_str());
    if (surface != nullptr)
      surface->Release();
    return info;
  }

  hr = surface->GetDesc(&info.desc);
  if (FAILED(hr)) {
    info.error = format("IDirect3DSurface9::GetDesc failed, hr=%s", hrText(hr).c_str());
  } else {
    info.valid = true;
  }

  surface->Release();
  return info;
}

std::string describeBackBuffer(const BackBufferInfo& info) {
  if (!info.valid)
    return info.error;
  return format("back buffer %ux%u format=%d pool=%d",
                info.desc.Width, info.desc.Height,
                static_cast<int>(info.desc.Format), static_cast<int>(info.desc.Pool));
}

struct PresentParametersInfo {
  bool                valid = false;
  std::string         error;
  D3DPRESENT_PARAMETERS params = {};
};

// IDirect3DDevice9 exposes no GetPresentParameters(); the active parameters live on
// the implicit swap chain, which is read here and released again straight away.
PresentParametersInfo readPresentParameters(IDirect3DDevice9* device) {
  PresentParametersInfo info;

  IDirect3DSwapChain9* swapChain = nullptr;
  HRESULT hr = device->GetSwapChain(0, &swapChain);
  if (FAILED(hr) || swapChain == nullptr) {
    info.error = format("GetSwapChain(0) failed, hr=%s", hrText(hr).c_str());
    if (swapChain != nullptr)
      swapChain->Release();
    return info;
  }

  hr = swapChain->GetPresentParameters(&info.params);
  if (FAILED(hr)) {
    info.error = format("IDirect3DSwapChain9::GetPresentParameters failed, hr=%s",
                        hrText(hr).c_str());
  } else {
    info.valid = true;
  }

  swapChain->Release();
  return info;
}

// Resets the device with parameters built from scratch for the pixel size SDL reports,
// then checks that the swap chain parameters and the new back buffer agree with it.
// Called twice: once for the resize, once for the later reset at the unchanged size.
bool resetWithFreshParameters(IDirect3DDevice9* device, HWND hwnd,
                              int pixelWidth, int pixelHeight,
                              const std::string& checkPrefix,
                              RunStats& stats, std::string* failure) {
  D3DPRESENT_PARAMETERS freshParams = makePresentParameters(hwnd, pixelWidth, pixelHeight);
  std::printf("[reset]  %s: Reset %s\n", checkPrefix.c_str(),
              describePresentParameters(freshParams).c_str());

  const HRESULT resetHr = device->Reset(&freshParams);
  if (FAILED(resetHr)) {
    std::printf("[reset]  %s: TestCooperativeLevel=0x%08X\n", checkPrefix.c_str(),
                static_cast<unsigned>(device->TestCooperativeLevel()));
    *failure = format("Reset failed, hr=%s", hrText(resetHr).c_str());
    return false;
  }
  ++stats.resets;

  const PresentParametersInfo params = readPresentParameters(device);
  check(stats, checkPrefix + "-params",
        params.valid
          && params.params.BackBufferWidth == static_cast<UINT>(pixelWidth)
          && params.params.BackBufferHeight == static_cast<UINT>(pixelHeight),
        params.valid ? describePresentParameters(params.params) : params.error);

  const BackBufferInfo backBuffer = readBackBufferInfo(device);
  ++stats.backBufferChecks;
  const bool matches = backBuffer.valid
    && backBuffer.desc.Width == static_cast<UINT>(pixelWidth)
    && backBuffer.desc.Height == static_cast<UINT>(pixelHeight)
    && backBuffer.desc.Format == D3DFMT_X8R8G8B8;
  check(stats, checkPrefix + "-backbuffer", matches,
        format("%s, window pixels %dx%d", describeBackBuffer(backBuffer).c_str(),
               pixelWidth, pixelHeight));
  return matches;
}

struct PumpResult {
  bool quitRequested = false;
};

PumpResult pumpEvents(RunStats& stats, const char* stage) {
  PumpResult result;

  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_EVENT_QUIT:
        ++stats.quitEvents;
        std::printf("[event]  %-10s SDL_EVENT_QUIT\n", stage);
        result.quitRequested = true;
        break;

      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        ++stats.closeEvents;
        std::printf("[event]  %-10s SDL_EVENT_WINDOW_CLOSE_REQUESTED\n", stage);
        result.quitRequested = true;
        break;

      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        ++stats.pixelSizeEvents;
        std::printf("[event]  %-10s SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED %dx%d\n",
                    stage, event.window.data1, event.window.data2);
        break;

      case SDL_EVENT_KEY_DOWN:
        ++stats.keyEvents;
        if (event.key.key == SDLK_ESCAPE) {
          std::printf("[event]  %-10s escape pressed\n", stage);
          result.quitRequested = true;
        }
        break;

      default:
        ++stats.otherEvents;
        break;
    }
  }

  return result;
}

// Polls the pixel size until it matches `wantWidth` x `wantHeight`, the poll budget
// is exhausted, the window closes, or the size cannot be read at all.
AwaitResult awaitPixelSize(SDL_Window* window, int wantWidth, int wantHeight,
                           int previousWidth, int previousHeight,
                           RunStats& stats, std::string* message) {
  int lastWidth = previousWidth;
  int lastHeight = previousHeight;

  for (int poll = 0; poll <= kResizeConfirmPolls; ++poll) {
    int width = 0;
    int height = 0;
    std::string error;

    if (!readPixelSize(window, &width, &height, &error)) {
      *message = error;
      return AwaitResult::Failed;
    }

    if (width != lastWidth || height != lastHeight) {
      std::printf("[resize] poll %2d: window pixels now %dx%d\n", poll, width, height);
      lastWidth = width;
      lastHeight = height;
    }

    if (width == wantWidth && height == wantHeight) {
      ++stats.resizeConfirmations;
      return AwaitResult::Confirmed;
    }

    const PumpResult pump = pumpEvents(stats, "resize");
    if (pump.quitRequested) {
      *message = format("window closed while waiting for %dx%d pixels", wantWidth, wantHeight);
      return AwaitResult::Interrupted;
    }

    SDL_Delay(kResizePollDelayMs);
  }

  *message = format("resize not confirmed within %d polls: wanted %dx%d pixels, window reports %dx%d",
                    kResizeConfirmPolls, wantWidth, wantHeight, lastWidth, lastHeight);
  return AwaitResult::Timeout;
}

FrameResult renderFrame(IDirect3DDevice9* device, D3DCOLOR color,
                        RunStats& stats, std::string* message) {
  HRESULT hr = device->BeginScene();
  if (FAILED(hr)) {
    *message = format("BeginScene failed, hr=%s", hrText(hr).c_str());
    return FrameResult::Failed;
  }

  hr = device->Clear(0, nullptr, D3DCLEAR_TARGET, color, 1.0f, 0);
  if (FAILED(hr)) {
    device->EndScene();
    *message = format("Clear(D3DCLEAR_TARGET, colour 0x%08X) failed, hr=%s",
                      static_cast<unsigned>(color), hrText(hr).c_str());
    return FrameResult::Failed;
  }
  ++stats.clears;

  hr = device->EndScene();
  if (FAILED(hr)) {
    *message = format("EndScene failed, hr=%s", hrText(hr).c_str());
    return FrameResult::Failed;
  }

  hr = device->Present(nullptr, nullptr, nullptr, nullptr);
  if (FAILED(hr)) {
    const char* hint = (hr == D3DERR_DEVICELOST) ? " (D3DERR_DEVICELOST)" : "";
    *message = format("Present failed, hr=%s%s", hrText(hr).c_str(), hint);
    return FrameResult::Failed;
  }

  ++stats.presents;
  ++stats.frames;
  return FrameResult::Presented;
}

const char* verdictText(int exitCode) {
  switch (exitCode) {
    case ExitPass:              return "PASS";
    case ExitFailure:           return "FAIL";
    case ExitUsage:             return "USAGE-ERROR";
    case ExitVulkanUnsupported: return "VULKAN-UNSUPPORTED (no pass claimed)";
    case ExitInterrupted:       return "INTERRUPTED (no pass claimed)";
    default:                    return "UNKNOWN";
  }
}

void printSummary(const RunStats& stats, int exitCode, Uint64 elapsedMs) {
  std::printf("[summary] exit=%d verdict=%s elapsed_ms=%llu\n",
              exitCode, verdictText(exitCode), static_cast<unsigned long long>(elapsedMs));
  std::printf("[summary] frames=%d clears=%d presents=%d resets=%d\n",
              stats.frames, stats.clears, stats.presents, stats.resets);
  std::printf("[summary] resize_requests=%d resize_confirmations=%d backbuffer_checks=%d\n",
              stats.resizeRequests, stats.resizeConfirmations, stats.backBufferChecks);
  std::printf("[summary] checks_passed=%d checks_failed=%d\n",
              stats.checksPassed, stats.checksFailed);
  std::printf("[summary] events quit=%d close=%d pixel_size=%d key=%d other=%d\n",
              stats.quitEvents, stats.closeEvents, stats.pixelSizeEvents,
              stats.keyEvents, stats.otherEvents);
}

void printDriverEnvironment() {
  const char* videoDriver = SDL_GetCurrentVideoDriver();
  std::printf("[env]    pointer_size=%u (32-bit DXVK Native build expects 4)\n",
              static_cast<unsigned>(sizeof(void*)));
  std::printf("[env]    sdl_build=%d.%d.%d sdl_runtime=0x%08X sdl_revision=%s\n",
              SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
              static_cast<unsigned>(SDL_GetVersion()), SDL_GetRevision());
  std::printf("[env]    sdl_video_driver=%s\n", videoDriver != nullptr ? videoDriver : "<none>");
  std::printf("[env]    DXVK_WSI_DRIVER=%s\n",
              SDL_getenv("DXVK_WSI_DRIVER") != nullptr ? SDL_getenv("DXVK_WSI_DRIVER") : "<unset>");

  const char* loaderVars[] = {"VK_LOADER_DRIVERS_SELECT", "VK_ICD_FILENAMES", "VK_DRIVER_FILES"};
  for (const char* name : loaderVars) {
    const char* value = SDL_getenv(name);
    if (value != nullptr && value[0] != '\0')
      std::printf("[env]    %s=%s\n", name, value);
  }
}

int run(const Options& options) {
  RunStats stats;

  SDL_Window*        window = nullptr;
  HWND               hwnd   = nullptr;
  IDirect3D9*        d3d    = nullptr;
  IDirect3DDevice9*  device = nullptr;
  bool               sdlReady = false;
  Uint64             startTicks = 0;

  auto shutdown = [&](int exitCode) {
    // Teardown order is part of the contract: device, then IDirect3D9, then the
    // window, and only then SDL itself.
    std::printf("[shutdown] device -> IDirect3D9 -> window -> SDL\n");

    if (device != nullptr) {
      const ULONG remaining = device->Release();
      device = nullptr;
      std::printf("[shutdown] IDirect3DDevice9 released, remaining refs=%u%s\n",
                  static_cast<unsigned>(remaining),
                  remaining == 0 ? "" : " (non-zero: something still holds a reference)");
    }

    if (d3d != nullptr) {
      const ULONG remaining = d3d->Release();
      d3d = nullptr;
      std::printf("[shutdown] IDirect3D9 released, remaining refs=%u\n",
                  static_cast<unsigned>(remaining));
    }

    if (window != nullptr) {
      SDL_DestroyWindow(window);
      window = nullptr;
      std::printf("[shutdown] SDL window destroyed\n");
    }

    if (sdlReady) {
      SDL_Quit();
      sdlReady = false;
      std::printf("[shutdown] SDL_Quit\n");
    }

    const Uint64 elapsedMs = (startTicks != 0 && SDL_GetTicks() >= startTicks)
                               ? SDL_GetTicks() - startTicks : 0;
    printSummary(stats, exitCode, elapsedMs);
    return exitCode;
  };

  auto report = [](const char* stage, const std::string& message) {
    std::printf("[fail]   stage=%s %s\n", stage, message.c_str());
    return ExitFailure;
  };

  std::printf("=== DXVK Native D3D9 + SDL3 smoke ===\n");
  std::printf("[plan]   frames=%d resize_at=%d (resize + reset) reset_at=%d (reset at same size)\n",
              options.frames, options.resizeAt, options.resetAt);
  std::printf("[plan]   initial=%dx%d resized=%dx%d\n",
              options.width, options.height, options.resizeWidth, options.resizeHeight);

  if (!SDL_Init(SDL_INIT_VIDEO))
    return report("sdl-init", format("SDL_Init(SDL_INIT_VIDEO) failed: %s", SDL_GetError()));
  sdlReady = true;
  startTicks = SDL_GetTicks();

  printDriverEnvironment();

  // SDL refuses to create a Vulkan window off the main thread, and DXVK's SDL3 WSI
  // talks to SDL from the thread that creates the device.
  check(stats, "main-thread", SDL_IsMainThread(), "device and window work happens on the main thread");

  // Pin the WSI backend before any D3D9 object exists: DXVK reads the variable while
  // it brings up the WSI, and it has no fallback on Linux.
  const char* wsiDriver = SDL_getenv("DXVK_WSI_DRIVER");
  if (wsiDriver == nullptr || wsiDriver[0] == '\0') {
    // Single-threaded at this point, so the "unsafe" setter is the intended one.
    if (SDL_setenv_unsafe("DXVK_WSI_DRIVER", kExpectedWsiDriver, 0) != 0)
      return report("wsi-driver", format("SDL_setenv_unsafe(DXVK_WSI_DRIVER) failed: %s", SDL_GetError()));
    std::printf("[wsi]    DXVK_WSI_DRIVER unset, set to %s for this process\n", kExpectedWsiDriver);
  } else if (std::strcmp(wsiDriver, kExpectedWsiDriver) != 0) {
    return report("wsi-driver", format("DXVK_WSI_DRIVER=%s but this build only registers the %s WSI",
                                       wsiDriver, kExpectedWsiDriver));
  }
  check(stats, "wsi-driver", std::strcmp(SDL_getenv("DXVK_WSI_DRIVER"), kExpectedWsiDriver) == 0,
        format("DXVK_WSI_DRIVER=%s", SDL_getenv("DXVK_WSI_DRIVER")));

  // Vulkan preflight. D3D9's adapter string is spoofed by DXVK, so enumerating the real
  // physical devices is the only trustworthy statement about the GPU in use, and it is
  // also what decides between "Vulkan unsupported" and an actual device run.
  if (!SDL_Vulkan_LoadLibrary(nullptr)) {
    std::printf("[vulkan] SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
    std::printf("[vulkan] classification=no-loader\n");
    std::printf("[vulkan] this run is not a pass; a software rasteriser or Wine result must not be substituted\n");
    return shutdown(ExitVulkanUnsupported);
  }

  VulkanProbe probe = probeVulkanDevices();
  classifyVulkanProbe(probe, options.expectGpu);

  std::printf("[vulkan] loader=%s physical_devices=%u%s\n",
              probe.loaderReady ? "ok" : "unavailable",
              static_cast<unsigned>(probe.devices.size()),
              options.expectGpu.empty() ? "" : " (filtered)");
  for (size_t i = 0; i < probe.devices.size(); ++i) {
    std::printf("[vulkan] device %u: \"%s\" type=%s%s\n",
                static_cast<unsigned>(i), probe.devices[i].name.c_str(),
                physicalDeviceTypeName(probe.devices[i].type),
                static_cast<int>(i) == probe.selectedIndex ? "  <- selected" : "");
  }
  if (!options.expectGpu.empty())
    std::printf("[vulkan] expect-gpu=\"%s\"\n", options.expectGpu.c_str());

  if (probe.classification != "ok") {
    std::printf("[vulkan] classification=%s\n", probe.classification.c_str());
    if (probe.classification == "expect-mismatch") {
      std::printf("[vulkan] no non-CPU device matches the requested GPU: this run is not a pass\n");
      return shutdown(ExitFailure);
    }
    std::printf("[vulkan] no usable hardware Vulkan device: stopping before any D3D9 object exists\n");
    std::printf("[vulkan] this run is not a pass; a software rasteriser or Wine result must not be substituted\n");
    return shutdown(ExitVulkanUnsupported);
  }

  const VulkanDeviceInfo& selectedDevice = probe.devices[static_cast<size_t>(probe.selectedIndex)];
  check(stats, "vulkan-devices", !probe.devices.empty(),
        format("%u physical device(s) enumerated", static_cast<unsigned>(probe.devices.size())));
  check(stats, "vulkan-device-hardware", selectedDevice.type != VK_PHYSICAL_DEVICE_TYPE_CPU,
        format("selected \"%s\" type=%s", selectedDevice.name.c_str(),
               physicalDeviceTypeName(selectedDevice.type)));
  if (!options.expectGpu.empty())
    check(stats, "vulkan-expected-gpu", true,
          format("filter \"%s\" matched \"%s\"", options.expectGpu.c_str(),
                 selectedDevice.name.c_str()));

  // Bind DXVK's selection to the hardware device admitted by the preflight;
  // enumerating a device alone does not prove the translator will choose it.
  if (SDL_setenv_unsafe("DXVK_FILTER_DEVICE_NAME", selectedDevice.name.c_str(), 1) != 0)
    return shutdown(report("device-filter", "could not set DXVK device filter"));
  std::printf("[vulkan] DXVK_FILTER_DEVICE_NAME=\"%s\"\n", selectedDevice.name.c_str());

  window = SDL_CreateWindow("KisakCOD DXVK native D3D9 smoke",
                            options.width, options.height,
                            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
  if (window == nullptr)
    return shutdown(report("create-window", format("SDL_CreateWindow failed: %s", SDL_GetError())));

  check(stats, "window-vulkan",
        (SDL_GetWindowFlags(window) & SDL_WINDOW_VULKAN) != 0,
        "window created with SDL_WINDOW_VULKAN");
  check(stats, "window-resizable",
        (SDL_GetWindowFlags(window) & SDL_WINDOW_RESIZABLE) != 0,
        "window created with SDL_WINDOW_RESIZABLE");
  check(stats, "window-visible",
        (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) == 0,
        "window is visible (SDL_WINDOW_HIDDEN not set)");

  hwnd = dxvk::wsi::toHwnd(window);
  if (hwnd == nullptr)
    return shutdown(report("wsi-handle", "dxvk::wsi::toHwnd returned a null HWND"));

  int initialPixelWidth  = 0;
  int initialPixelHeight = 0;
  std::string error;
  if (!readPixelSize(window, &initialPixelWidth, &initialPixelHeight, &error))
    return shutdown(report("window-pixels", error));

  std::printf("[window] requested=%dx%d points, pixels=%dx%d\n",
              options.width, options.height, initialPixelWidth, initialPixelHeight);

  // D3D9 object creation.
  d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (d3d == nullptr) {
    return shutdown(report("direct3dcreate9",
      "Direct3DCreate9 returned null; check d3d9.log, DXVK_WSI_DRIVER and the DXVK native prefix"));
  }

  const UINT adapterCount = d3d->GetAdapterCount();
  check(stats, "adapter-count", adapterCount > 0,
        format("GetAdapterCount()=%u", static_cast<unsigned>(adapterCount)));

  D3DADAPTER_IDENTIFIER9 identifier;
  std::memset(&identifier, 0, sizeof(identifier));
  HRESULT hr = d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &identifier);
  if (SUCCEEDED(hr)) {
    std::printf("[adapter] description=\"%s\" vendor=0x%04X device=0x%04X\n",
                identifier.Description, static_cast<unsigned>(identifier.VendorId),
                static_cast<unsigned>(identifier.DeviceId));
    std::printf("[adapter] D3D9 hides/spoofs the real GPU by default; the enumerated Vulkan "
                "device above is authoritative\n");
  }
  check(stats, "adapter-identifier", SUCCEEDED(hr),
        SUCCEEDED(hr) ? format("adapter 0 is \"%s\"", identifier.Description)
                      : format("GetAdapterIdentifier failed, hr=%s", hrText(hr).c_str()));

  hr = d3d->CheckDeviceType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                            D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE);
  check(stats, "device-type-x8r8g8b8", SUCCEEDED(hr),
        SUCCEEDED(hr) ? "HAL accepts windowed X8R8G8B8"
                      : format("CheckDeviceType(HAL, X8R8G8B8, windowed) failed, hr=%s",
                               hrText(hr).c_str()));

  D3DPRESENT_PARAMETERS presentParams =
    makePresentParameters(hwnd, initialPixelWidth, initialPixelHeight);
  std::printf("[device] CreateDevice(HAL, HardwareVertexProcessing) %s\n",
              describePresentParameters(presentParams).c_str());

  hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                         D3DCREATE_HARDWARE_VERTEXPROCESSING,
                         &presentParams, &device);
  if (FAILED(hr) || device == nullptr)
    return shutdown(report("create-device",
      format("CreateDevice failed, hr=%s; see d3d9.log for the Vulkan side", hrText(hr).c_str())));

  const PresentParametersInfo accepted = readPresentParameters(device);
  check(stats, "present-params",
        accepted.valid
          && accepted.params.BackBufferFormat == D3DFMT_X8R8G8B8
          && accepted.params.SwapEffect == D3DSWAPEFFECT_DISCARD
          && accepted.params.BackBufferCount == 1
          && accepted.params.Windowed == TRUE,
        accepted.valid ? describePresentParameters(accepted.params) : accepted.error);

  BackBufferInfo initialBackBuffer = readBackBufferInfo(device);
  ++stats.backBufferChecks;
  const bool initialBackBufferMatches =
    initialBackBuffer.valid
      && initialBackBuffer.desc.Width == static_cast<UINT>(initialPixelWidth)
      && initialBackBuffer.desc.Height == static_cast<UINT>(initialPixelHeight)
      && initialBackBuffer.desc.Format == D3DFMT_X8R8G8B8;
  check(stats, "backbuffer-initial", initialBackBufferMatches,
        format("%s, window pixels %dx%d", describeBackBuffer(initialBackBuffer).c_str(),
               initialPixelWidth, initialPixelHeight));
  if (!initialBackBufferMatches)
    return shutdown(ExitFailure);

  // Frame loop. Exactly one bounded resize request and one explicit reset happen at
  // the frame indices given on the command line.
  int  stage = 0;
  bool quitRequested = false;
  int  resizedPixelWidth  = 0;
  int  resizedPixelHeight = 0;

  for (int frame = 1; frame <= options.frames; ++frame) {
    if (pumpEvents(stats, stageName(stage)).quitRequested) {
      std::printf("[loop]   stop requested at frame %d/%d\n", frame, options.frames);
      quitRequested = true;
      break;
    }

    if (frame == options.resizeAt) {
      ++stats.resizeRequests;

      // Before the request the back buffer still has to describe the size the device
      // was created with; D3D9 only changes it on Reset.
      BackBufferInfo before = readBackBufferInfo(device);
      ++stats.backBufferChecks;
      const bool stable = before.valid
        && before.desc.Width == static_cast<UINT>(initialPixelWidth)
        && before.desc.Height == static_cast<UINT>(initialPixelHeight);
      check(stats, "backbuffer-pre-resize", stable,
            format("%s, expected %dx%d before the resize request",
                   describeBackBuffer(before).c_str(), initialPixelWidth, initialPixelHeight));
      if (!stable)
        return shutdown(ExitFailure);

      int pointsWidth = 0;
      int pointsHeight = 0;
      if (!SDL_GetWindowSize(window, &pointsWidth, &pointsHeight))
        return shutdown(report("resize-points", format("SDL_GetWindowSize failed: %s", SDL_GetError())));

      // The resize request is expressed in points; the swap chain needs pixels, so
      // derive the current scale from the window SDL already reported.
      const double scaleX = pointsWidth > 0
                              ? static_cast<double>(initialPixelWidth) / static_cast<double>(pointsWidth) : 1.0;
      const double scaleY = pointsHeight > 0
                              ? static_cast<double>(initialPixelHeight) / static_cast<double>(pointsHeight) : 1.0;
      const int wantPixelWidth  = static_cast<int>(std::lround(options.resizeWidth * scaleX));
      const int wantPixelHeight = static_cast<int>(std::lround(options.resizeHeight * scaleY));

      std::printf("[resize] frame %d: SDL_SetWindowSize(%d, %d) -> expecting %dx%d pixels\n",
                  frame, options.resizeWidth, options.resizeHeight, wantPixelWidth, wantPixelHeight);

      if (!SDL_SetWindowSize(window, options.resizeWidth, options.resizeHeight))
        return shutdown(report("resize-request", format("SDL_SetWindowSize failed: %s", SDL_GetError())));

      std::string message;
      const AwaitResult awaited = awaitPixelSize(window, wantPixelWidth, wantPixelHeight,
                                                 initialPixelWidth, initialPixelHeight,
                                                 stats, &message);
      if (awaited == AwaitResult::Interrupted)
        return shutdown(ExitInterrupted);
      if (awaited == AwaitResult::Failed)
        return shutdown(report("resize-confirm", message));
      if (awaited == AwaitResult::Timeout) {
        // Without a confirmed pixel size an immediate Reset would describe the wrong
        // surface, so the run stops here rather than guessing.
        return shutdown(report("resize-confirm", message));
      }

      if (!readPixelSize(window, &resizedPixelWidth, &resizedPixelHeight, &message))
        return shutdown(report("resize-pixels", message));

      check(stats, "resize-pixels",
            resizedPixelWidth == wantPixelWidth && resizedPixelHeight == wantPixelHeight,
            format("window pixels %dx%d after resize (expected %dx%d)",
                   resizedPixelWidth, resizedPixelHeight, wantPixelWidth, wantPixelHeight));

      check(stats, "resize-changed",
            resizedPixelWidth != initialPixelWidth || resizedPixelHeight != initialPixelHeight,
            format("%dx%d differs from the initial %dx%d", resizedPixelWidth, resizedPixelHeight,
                   initialPixelWidth, initialPixelHeight));

      std::printf("[resize] confirmed in %d poll(s), window pixels %dx%d\n",
                  stats.resizeConfirmations, resizedPixelWidth, resizedPixelHeight);

      // Reset straight after the confirmed resize, while the run keeps going: the swap
      // chain is rebuilt for the new pixel size here rather than at the end.
      std::string resetFailure;
      if (!resetWithFreshParameters(device, hwnd, resizedPixelWidth, resizedPixelHeight,
                                    "resize-reset", stats, &resetFailure))
        return shutdown(report("resize-reset", resetFailure));

      stage = 1;
      SDL_SetWindowTitle(window, "DXVK native D3D9 smoke: resized and reset");
    }

    if (frame == options.resetAt) {
      int pixelWidth = 0;
      int pixelHeight = 0;
      std::string message;
      if (!readPixelSize(window, &pixelWidth, &pixelHeight, &message))
        return shutdown(report("same-size-reset-read-size", message));

      // The window must not have drifted between the resize and this reset, otherwise
      // this stops being a same-size reset.
      check(stats, "same-size-reset-size",
            resizedPixelWidth > 0 && pixelWidth == resizedPixelWidth && pixelHeight == resizedPixelHeight,
            format("window pixels %dx%d (resize confirmed %dx%d)",
                   pixelWidth, pixelHeight, resizedPixelWidth, resizedPixelHeight));

      // Separate reset at the unchanged size: exercises the reset path without a
      // resize behind it.
      std::string resetFailure;
      if (!resetWithFreshParameters(device, hwnd, pixelWidth, pixelHeight,
                                    "same-size-reset", stats, &resetFailure))
        return shutdown(report("same-size-reset", resetFailure));

      stage = 2;
      SDL_SetWindowTitle(window, "DXVK native D3D9 smoke: same-size reset");
    }

    std::string frameMessage;
    if (renderFrame(device, stageColor(stage), stats, &frameMessage) != FrameResult::Presented)
      return shutdown(report(stageName(stage), frameMessage));

    if (frame == 1 || frame % 30 == 0 || frame == options.frames)
      std::printf("[frame]  %3d/%d stage=%s colour=0x%08X\n",
                  frame, options.frames, stageName(stage),
                  static_cast<unsigned>(stageColor(stage)));
  }

  if (quitRequested) {
    std::printf("[result] interrupted after %d frame(s): partial run, not a pass\n", stats.frames);
    return shutdown(ExitInterrupted);
  }

  check(stats, "frame-budget", stats.frames == options.frames,
        format("presented %d of %d frames", stats.frames, options.frames));
  check(stats, "resets-performed", stats.resets == 2,
        format("%d Reset call(s): one after the confirmed resize, one at the unchanged size",
               stats.resets));

  if (stats.checksFailed != 0) {
    std::printf("[result] %d check(s) failed\n", stats.checksFailed);
    return shutdown(ExitFailure);
  }

  std::printf("[result] PASS: %d frame(s) presented, %d check(s) passed\n",
              stats.frames, stats.checksPassed);
  return shutdown(ExitPass);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;

  // The expected GPU can be pinned from the environment so a CI job does not have to
  // thread the flag through its command line.
  if (const char* expectGpu = std::getenv("KDXK_SMOKE_EXPECT_GPU"); expectGpu != nullptr)
    options.expectGpu = expectGpu;

  bool helpRequested = false;

  const int parsed = parseArgs(argc, argv, options, &helpRequested);
  if (helpRequested) {
    printUsage(stdout);
    return ExitUsage;
  }
  if (parsed != ExitPass) {
    printUsage(stderr);
    return ExitUsage;
  }

  return run(options);
}

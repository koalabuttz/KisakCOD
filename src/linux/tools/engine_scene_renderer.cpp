// The free camera's SceneRenderer over the engine renderer: see
// engine_scene_renderer.h and docs/linux-free-camera.md.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <database/database.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_buffers.h>
#include <gfx_d3d/r_cinematic.h>
#include <gfx_d3d/r_dvars.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_model_lighting.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_scene.h>
#include <gfx_d3d/rb_sky.h>
#include <linux/d3d9_types.h>

#include "engine_scene_renderer.h"
#include "world_surface_export.h"

#include <exception>
#include <stdexcept>

// r_init.cpp defines this without a header: R_RecoverLostDevice is its only
// caller.
void R_ResetDevice();

namespace kisak_engine_view
{
namespace
{
namespace wde = kisak_world_draw_engine;
using kisak::view::CameraView;
using kisak::view::ViewExtent;

std::string ExtentText(ViewExtent extent)
{
    return std::to_string(extent.width) + "x" + std::to_string(extent.height);
}

// R_RecoverLostDevice (r_init.cpp) from its log line on, call for call. Its gate,
// R_CanRecoverLostDevice, asserts that the device reports itself lost, so the
// sequence is repeated here instead; dx.deviceLost is set as R_CheckLostDevice
// sets it before calling it, and R_ResetDevice clears it.
void RecoverDevice()
{
    wde::Require(dx.device && gfxBuf.dynamicVertexBuffer && gfxBuf.dynamicVertexBuffer->buffer &&
                     gfxBuf.dynamicIndexBuffer && gfxBuf.dynamicIndexBuffer->buffer,
                 "reset", "the renderer has no device or dynamic buffers to reset");
    dx.deviceLost = 1;
    const int remoteScreenUpdateNesting = R_PopRemoteScreenUpdate();
    R_SyncRenderThread();
    R_Cinematic_BeginLostDevice();
    DB_BeginRecoverLostDevice();
    R_ResetModelLighting();
    R_ReleaseLostImages();
    Material_ReleaseAll();
    R_ReleaseWorld();
    R_ResetDevice();
    R_ReloadWorld();
    Material_ReloadAll();
    R_ReloadLostImages();
    dx.sunSpriteSamples = RB_CalcSunSpriteSamples();
    DB_EndRecoverLostDevice();
    R_Cinematic_EndLostDevice();
    R_PushRemoteScreenUpdate(remoteScreenUpdateNesting);
}
} // namespace

EngineSceneRenderer::EngineSceneRenderer(ViewExtent extent, bool readBack, bool effects)
    : extent_(extent), readBack_(readBack), refdef_(std::make_unique<refdef_s>()),
      effects_(effects ? std::make_unique<wde::MapEffects>() : nullptr)
{
    wde::RequireStartedRenderer();
    wde::Require(!extent.Empty(), "renderer", "the renderer must start at a real extent");
    wde::Require(vidConfig.displayWidth == extent.width && vidConfig.displayHeight == extent.height &&
                     vidConfig.sceneWidth == extent.width && vidConfig.sceneHeight == extent.height,
                 "renderer", "R_Init did not start at " + ExtentText(extent));
    wde::Require(!vidConfig.isFullscreen && dx.windowCount == 1 && dx.windows[0].hwnd, "renderer",
                 "the renderer is not one windowed view");
    wde::Require(!wde::MapHeld(), "renderer", "a map is loaded before the adapter took the renderer");
    // R_LoadWorld would start a light visibility log that only R_Shutdown saves.
    wde::Require(r_vc_makelog && !r_vc_makelog->current.integer, "renderer", "r_vc_makelog is on");
    wde::PresentEmptyFrame();
    ++counters_.emptyFrames;
    wde::Require(wde::PreTessBuffersAgree(), "renderer",
                 "after the first empty frame the backend's pre-tessellated index buffer is still not the front end's");
}

EngineSceneRenderer::~EngineSceneRenderer() = default;

void *EngineSceneRenderer::Window() const
{
    return shutdown_ ? nullptr : static_cast<void *>(dx.windows[0].hwnd);
}

void EngineSceneRenderer::RequireState(const char *stage) const
{
    wde::Require(!shutdown_, stage, "the renderer is shut down");
    wde::Require(dx.device != nullptr, stage, "the renderer has no device");
}

bool EngineSceneRenderer::LoadMap(const std::string &map, std::string &error)
{
    try
    {
        RequireState("world");
        wde::Require(!mapLoaded_, "world", "a map is already loaded");
        wde::Require(kisak_world_inventory::IsAllowedMapName(map.c_str()), "world",
                     "'" + map + "' is not an allowlisted map");
        try
        {
            worldName_ = wde::LoadMap(map.c_str());
            if (effects_)
                effects_->Start(map.c_str());
        }
        catch (const std::exception &failure)
        {
            // Nothing of a failed map stays held. If that fails too, the next load is
            // refused, because the map is still held.
            std::string reason = failure.what();
            if (effects_)
                effects_->Stop();
            if (wde::MapHeld())
            {
                try
                {
                    wde::UnloadMap();
                }
                catch (const std::exception &cleanup)
                {
                    reason += "; releasing what had loaded then failed: " + std::string(cleanup.what());
                }
            }
            throw std::runtime_error(reason);
        }
        mapLoaded_ = true;
        ++counters_.loads;
        return true;
    }
    catch (const std::exception &failure)
    {
        error = failure.what();
        worldName_.clear();
        return false;
    }
}

void EngineSceneRenderer::UnloadMap() noexcept
{
    if (!mapLoaded_)
        return;
    mapLoaded_ = false;
    worldName_.clear();
    try
    {
        // CG_Shutdown stops the effects before the map goes.
        if (effects_)
            effects_->Stop();
        wde::UnloadMap();
        ++counters_.unloads;
    }
    catch (const std::exception &failure)
    {
        problem_ = failure.what();
    }
}

void EngineSceneRenderer::ResetAtExtent(ViewExtent extent)
{
    RecoverDevice();
    wde::Require(!dx.deviceLost, "reset", "the device is still marked lost after the reset");
    uint32_t width = 0, height = 0;
    wde::BackBufferSize(width, height);
    wde::Require(width == extent.width && height == extent.height, "reset",
                 "the back buffer is " + std::to_string(width) + "x" + std::to_string(height) + ", not " +
                     ExtentText(extent));
    // R_ResetDevice's R_ReleaseForShutdownOrReset cleared the frame data's
    // pre-tessellated index buffer, as R_Init leaves it.
    wde::PresentEmptyFrame();
    ++counters_.emptyFrames;
    wde::Require(wde::PreTessBuffersAgree(), "reset",
                 "after the empty frame the backend's pre-tessellated index buffer is still not the front end's");
}

bool EngineSceneRenderer::Resize(ViewExtent extent, std::string &error)
{
    try
    {
        RequireState("resize");
        wde::Require(!extent.Empty(), "resize", "an empty extent is a minimized window, not a size");
        D3DCAPS9 caps{};
        const HRESULT hr = dx.device->GetDeviceCaps(&caps);
        wde::Require(SUCCEEDED(hr), "resize", "GetDeviceCaps failed: " + wde::Hex(uint32_t(hr)));
        // Every render target is a texture of the display's size (R_InitRenderTargets).
        wde::Require(extent.width <= caps.MaxTextureWidth && extent.height <= caps.MaxTextureHeight, "resize",
                     ExtentText(extent) + " is larger than the device's largest texture (" +
                         std::to_string(caps.MaxTextureWidth) + "x" + std::to_string(caps.MaxTextureHeight) + ")");
        // R_ResetDevice reads the size from dx.windows[0]; the render targets and the
        // scene viewport read it from the video config, as R_Init stored it.
        dx.windows[0].width = int(extent.width);
        dx.windows[0].height = int(extent.height);
        GfxWindowParms parms{};
        parms.hwnd = dx.windows[0].hwnd;
        parms.hz = vidConfig.displayFrequency;
        parms.fullscreen = vidConfig.isFullscreen != 0;
        parms.x = 0;
        parms.y = 0;
        parms.sceneWidth = int(extent.width);
        parms.sceneHeight = int(extent.height);
        parms.displayWidth = int(extent.width);
        parms.displayHeight = int(extent.height);
        parms.aaSamples = r_aaSamples->current.integer;
        R_StoreWindowSettings(&parms);
        extent_ = extent;
        ResetAtExtent(extent);
        wde::Require(vidConfig.sceneWidth == extent.width && vidConfig.sceneHeight == extent.height &&
                         vidConfig.displayWidth == extent.width && vidConfig.displayHeight == extent.height,
                     "resize", "the video config is not " + ExtentText(extent) + " after the reset");
        ++counters_.resizes;
        return true;
    }
    catch (const std::exception &failure)
    {
        error = failure.what();
        return false;
    }
}

kisak::view::SceneRenderer::DeviceStatus EngineSceneRenderer::Device()
{
    RequireState("device");
    if (resetRequested_)
    {
        resetRequested_ = false;
        return DeviceStatus::NeedsReset;
    }
    const HRESULT level = dx.device->TestCooperativeLevel();
    if (level == D3D_OK)
        return DeviceStatus::Ready;
    if (level == D3DERR_DEVICELOST)
        return DeviceStatus::Lost;
    if (level == D3DERR_DEVICENOTRESET)
        return DeviceStatus::NeedsReset;
    // Anything else is not one of TestCooperativeLevel's answers; the owner fails
    // closed on the exception.
    wde::Fail("device", "TestCooperativeLevel returned " + wde::Hex(uint32_t(level)));
}

kisak::view::SceneRenderer::ResetResult EngineSceneRenderer::ResetDevice(std::string &error)
{
    try
    {
        RequireState("reset");
        // R_CanRecoverLostDevice: a device that is still lost cannot be reset yet.
        if (dx.device->TestCooperativeLevel() == D3DERR_DEVICELOST)
            return ResetResult::StillLost;
        ResetAtExtent(extent_);
        ++counters_.resets;
        return ResetResult::Reset;
    }
    catch (const std::exception &failure)
    {
        error = failure.what();
        return ResetResult::Failed;
    }
}

bool EngineSceneRenderer::DrawFrame(const CameraView &view, std::string &error)
{
    try
    {
        RequireState("frame");
        wde::Require(mapLoaded_ && rgp.world, "frame", "no map is loaded");
        // The adapter draws one view over the whole scene, as the probe does; a
        // split or inset view would need its own scissor and viewport rules.
        wde::Require(view.x == 0 && view.y == 0 && view.width == extent_.width && view.height == extent_.height,
                     "frame",
                     "the camera view " + std::to_string(view.width) + "x" + std::to_string(view.height) + " at " +
                         std::to_string(view.x) + "," + std::to_string(view.y) + " is not the whole " +
                         ExtentText(extent_) + " scene");
        wde::Require(vidConfig.sceneWidth == extent_.width && vidConfig.sceneHeight == extent_.height, "frame",
                     "the video config's scene is not " + ExtentText(extent_));
        wde::PrepareRefdef(*refdef_);
        kisak::view::FillRefdef(view, *refdef_);
        if (effects_)
        {
            last_ = wde::DrawRefdef(*refdef_, readBack_, {},
                                    [this](const refdef_s &refdef) { effects_->Update(refdef); });
            effects_->Count(last_.effects);
        }
        else
            last_ = wde::DrawRefdef(*refdef_, readBack_);
        ++counters_.draws;
        return true;
    }
    catch (const std::exception &failure)
    {
        error = failure.what();
        return false;
    }
}

void EngineSceneRenderer::Shutdown() noexcept
{
    if (shutdown_)
        return;
    UnloadMap();
    shutdown_ = true;
    // CL_ShutdownRenderer(1) without the world parts UnloadMap already released:
    // the renderer, its graphics zones (R_UnloadGraphicsAssets), the window and
    // the device.
    R_Shutdown(1);
}

} // namespace kisak_engine_view

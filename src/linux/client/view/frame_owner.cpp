#include "frame_owner.h"

#include <cmath>
#include <exception>
#include <stdexcept>

namespace kisak::view {

const char *FrameStateName(FrameState state)
{
    switch (state) {
    case FrameState::NoMap: return "no-map";
    case FrameState::Ready: return "ready";
    case FrameState::DeviceLost: return "device-lost";
    case FrameState::Failed: return "failed";
    case FrameState::Stopped: return "stopped";
    }
    return "unknown";
}

const char *FrameOutcomeName(FrameOutcome outcome)
{
    switch (outcome) {
    case FrameOutcome::Drawn: return "drawn";
    case FrameOutcome::NoMap: return "no-map";
    case FrameOutcome::Minimized: return "minimized";
    case FrameOutcome::DeviceLost: return "device-lost";
    case FrameOutcome::Failed: return "failed";
    case FrameOutcome::Stopped: return "stopped";
    }
    return "unknown";
}

FrameOwner::FrameOwner(SceneRenderer &renderer, ViewExtent extent, const CameraTuning &tuning)
    : renderer_(renderer), camera_(tuning), applied_(extent), pending_(extent),
      thread_(std::this_thread::get_id())
{
}

FrameOwner::~FrameOwner()
{
    Shutdown();
}

void FrameOwner::CheckCaller(const char *operation) const
{
    if (std::this_thread::get_id() != thread_)
        throw std::logic_error(std::string("Free camera ") + operation + " called from a foreign thread");
    if (inFrame_)
        throw std::logic_error(std::string("Free camera ") + operation + " called from inside a frame");
}

void FrameOwner::Fail(const std::string &what)
{
    error_ = what;
    state_ = FrameState::Failed;
    controls_.ReleaseAll();
}

void FrameOwner::UnloadNoexcept() noexcept
{
    if (!mapLoaded_)
        return;
    mapLoaded_ = false;
    ++counters_.unloads;
    renderer_.UnloadMap();
}

bool FrameOwner::Load(const std::string &map, const CameraPose *start)
{
    CheckCaller("load");
    if (state_ == FrameState::Stopped || state_ == FrameState::Failed) {
        if (error_.empty())
            error_ = "Free camera owner is stopped";
        return false;
    }
    if (map.empty())
        throw std::invalid_argument("Free camera load needs a map name");
    if (start)
        camera_.SetPose(*start); // validate before touching the current map
    UnloadNoexcept();
    controls_.ReleaseAll();
    state_ = FrameState::NoMap;
    std::string error;
    bool loaded = false;
    try {
        loaded = renderer_.LoadMap(map, error);
    } catch (const std::exception &failure) {
        error = failure.what();
    }
    if (!loaded) {
        ++counters_.loadFailures;
        error_ = error.empty() ? "Map load failed without a reason" : error;
        map_.clear();
        return false;
    }
    ++counters_.loads;
    mapLoaded_ = true;
    map_ = map;
    error_.clear();
    state_ = FrameState::Ready;
    return true;
}

bool FrameOwner::Reload()
{
    CheckCaller("reload");
    if (!mapLoaded_) {
        if (state_ != FrameState::Failed && state_ != FrameState::Stopped)
            error_ = "No map to reload";
        return false;
    }
    const std::string map = map_;
    return Load(map);
}

void FrameOwner::Unload()
{
    CheckCaller("unload");
    UnloadNoexcept();
    map_.clear();
    controls_.ReleaseAll();
    if (state_ == FrameState::Ready || state_ == FrameState::DeviceLost)
        state_ = FrameState::NoMap;
}

void FrameOwner::Resize(ViewExtent extent)
{
    CheckCaller("resize");
    pending_ = extent;
    pendingResize_ = true;
}

void FrameOwner::Focus(bool focused)
{
    focused_ = focused;
    if (!focused)
        controls_.ReleaseAll();
}

void FrameOwner::SetLookCaptured(bool captured)
{
    lookCaptured_ = captured;
    if (!captured)
        (void)controls_.TakeLook();
}

bool FrameOwner::Key(int key, bool down)
{
    if (down && !focused_)
        return false;
    return controls_.Key(key, down);
}

void FrameOwner::Mouse(int dx, int dy)
{
    if (focused_ && lookCaptured_)
        controls_.Mouse(dx, dy);
}

void FrameOwner::SetAspect(float aspect)
{
    if (!std::isfinite(aspect))
        throw std::invalid_argument("Free camera aspect must be finite");
    aspect_ = aspect;
}

FrameOutcome FrameOwner::Frame(int msec, int timeMilliseconds)
{
    CheckCaller("frame");
    if (msec < 0)
        throw std::invalid_argument("Free camera frame time must not be negative");
    if (state_ == FrameState::Stopped)
        return FrameOutcome::Stopped;
    ++counters_.frames;
    if (state_ == FrameState::Failed) {
        // Nothing is stepped or drawn after failure; drop input so nothing waits.
        controls_.ReleaseAll();
        return FrameOutcome::Failed;
    }
    inFrame_ = true;
    FrameOutcome outcome;
    try {
        outcome = RunFrame(msec, timeMilliseconds);
    } catch (const std::exception &failure) {
        Fail(failure.what());
        outcome = FrameOutcome::Failed;
    } catch (...) {
        Fail("Unknown renderer exception");
        outcome = FrameOutcome::Failed;
    }
    inFrame_ = false;
    if (shutdownRequested_) {
        FinishShutdown();
        return FrameOutcome::Stopped;
    }
    return outcome;
}

FrameOutcome FrameOwner::RunFrame(int msec, int timeMilliseconds)
{
    // Input first, always: counts and steps taken without a map are discarded so a
    // load never starts with a burst that arrived while it was loading.
    const FreeCameraControls::Look look = controls_.TakeLook();
    const MoveIntent intent = focused_ ? controls_.Intent() : MoveIntent{};
    if (mapLoaded_) {
        if (look.dx || look.dy)
            camera_.Look(static_cast<float>(look.dx), static_cast<float>(look.dy));
        camera_.Step(intent, msec);
    }

    switch (renderer_.Device()) {
    case SceneRenderer::DeviceStatus::Ready:
        break;
    case SceneRenderer::DeviceStatus::Lost:
        if (mapLoaded_)
            state_ = FrameState::DeviceLost;
        return FrameOutcome::DeviceLost;
    case SceneRenderer::DeviceStatus::NeedsReset: {
        std::string error;
        switch (renderer_.ResetDevice(error)) {
        case SceneRenderer::ResetResult::Reset:
            ++counters_.resets;
            break;
        case SceneRenderer::ResetResult::StillLost:
            ++counters_.resetRetries;
            if (mapLoaded_)
                state_ = FrameState::DeviceLost;
            return FrameOutcome::DeviceLost;
        case SceneRenderer::ResetResult::Failed:
            Fail(error.empty() ? "Device reset failed without a reason" : error);
            return FrameOutcome::Failed;
        }
        break;
    }
    }
    state_ = mapLoaded_ ? FrameState::Ready : FrameState::NoMap;

    if (pendingResize_ && !pending_.Empty()) {
        pendingResize_ = false;
        if (!(pending_ == applied_)) {
            std::string error;
            if (!renderer_.Resize(pending_, error)) {
                Fail(error.empty() ? "Resize failed without a reason" : error);
                return FrameOutcome::Failed;
            }
            ++counters_.resizes;
            applied_ = pending_;
        }
    }
    // A pending empty extent is a minimized window: keep the last real extent for
    // the renderer and draw nothing until a non-empty extent arrives.
    if ((pendingResize_ && pending_.Empty()) || applied_.Empty())
        return FrameOutcome::Minimized;
    if (!mapLoaded_)
        return FrameOutcome::NoMap;

    const CameraView view = camera_.View(applied_, timeMilliseconds, aspect_);
    std::string error;
    if (!renderer_.DrawFrame(view, error)) {
        Fail(error.empty() ? "Draw failed without a reason" : error);
        return FrameOutcome::Failed;
    }
    ++counters_.draws;
    return FrameOutcome::Drawn;
}

void FrameOwner::FinishShutdown() noexcept
{
    if (shutdownDone_)
        return;
    shutdownDone_ = true;
    UnloadNoexcept();
    controls_.ReleaseAll();
    renderer_.Shutdown();
    state_ = FrameState::Stopped;
}

void FrameOwner::Shutdown() noexcept
{
    if (inFrame_) {
        // Requested by the renderer during its own call: finish when it returns.
        shutdownRequested_ = true;
        return;
    }
    FinishShutdown();
}

} // namespace kisak::view

#include "free_camera.h"

#include <universal/com_math.h>

#include <cmath>
#include <stdexcept>
#include <string>

namespace kisak::view {
namespace {
void RequireFinite(float value, const char *name)
{
    if (!std::isfinite(value))
        throw std::invalid_argument(std::string("Free camera ") + name + " must be finite");
}

void RequireRange(float value, float low, float high, const char *name)
{
    RequireFinite(value, name);
    if (value < low || value > high)
        throw std::invalid_argument(std::string("Free camera ") + name + " is out of range");
}

void RequireAxis(int value, const char *name)
{
    if (value < -1 || value > 1)
        throw std::invalid_argument(std::string("Free camera ") + name + " must be -1, 0 or +1");
}

// Wrap to [-180, 180). fmod keeps large inputs exact enough; the result never
// accumulates drift because every step wraps.
float WrapDegrees(float angle)
{
    float wrapped = std::fmod(angle + 180.0f, 360.0f);
    if (wrapped < 0.0f)
        wrapped += 360.0f;
    return wrapped - 180.0f;
}
} // namespace

void ValidateTuning(const CameraTuning &t)
{
    RequireRange(t.sensitivity, 0.0f, 1000.0f, "sensitivity");
    RequireRange(t.yawPerCount, -10.0f, 10.0f, "yaw per count");
    RequireRange(t.pitchPerCount, -10.0f, 10.0f, "pitch per count");
    RequireRange(t.yawKeySpeed, 0.0f, 3600.0f, "yaw key speed");
    RequireRange(t.pitchKeySpeed, 0.0f, 3600.0f, "pitch key speed");
    RequireRange(t.pitchUpLimit, 0.0f, 90.0f, "pitch up limit");
    RequireRange(t.pitchDownLimit, 0.0f, 90.0f, "pitch down limit");
    RequireRange(t.moveSpeed, 0.0f, 100000.0f, "move speed");
    RequireRange(t.fastScale, 1.0f, 100.0f, "fast scale");
    // cg_fov's registered range.
    RequireRange(t.fovX, 1.0f, 160.0f, "field of view");
    RequireRange(t.zNear, 0.0f, 1000.0f, "near plane");
    if (t.maxStepMilliseconds < 1 || t.maxStepMilliseconds > 1000)
        throw std::invalid_argument("Free camera step cap must be 1..1000 milliseconds");
    RequireRange(t.originLimit, 1.0f, 1048576.0f, "origin limit");
}

void ProjectionForFov(float fovX, float aspect, float &tanHalfFovX, float &tanHalfFovY)
{
    RequireRange(fovX, 1.0f, 160.0f, "field of view");
    RequireFinite(aspect, "aspect");
    if (aspect <= 0.0f)
        throw std::invalid_argument("Free camera aspect must be positive");
    // Same operation order and float narrowing as CG_UpdateFov.
    const float dxDzAtDefaultAspectRatio = std::tan(fovX * (static_cast<float>(M_PI) / 180.0f) * 0.5);
    const float dyDz = dxDzAtDefaultAspectRatio * 0.75;
    const float dxDz = dyDz * aspect;
    tanHalfFovX = dxDz;
    tanHalfFovY = dyDz;
}

FreeCamera::FreeCamera(const CameraTuning &tuning, const CameraPose &pose) : tuning_(tuning)
{
    ValidateTuning(tuning_);
    SetPose(pose);
}

void FreeCamera::SetPose(const CameraPose &pose)
{
    for (int i = 0; i < 3; ++i) {
        RequireFinite(pose.origin[i], "origin");
        RequireFinite(pose.angles[i], "angle");
    }
    pose_ = pose;
    // A supplied pitch is an orientation, so 350 means -10 before the limits apply.
    pose_.angles[0] = WrapDegrees(pose_.angles[0]);
    Normalize();
}

void FreeCamera::Normalize()
{
    // Input pitch is a turn, not an orientation: it is clamped without wrapping, so
    // a large look-down burst stops at the limit instead of wrapping to look up.
    float pitch = pose_.angles[0];
    if (pitch < -tuning_.pitchUpLimit)
        pitch = -tuning_.pitchUpLimit;
    if (pitch > tuning_.pitchDownLimit)
        pitch = tuning_.pitchDownLimit;
    pose_.angles[0] = pitch;
    pose_.angles[1] = WrapDegrees(pose_.angles[1]);
    pose_.angles[2] = WrapDegrees(pose_.angles[2]);
    for (float &axis : pose_.origin) {
        if (axis < -tuning_.originLimit)
            axis = -tuning_.originLimit;
        if (axis > tuning_.originLimit)
            axis = tuning_.originLimit;
    }
}

void FreeCamera::Look(float countsX, float countsY)
{
    RequireFinite(countsX, "mouse x");
    RequireFinite(countsY, "mouse y");
    // CL_MouseMove with cl_mouseAccel 0, no strafe button and freelook on.
    const float mx = countsX * tuning_.sensitivity;
    const float my = countsY * tuning_.sensitivity;
    pose_.angles[1] -= tuning_.yawPerCount * mx;
    pose_.angles[0] += tuning_.pitchPerCount * my;
    Normalize();
}

void FreeCamera::Step(const MoveIntent &intent, int msec)
{
    RequireAxis(intent.forward, "forward");
    RequireAxis(intent.right, "right");
    RequireAxis(intent.up, "up");
    RequireAxis(intent.yaw, "yaw");
    RequireAxis(intent.pitch, "pitch");
    if (msec < 0)
        throw std::invalid_argument("Free camera step must not be negative");
    if (msec > tuning_.maxStepMilliseconds)
        msec = tuning_.maxStepMilliseconds;
    if (msec == 0)
        return;

    // CL_AdjustAngles: speed = 0.001 * frametime, scaled by the key speed.
    const float seconds = 0.001f * static_cast<float>(msec);
    pose_.angles[1] += seconds * tuning_.yawKeySpeed * static_cast<float>(intent.yaw);
    pose_.angles[0] += seconds * tuning_.pitchKeySpeed * static_cast<float>(intent.pitch);
    Normalize();

    if (!intent.forward && !intent.right && !intent.up)
        return;
    float forward[3], right[3], up[3];
    AngleVectors(pose_.angles, forward, right, up);
    float wish[3];
    for (int i = 0; i < 3; ++i)
        wish[i] = forward[i] * intent.forward + right[i] * intent.right + up[i] * intent.up;
    const float length = std::sqrt(wish[0] * wish[0] + wish[1] * wish[1] + wish[2] * wish[2]);
    if (length <= 0.0f)
        return;
    // Diagonals are not faster: the wish direction is normalized, as the noclip
    // move normalizes wishvel before scaling it.
    const float speed = tuning_.moveSpeed * (intent.fast ? tuning_.fastScale : 1.0f);
    const float distance = speed * seconds / length;
    for (int i = 0; i < 3; ++i)
        pose_.origin[i] += wish[i] * distance;
    Normalize();
}

CameraView FreeCamera::View(ViewExtent extent, int timeMilliseconds, float aspect) const
{
    if (extent.Empty())
        throw std::invalid_argument("Free camera view needs a non-empty extent");
    if (aspect <= 0.0f)
        aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    CameraView view;
    view.width = extent.width;
    view.height = extent.height;
    ProjectionForFov(tuning_.fovX, aspect, view.tanHalfFovX, view.tanHalfFovY);
    for (int i = 0; i < 3; ++i)
        view.vieworg[i] = pose_.origin[i];
    AnglesToAxis(pose_.angles, view.viewaxis);
    view.zNear = tuning_.zNear;
    view.time = timeMilliseconds;
    return view;
}

} // namespace kisak::view

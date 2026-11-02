#pragma once

// Host-owned free camera for the native Linux client's map-view milestone.
//
// This is not player movement. There is no playerState, no pmove, no collision and
// no cgame view code here: the camera flies through a loaded map so the renderer
// can be exercised before player spawn exists. It does follow the engine's own
// conventions so a later switch to the real client path changes inputs, not
// meaning:
//
//   * angles are {pitch, yaw, roll} in degrees and become an axis through the
//     production AnglesToAxis/AngleVectors (universal/com_math_anglevectors.cpp);
//   * mouse counts turn the view like CL_MouseMove with no acceleration
//     (yaw -= m_yaw * sensitivity * dx, pitch += m_pitch * sensitivity * dy);
//   * held turn keys turn like CL_AdjustAngles (cl_yawspeed/cl_pitchspeed degrees
//     per second);
//   * pitch stops at player_view_pitch_up/down;
//   * the projection uses CG_UpdateFov's formula.
//
// The tuning defaults are copies of the production dvar defaults, not live dvars.
// Nothing here touches the renderer, D3D9, SDL or a file.

#include <cstdint>

namespace kisak::view {

struct CameraTuning {
    float sensitivity = 5.0f;        // "sensitivity"
    float yawPerCount = 0.022f;      // "m_yaw"
    float pitchPerCount = 0.022f;    // "m_pitch"
    float yawKeySpeed = 140.0f;      // "cl_yawspeed", degrees per second
    float pitchKeySpeed = 140.0f;    // "cl_pitchspeed", degrees per second
    float pitchUpLimit = 85.0f;      // "player_view_pitch_up"
    float pitchDownLimit = 85.0f;    // "player_view_pitch_down"
    float moveSpeed = 190.0f;        // "g_speed", world units per second
    float fastScale = 4.0f;          // host-only fast-move multiplier
    float fovX = 65.0f;              // "cg_fov", degrees at 4:3
    // 0 asks the renderer for its r_znear default, as the MP cgame view does.
    float zNear = 0.0f;
    // A host hitch must not become a teleport: steps longer than this are cut,
    // like the client's frame_msec cap. Must be 1..1000.
    int maxStepMilliseconds = 200;
    // Host safety bound on every origin component, in world units.
    float originLimit = 131072.0f;
};

// Throws std::invalid_argument when a field is not finite or out of range.
void ValidateTuning(const CameraTuning &tuning);

struct CameraPose {
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float angles[3] = {0.0f, 0.0f, 0.0f}; // pitch, yaw, roll in degrees
};

// Movement held this step. Every axis is -1, 0 or +1; opposing keys cancel before
// this point. Roll is never driven by input.
struct MoveIntent {
    int forward = 0;   // +1 forward, -1 back
    int right = 0;     // +1 right, -1 left
    int up = 0;        // +1 up along the view's up axis, -1 down
    int yaw = 0;       // +1 turn left (yaw increases), -1 turn right
    int pitch = 0;     // +1 look down (pitch increases), -1 look up
    bool fast = false;
};

struct ViewExtent {
    uint32_t width = 0;
    uint32_t height = 0;
    bool Empty() const { return width == 0 || height == 0; }
    bool operator==(const ViewExtent &) const = default;
};

// The refdef_s fields a free camera owns. The renderer side copies these into the
// real refdef with FillRefdef; every other refdef field stays the renderer's.
struct CameraView {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    float tanHalfFovX = 0.0f, tanHalfFovY = 0.0f;
    float vieworg[3] = {0.0f, 0.0f, 0.0f};
    float viewaxis[3][3] = {};
    float zNear = 0.0f;
    int time = 0;
};

// CG_UpdateFov: tan(fov/2) at 4:3, then vertical = 0.75 of that and horizontal =
// vertical * aspect. aspect is vidConfig.aspectRatioWindow on the real client.
void ProjectionForFov(float fovX, float aspect, float &tanHalfFovX, float &tanHalfFovY);

class FreeCamera {
public:
    explicit FreeCamera(const CameraTuning &tuning = {}, const CameraPose &pose = {});

    // Mouse counts since the last call; applied immediately, in arrival order, as
    // CL_MouseMove applies them. Non-finite input is rejected.
    void Look(float countsX, float countsY);
    // Advance by msec (cut to maxStepMilliseconds; negative is rejected): key
    // turning first, then movement along the turned view, as CL_CreateCmd orders
    // CL_AdjustAngles before the move that consumes the angles.
    void Step(const MoveIntent &intent, int msec);

    const CameraPose &Pose() const { return pose_; }
    // Replaces the pose after the same pitch/yaw/origin normalization a step uses.
    void SetPose(const CameraPose &pose);
    const CameraTuning &Tuning() const { return tuning_; }

    // aspect <= 0 means width/height. Throws std::invalid_argument for an empty
    // extent: the caller skips the draw instead of asking for a view.
    CameraView View(ViewExtent extent, int timeMilliseconds, float aspect = 0.0f) const;

private:
    void Normalize();
    CameraTuning tuning_;
    CameraPose pose_;
};

// Copy the camera-owned fields into a refdef. Templated so this header never needs
// the renderer's headers: the renderer instantiates it with refdef_s.
template <class Refdef>
void FillRefdef(const CameraView &view, Refdef &refdef)
{
    refdef.x = view.x;
    refdef.y = view.y;
    refdef.width = view.width;
    refdef.height = view.height;
    refdef.tanHalfFovX = view.tanHalfFovX;
    refdef.tanHalfFovY = view.tanHalfFovY;
    for (int i = 0; i < 3; ++i) {
        refdef.vieworg[i] = view.vieworg[i];
        for (int j = 0; j < 3; ++j)
            refdef.viewaxis[i][j] = view.viewaxis[i][j];
    }
    refdef.zNear = view.zNear;
    refdef.time = view.time;
}

} // namespace kisak::view

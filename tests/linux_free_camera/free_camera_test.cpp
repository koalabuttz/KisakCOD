// tests/linux_free_camera/free_camera_test.cpp
//
// Asset-free tests for the host free camera and its key bindings. Nothing here
// opens a file, creates a window or device, or starts the game.
//
// Provenance of expected values
// -----------------------------
// The view axis comes from the production AnglesToAxis, compiled unmodified from
// src/universal/com_math_anglevectors.cpp beside this test, so the axis checks pin
// the engine's real convention (axis[1] is left, i.e. AngleVectors' right negated).
// The tuning defaults are compared against literals copied from the dvar
// registrations named in free_camera.h. The 65-degree projection is compared with
// 0.6370702385902405, the constant the original MP cgame precomputed for
// tan(65/2 degrees) (src/cgame_mp/cg_view_mp.cpp, CG_UpdateFov comment), which is
// an independent source for that number.
//
// What is not claimed: no frame is rendered, nothing is compared with a captured
// game frame, and the movement is not PM_NoclipMove (no acceleration, friction or
// collision).

#include "check.h"

#include <linux/client/view/free_camera.h>
#include <linux/client/view/free_camera_controls.h>
#include <ui/keycodes.h>

#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

using namespace kisak::view;
using namespace free_camera_check;

namespace {

constexpr double kAngle = 1e-4;
constexpr double kUnits = 1e-3;

CameraPose PoseAt(float x, float y, float z, float pitch, float yaw, float roll = 0.0f)
{
    CameraPose pose;
    pose.origin[0] = x;
    pose.origin[1] = y;
    pose.origin[2] = z;
    pose.angles[0] = pitch;
    pose.angles[1] = yaw;
    pose.angles[2] = roll;
    return pose;
}

void TestTuningDefaults()
{
    const CameraTuning t;
    Near(t.sensitivity, 5.0, 0, "default sensitivity matches dvar 'sensitivity'");
    Near(t.yawPerCount, 0.022f, 0, "default yaw per count matches m_yaw");
    Near(t.pitchPerCount, 0.022f, 0, "default pitch per count matches m_pitch");
    Near(t.yawKeySpeed, 140.0, 0, "default yaw key speed matches cl_yawspeed");
    Near(t.pitchKeySpeed, 140.0, 0, "default pitch key speed matches cl_pitchspeed");
    Near(t.pitchUpLimit, 85.0, 0, "default pitch up limit matches player_view_pitch_up");
    Near(t.pitchDownLimit, 85.0, 0, "default pitch down limit matches player_view_pitch_down");
    Near(t.moveSpeed, 190.0, 0, "default speed matches g_speed");
    Near(t.fovX, 65.0, 0, "default fov matches cg_fov");
    Near(t.zNear, 0.0, 0, "default near plane defers to the renderer like the MP cgame");
    Check(t.maxStepMilliseconds == 200, "default step cap is 200 ms");
    bool valid = true;
    try { ValidateTuning(t); } catch (...) { valid = false; }
    Check(valid, "default tuning validates");
}

void TestTuningValidation()
{
    auto with = [](auto change) { CameraTuning t; change(t); return t; };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Throws<std::invalid_argument>([&] { ValidateTuning(with([&](CameraTuning &t) { t.sensitivity = nan; })); }, "NaN sensitivity is refused");
    Throws<std::invalid_argument>([&] { ValidateTuning(with([&](CameraTuning &t) { t.moveSpeed = inf; })); }, "infinite speed is refused");
    Throws<std::invalid_argument>([&] { ValidateTuning(with([](CameraTuning &t) { t.fovX = 0.5f; })); }, "fov below cg_fov's range is refused");
    Throws<std::invalid_argument>([&] { ValidateTuning(with([](CameraTuning &t) { t.fovX = 161.0f; })); }, "fov above cg_fov's range is refused");
    Throws<std::invalid_argument>([&] { ValidateTuning(with([](CameraTuning &t) { t.pitchUpLimit = 91.0f; })); }, "pitch limit above 90 is refused");
    Throws<std::invalid_argument>([&] { ValidateTuning(with([](CameraTuning &t) { t.maxStepMilliseconds = 0; })); }, "zero step cap is refused");
    Throws<std::invalid_argument>([&] { ValidateTuning(with([](CameraTuning &t) { t.fastScale = 0.5f; })); }, "fast scale below one is refused");
    Throws<std::invalid_argument>([&] { FreeCamera camera(with([](CameraTuning &t) { t.zNear = -1.0f; })); }, "constructor validates tuning");
    Throws<std::invalid_argument>([&] { FreeCamera camera({}, PoseAt(nan, 0, 0, 0, 0)); }, "constructor refuses a NaN origin");
    Throws<std::invalid_argument>([&] { FreeCamera camera({}, PoseAt(0, 0, 0, 0, inf)); }, "constructor refuses an infinite angle");
}

void TestAxisConvention()
{
    FreeCamera camera({}, PoseAt(10.0f, -20.0f, 30.0f, 0, 0));
    CameraView view = camera.View({640, 480}, 1234);
    const float identity[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            Near(view.viewaxis[i][j], identity[i][j], 1e-6, "zero angles give forward x, left y, up z");
    Near(view.vieworg[0], 10.0, 0, "vieworg x is the camera origin");
    Near(view.vieworg[1], -20.0, 0, "vieworg y is the camera origin");
    Near(view.vieworg[2], 30.0, 0, "vieworg z is the camera origin");
    Check(view.time == 1234, "view carries the frame time");
    Check(view.width == 640 && view.height == 480 && view.x == 0 && view.y == 0, "view rectangle is the full extent");

    camera.SetPose(PoseAt(0, 0, 0, 0, 90.0f));
    view = camera.View({640, 480}, 0);
    Near(view.viewaxis[0][0], 0.0, 1e-6, "yaw 90 forward x");
    Near(view.viewaxis[0][1], 1.0, 1e-6, "yaw 90 forward points along +y");
    Near(view.viewaxis[1][0], -1.0, 1e-6, "yaw 90 left points along -x");
    Near(view.viewaxis[2][2], 1.0, 1e-6, "yaw 90 up stays +z");

    camera.SetPose(PoseAt(0, 0, 0, 45.0f, 0));
    view = camera.View({640, 480}, 0);
    Near(view.viewaxis[0][2], -std::sqrt(0.5), 1e-6, "positive pitch looks down");

    // Many poses: the production axis is orthonormal and left == up x forward.
    uint32_t seed = 12345;
    auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / float(1u << 24); };
    double worst = 0.0;
    for (int n = 0; n < 2000; ++n) {
        camera.SetPose(PoseAt(0, 0, 0, next() * 170.0f - 85.0f, next() * 360.0f - 180.0f, next() * 60.0f - 30.0f));
        view = camera.View({640, 480}, 0);
        const auto &a = view.viewaxis;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                double dot = a[i][0] * a[j][0] + a[i][1] * a[j][1] + a[i][2] * a[j][2];
                worst = std::fmax(worst, std::fabs(dot - (i == j ? 1.0 : 0.0)));
            }
        const double cross[3] = {a[2][1] * a[0][2] - a[2][2] * a[0][1], a[2][2] * a[0][0] - a[2][0] * a[0][2],
                                 a[2][0] * a[0][1] - a[2][1] * a[0][0]};
        for (int k = 0; k < 3; ++k)
            worst = std::fmax(worst, std::fabs(cross[k] - a[1][k]));
    }
    Near(worst, 0.0, 1e-5, "2000 production axes are orthonormal with left == up x forward");
}

void TestMouseLook()
{
    const CameraTuning tuning;
    FreeCamera camera(tuning);
    camera.Look(1.0f, 0.0f);
    Near(camera.Pose().angles[1], -0.11, kAngle, "one count right turns yaw by -m_yaw*sensitivity");
    camera.SetPose({});
    camera.Look(-90.0f / (0.022f * 5.0f), 0.0f);
    Near(camera.Pose().angles[1], 90.0, kAngle, "counts left turn the view left by 90 degrees");
    camera.SetPose({});
    camera.Look(0.0f, 10.0f);
    Near(camera.Pose().angles[0], 1.1, kAngle, "counts down raise pitch (look down)");
    camera.Look(0.0f, 100000.0f);
    Near(camera.Pose().angles[0], 85.0, 0, "pitch stops at player_view_pitch_down");
    camera.Look(0.0f, -100000.0f);
    Near(camera.Pose().angles[0], -85.0, 0, "pitch stops at player_view_pitch_up");
    camera.SetPose(PoseAt(0, 0, 0, 0, 179.0f));
    camera.Look(-2.0f / 0.11f, 0.0f);
    Near(camera.Pose().angles[1], -179.0, kAngle, "yaw wraps across 180");
    Throws<std::invalid_argument>([&] { camera.Look(std::numeric_limits<float>::quiet_NaN(), 0); }, "NaN look is refused");
    Near(camera.Pose().angles[1], -179.0, kAngle, "refused look leaves the pose unchanged");
}

void TestKeyTurn()
{
    FreeCamera camera;
    MoveIntent turn;
    turn.yaw = 1;
    for (int i = 0; i < 5; ++i)
        camera.Step(turn, 100);
    Near(camera.Pose().angles[1], 70.0, kAngle, "turn-left key: cl_yawspeed for half a second");
    turn = {};
    turn.pitch = -1;
    camera.Step(turn, 200);
    Near(camera.Pose().angles[0], -28.0, kAngle, "look-up key lowers pitch at cl_pitchspeed");
    for (int i = 0; i < 10; ++i)
        camera.Step(turn, 200);
    Near(camera.Pose().angles[0], -85.0, 0, "held look-up stops at the limit");
    camera.SetPose(PoseAt(0, 0, 0, 350.0f, 0));
    Near(camera.Pose().angles[0], -10.0, kAngle, "a supplied pitch of 350 is the orientation -10");
    camera.SetPose(PoseAt(0, 0, 0, 120.0f, 0));
    Near(camera.Pose().angles[0], 85.0, 0, "a supplied pitch past the limit is clamped");
}

void TestMovement()
{
    MoveIntent forward;
    forward.forward = 1;
    FreeCamera camera;
    for (int i = 0; i < 5; ++i)
        camera.Step(forward, 200);
    Near(camera.Pose().origin[0], 190.0, kUnits, "one second forward at yaw 0 moves g_speed along +x");
    Near(camera.Pose().origin[1], 0.0, kUnits, "forward does not drift sideways");

    camera.SetPose(PoseAt(0, 0, 0, 0, 90.0f));
    for (int i = 0; i < 5; ++i)
        camera.Step(forward, 200);
    Near(camera.Pose().origin[1], 190.0, kUnits, "forward at yaw 90 moves along +y");

    MoveIntent strafe;
    strafe.right = 1;
    camera.SetPose({});
    camera.Step(strafe, 200);
    Near(camera.Pose().origin[1], -38.0, kUnits, "strafe right at yaw 0 moves along -y (AngleVectors right)");

    MoveIntent up;
    up.up = 1;
    camera.SetPose({});
    camera.Step(up, 200);
    Near(camera.Pose().origin[2], 38.0, kUnits, "up moves along +z at zero pitch");

    camera.SetPose(PoseAt(0, 0, 0, 45.0f, 0));
    camera.Step(forward, 200);
    Near(camera.Pose().origin[2], -38.0 * std::sqrt(0.5), kUnits, "forward follows the pitched view down");

    MoveIntent diagonal;
    diagonal.forward = 1;
    diagonal.right = 1;
    camera.SetPose({});
    camera.Step(diagonal, 200);
    const double travelled = std::hypot(camera.Pose().origin[0], camera.Pose().origin[1]);
    Near(travelled, 38.0, kUnits, "diagonal movement is not faster");

    MoveIntent fast = forward;
    fast.fast = true;
    camera.SetPose({});
    camera.Step(fast, 200);
    Near(camera.Pose().origin[0], 152.0, kUnits, "fast multiplies speed by fastScale");

    camera.SetPose({});
    camera.Step(forward, 5000);
    Near(camera.Pose().origin[0], 38.0, kUnits, "a 5 second hitch is cut to the 200 ms cap");

    camera.SetPose({});
    camera.Step(forward, 0);
    Near(camera.Pose().origin[0], 0.0, 0, "a zero step moves nothing");

    // Frame-rate independence for straight motion.
    FreeCamera fine, coarse;
    for (int i = 0; i < 100; ++i)
        fine.Step(forward, 10);
    for (int i = 0; i < 5; ++i)
        coarse.Step(forward, 200);
    Near(fine.Pose().origin[0], coarse.Pose().origin[0], 1e-2, "100 x 10 ms equals 5 x 200 ms");

    CameraTuning limited;
    limited.originLimit = 100.0f;
    FreeCamera bounded(limited);
    for (int i = 0; i < 10; ++i)
        bounded.Step(forward, 200);
    Near(bounded.Pose().origin[0], 100.0, 0, "origin is held inside the safety bound");

    MoveIntent bad;
    bad.forward = 2;
    Throws<std::invalid_argument>([&] { camera.Step(bad, 10); }, "an axis outside -1..1 is refused");
    Throws<std::invalid_argument>([&] { camera.Step(forward, -1); }, "a negative step is refused");
}

void TestProjection()
{
    float x = 0, y = 0;
    ProjectionForFov(65.0f, 4.0f / 3.0f, x, y);
    Near(x, 0.6370702385902405, 1e-6, "65 degrees at 4:3 matches the cgame's precomputed tan");
    Near(y, 0.6370702385902405 * 0.75, 1e-6, "vertical is 0.75 of the 4:3 horizontal");
    ProjectionForFov(65.0f, 16.0f / 9.0f, x, y);
    Near(y, 0.6370702385902405 * 0.75, 1e-6, "vertical fov is fixed across aspect ratios");
    Near(x, 0.6370702385902405 * 0.75 * 16.0 / 9.0, 1e-6, "horizontal widens with aspect (Hor+)");
    Throws<std::invalid_argument>([&] { ProjectionForFov(65.0f, 0.0f, x, y); }, "zero aspect is refused");
    Throws<std::invalid_argument>([&] { ProjectionForFov(200.0f, 1.0f, x, y); }, "fov above range is refused");

    FreeCamera camera;
    CameraView view = camera.View({1920, 1080}, 0);
    Near(view.tanHalfFovX / view.tanHalfFovY, 16.0 / 9.0, 1e-6, "aspect defaults to width/height");
    view = camera.View({1920, 1080}, 0, 4.0f / 3.0f);
    Near(view.tanHalfFovX / view.tanHalfFovY, 4.0 / 3.0, 1e-6, "an explicit aspectRatioWindow wins");
    Throws<std::invalid_argument>([&] { camera.View({0, 1080}, 0); }, "an empty extent has no view");
}

struct FakeRefdef {
    uint32_t x = 9, y = 9, width = 9, height = 9;
    float tanHalfFovX = 9, tanHalfFovY = 9;
    float vieworg[3] = {9, 9, 9};
    float viewaxis[3][3] = {{9, 9, 9}, {9, 9, 9}, {9, 9, 9}};
    float viewOffset[3] = {7, 7, 7};
    int time = 9;
    float zNear = 9;
    float blurRadius = 7;
    int localClientNum = 7;
};

void TestFillRefdef()
{
    FreeCamera camera({}, PoseAt(1, 2, 3, 0, 90.0f));
    const CameraView view = camera.View({800, 600}, 4321);
    FakeRefdef refdef;
    FillRefdef(view, refdef);
    Check(refdef.x == 0 && refdef.y == 0 && refdef.width == 800 && refdef.height == 600, "refdef rectangle copied");
    Near(refdef.tanHalfFovX, view.tanHalfFovX, 0, "refdef tanHalfFovX copied");
    Near(refdef.tanHalfFovY, view.tanHalfFovY, 0, "refdef tanHalfFovY copied");
    Near(refdef.vieworg[2], 3.0, 0, "refdef vieworg copied");
    Near(refdef.viewaxis[0][1], view.viewaxis[0][1], 0, "refdef axis copied");
    Check(refdef.time == 4321, "refdef time copied");
    Near(refdef.zNear, 0.0, 0, "refdef zNear copied");
    Check(refdef.viewOffset[0] == 7 && refdef.blurRadius == 7 && refdef.localClientNum == 7,
          "fields the camera does not own are untouched");
}

void TestControls()
{
    FreeCameraControls controls;
    Check(!controls.Key('x', true), "unbound key is not consumed");
    Check(!controls.Key('W', true), "uppercase is not an engine key code for a letter");
    Check(controls.Key('w', true), "w is bound");
    Check(controls.Intent().forward == 1, "w holds forward");
    controls.Key('s', true);
    Check(controls.Intent().forward == 0, "w and s together cancel");
    controls.Key('w', false);
    Check(controls.Intent().forward == -1, "releasing w leaves back");
    controls.Key('s', false);
    controls.Key('d', true);
    Check(controls.Intent().right == 1, "d strafes right");
    controls.Key('a', true);
    Check(controls.Intent().right == 0, "a and d cancel");
    controls.Key(K_SPACE, true);
    Check(controls.Intent().up == 1, "space moves up");
    controls.Key('c', true);
    Check(controls.Intent().up == 0, "space and c cancel");
    controls.Key(K_SHIFT, true);
    Check(controls.Intent().fast, "shift is fast");
    controls.Key(K_LEFTARROW, true);
    Check(controls.Intent().yaw == 1, "left arrow turns left");
    controls.Key(K_DOWNARROW, true);
    Check(controls.Intent().pitch == 1, "down arrow looks down");
    controls.Key(K_UPARROW, true);
    Check(controls.Intent().pitch == 0, "up and down arrows cancel");
    controls.Mouse(3, -4);
    controls.Mouse(2, 1);
    FreeCameraControls::Look look = controls.TakeLook();
    Check(look.dx == 5 && look.dy == -3, "mouse counts accumulate");
    look = controls.TakeLook();
    Check(look.dx == 0 && look.dy == 0, "taking the look clears it");
    for (int i = 0; i < 4; ++i)
        controls.Mouse(INT_MAX, INT_MIN);
    look = controls.TakeLook();
    Check(look.dx == INT_MAX && look.dy == INT_MIN, "an overflowing burst saturates instead of wrapping");
    controls.Mouse(1, 1);
    Check(controls.AnyHeld(), "keys are held before release");
    controls.ReleaseAll();
    const MoveIntent idle = controls.Intent();
    Check(!controls.AnyHeld() && idle.forward == 0 && idle.right == 0 && idle.up == 0 && idle.yaw == 0 &&
              idle.pitch == 0 && !idle.fast,
          "release drops every held key");
    look = controls.TakeLook();
    Check(look.dx == 0 && look.dy == 0, "release drops pending look");
}

} // namespace

int main()
{
    TestTuningDefaults();
    TestTuningValidation();
    TestAxisConvention();
    TestMouseLook();
    TestKeyTurn();
    TestMovement();
    TestProjection();
    TestFillRefdef();
    TestControls();
    return Finish("free_camera_test");
}

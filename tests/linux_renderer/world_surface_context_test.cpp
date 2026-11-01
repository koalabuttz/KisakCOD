// tests/linux_renderer/world_surface_context_test.cpp
//
// Asset-free tests for the numeric shader-constant context of one unlit world surface
// draw. Nothing here opens a file, creates a device, draws, or starts the game: it is
// arithmetic over explicitly supplied inputs only.
//
// What is and is not being claimed
// --------------------------------
// These tests check that the constants a standalone packet would upload match the
// values the engine's own formula chain produces for a given camera and color state.
// They do NOT claim that any captured frame was reproduced, and they do not verify
// shader execution: without the authentic shader bytes and captured argument routing, a
// matching constant payload says nothing about the rendered image.
//
// Camera basis provenance
// -----------------------
// The camera bases used here are produced by a transcription of the production
// AnglesToAxis (src/universal/com_math_anglevectors.cpp), not written by hand. That
// matters: AnglesToAxis stores AngleVectors' right vector negated, so the engine's left
// axis satisfies left == up x forward. An earlier revision of this test asserted
// left == forward x up and therefore validated a basis the engine never produces; the
// zero-angle case (forward (1,0,0), left (0,1,0), up (0,0,1)) is included specifically
// to pin the real convention.
//
// Expected values
// ---------------
// Every expected matrix below is frozen. The values were produced independently of the
// header under test, by modelling the engine's operation order (float32 inputs, double
// accumulators, one rounding per stored element) in a separate script, and are pasted as
// literals. A test that only compared the header against itself would accept a
// wrong-but-self-consistent transposition, which is why the frozen tables, the
// hand-derived zero-angle view matrix, and the two-formulation projection check are all
// present.
//
// Fixture A carries the properties the constant chain is most likely to get wrong: a
// nonzero asymmetric camera origin, a rotated orthonormal production basis, unequal
// half-FOV tangents, and a near plane other than 1.

#include "world_surface_context.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using world_surface_context::Context;
using world_surface_context::Error;
using world_surface_context::Inputs;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* label)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::printf("FAIL  %s\n", label);
    }
}

// Tolerances are tight enough that a transposed, skewed, or double-subtracted matrix
// cannot pass: those errors move an entry by O(1) or more, while the only expected
// deviation here is float rounding, fused multiply-add contraction, and x87 versus SSE
// evaluation of the trigonometric basis.
float Tolerance(float expected)
{
    const float magnitude = std::fabs(expected);
    return 1.0e-6f + 1.0e-6f * magnitude;
}

// Row 3 of the derived view-projection is a cancellation residue of order 1e-5, produced
// as the difference of terms near 1e2. Its low bits therefore depend on the exact float
// bits of the trigonometric basis, which legitimately vary with fused multiply-add
// contraction and with x87 versus SSE evaluation. Cells that small are compared against
// this floor instead of the relative tolerance.
//
// The floor still discriminates: every structural error moves those cells by about 1e2
// (row 3 not rewritten gives 188.37, an origin applied twice gives roughly a full
// origin), which is six orders of magnitude above the floor. The exactly representable
// zero-angle fixture below freezes the same residue with no floor at all.
constexpr float kResidueFloor = 1.0e-4f;

void CheckClose(float actual, float expected, const char* label)
{
    const float difference = std::fabs(actual - expected);
    const bool ok = difference <= Tolerance(expected);
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("FAIL  %s: actual %.9g expected %.9g (|d| = %.3g)\n",
            label, actual, expected, difference);
    }
}

void CheckMatrix(const float actual[4][4], const float expected[4][4], const char* label,
    float floor = 0.0f)
{
    ++g_checks;
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float tolerance = Tolerance(expected[row][column]);
            if (tolerance < floor)
                tolerance = floor;
            const float difference = std::fabs(actual[row][column] - expected[row][column]);
            if (!(difference <= tolerance))
            {
                ++g_failures;
                std::printf("FAIL  %s [%d][%d]: actual %.9g expected %.9g (|d| = %.3g)\n",
                    label, row, column, actual[row][column], expected[row][column], difference);
            }
        }
    }
}

// Bit comparison, used only where the engine copies a value without arithmetic.
void CheckSameBits(const float* actual, const float* expected, int count, const char* label)
{
    ++g_checks;
    if (std::memcmp(actual, expected, sizeof(float) * (size_t)count) != 0)
    {
        ++g_failures;
        std::printf("FAIL  %s: expected an exact copy\n", label);
        for (int index = 0; index < count; ++index)
            std::printf("        [%d] actual %.9g expected %.9g\n", index, actual[index], expected[index]);
    }
}

// ---------------------------------------------------------------------------
// Camera basis: faithful transcription of the production AnglesToAxis.
//
// The engine writes it as "AngleVectors() and then a right subtract, but it's been
// optimized to not calculate and then negate right in this function", which is why the
// left axis appears as a negated right vector.
// ---------------------------------------------------------------------------
constexpr double kPi = 3.14159265358979323846;

struct Axis
{
    float forward[3];
    float left[3];
    float up[3];
};

Axis ProductionAxis(float yawDegrees, float pitchDegrees, float rollDegrees)
{
    const double yaw = (double)yawDegrees * (kPi * 2.0 / 360.0);
    const float cy = (float)std::cos(yaw);
    const float sy = (float)std::sin(yaw);

    const double pitch = (double)pitchDegrees * (kPi * 2.0 / 360.0);
    const float cp = (float)std::cos(pitch);
    const float sp = (float)std::sin(pitch);

    Axis axis;

    // forward
    axis.forward[0] = cp * cy;
    axis.forward[1] = cp * sy;
    axis.forward[2] = -sp;

    const double roll = (double)rollDegrees * (kPi * 2.0 / 360.0);
    const float cr = (float)std::cos(roll);
    const float sr = (float)std::sin(roll);

    // right (negated) -- this is the engine's left axis
    axis.left[0] = sr * sp * cy + -sy * cr;
    axis.left[1] = sr * sp * sy + cr * cy;
    axis.left[2] = sr * cp;

    // up
    axis.up[0] = cr * sp * cy + -sr * -sy;
    axis.up[1] = cr * sp * sy + -sr * cy;
    axis.up[2] = cr * cp;

    return axis;
}

// ---------------------------------------------------------------------------
// Fixture A: production basis for a nonzero yaw/pitch/roll, nonzero asymmetric origin,
// unequal half-FOV tangents, near plane 4.
// ---------------------------------------------------------------------------
const float kFixtureYaw = 37.0f;
const float kFixturePitch = -21.0f;
const float kFixtureRoll = 13.0f;

const float kFixtureOrigin[3] = { 128.5f, -64.25f, 302.75f };
const float kFixtureTanHalfFovX = 0.75f;
const float kFixtureTanHalfFovY = 0.5f;
const float kFixtureZNear = 4.0f;

// Frozen transcription output for AnglesToAxis(37, -21, 13). Asserted against the
// transcribed helper so the frozen tables are demonstrably tied to the production basis
// rather than to a hand-invented one.
const float kExpectedFixtureForward[3] = { 0.745590508f, 0.56184268f, 0.35836795f };
const float kExpectedFixtureLeft[3] = { -0.650772691f, 0.729651153f, 0.210009888f };
const float kExpectedFixtureUp[3] = { -0.143491045f, -0.389797449f, 0.90965277f };

// Material color sentinel: asymmetric, not neutral, alpha not opaque. The builder must
// copy this rather than substitute a default.
const float kFixtureMaterialColor[4] = { 0.125f, 0.625f, 0.875f, 0.375f };

// Frame fog sentinel. Bytes are deliberately asymmetric so a channel reorder is
// visible: 0x11, 0x22, 0x33, 0x44 cannot be a permutation of an equal-valued color.
const unsigned char kFixtureFogBgra[4] = { 0x11, 0x22, 0x33, 0x44 };
const float kFixtureFogDensity = 0.0125f;
const float kFixtureFogStart = 250.0f;

// A fog color that must be preserved unchanged when r_fog is off. Distinct from any
// value the enabled path could produce.
const float kPreviousFogColor[4] = { 0.875f, 0.5f, 0.0625f, 0.9375f };

// Every member is assigned: this is the "all inputs supplied explicitly" case, so no
// field leans on a default value.
Inputs MakeFixtureA()
{
    const Axis axis = ProductionAxis(kFixtureYaw, kFixturePitch, kFixtureRoll);

    Inputs input;
    input.camera.origin[0] = kFixtureOrigin[0];
    input.camera.origin[1] = kFixtureOrigin[1];
    input.camera.origin[2] = kFixtureOrigin[2];
    for (int index = 0; index < 3; ++index)
    {
        input.camera.forward[index] = axis.forward[index];
        input.camera.left[index] = axis.left[index];
        input.camera.up[index] = axis.up[index];
    }
    input.camera.tanHalfFovX = kFixtureTanHalfFovX;
    input.camera.tanHalfFovY = kFixtureTanHalfFovY;
    input.camera.zNear = kFixtureZNear;

    input.materialColor.rgba[0] = kFixtureMaterialColor[0];
    input.materialColor.rgba[1] = kFixtureMaterialColor[1];
    input.materialColor.rgba[2] = kFixtureMaterialColor[2];
    input.materialColor.rgba[3] = kFixtureMaterialColor[3];

    input.fog.enabled = true;
    input.fog.density = kFixtureFogDensity;
    input.fog.fogStart = kFixtureFogStart;
    for (int index = 0; index < 4; ++index)
        input.fog.colorBgra[index] = kFixtureFogBgra[index];
    for (int index = 0; index < 4; ++index)
        input.fog.previousColor[index] = kPreviousFogColor[index];
    return input;
}

// ---------------------------------------------------------------------------
// Frozen expectations for fixture A.
// ---------------------------------------------------------------------------

// com_math.cpp MatrixForViewer
const float kExpectedView[4][4] = {
    { 0.650772691f, -0.143491045f, 0.745590508f, 0.0f },
    { -0.729651153f, -0.389797449f, 0.56184268f, 0.0f },
    { -0.210009888f, 0.90965277f, 0.35836795f, 0.0f },
    { -66.9238815f, -282.003265f, -168.205887f, 1.0f },
};

// com_math.cpp InfinitePerspectiveMatrix: z scale is 0.99951172, not 1, and the
// translation term is -(zNear * 0.99951172) = -3.99804688 for zNear 4.
const float kExpectedProjection[4][4] = {
    { 1.33268225f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 1.99902344f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 0.999511719f, 1.0f },
    { 0.0f, 0.0f, -3.99804688f, 0.0f },
};

// MatrixMultiply44(view, projection)
const float kExpectedViewProjection[4][4] = {
    { 0.867273211f, -0.286841959f, 0.745226443f, 0.745590508f },
    { -0.972393155f, -0.779214263f, 0.56156832f, 0.56184268f },
    { -0.279876441f, 1.81841719f, 0.35819295f, 0.35836795f },
    { -89.1882706f, -563.73114f, -172.121796f, -168.205887f },
};

// R_DeriveViewProjectionMatrix: rows 0..2 unchanged, row 3 is the eye offset
// transformed through the above. Row 3 collapses to a rounding residue because the
// world matrix's -origin cancels it; the residue is the honest expected value.
const float kExpectedDerivedViewProjection[4][4] = {
    { 0.867273211f, -0.286841959f, 0.745226443f, 0.745590508f },
    { -0.972393155f, -0.779214263f, 0.56156832f, 0.56184268f },
    { -0.279876441f, 1.81841719f, 0.35819295f, 0.35836795f },
    { 4.88013029e-06f, -1.07139349e-05f, -3.99804664f, -1.97440386e-06f },
};

// R_CmdBufSet3D: identity with row 3 = -origin.
const float kExpectedWorld[4][4] = {
    { 1.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 1.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 1.0f, 0.0f },
    { -128.5f, 64.25f, -302.75f, 1.0f },
};

// Register payload for VS c4..c7: transpose of the world matrix. The negated origin
// belongs in the last column here, not the last row.
const float kExpectedTransposeWorld[4][4] = {
    { 1.0f, 0.0f, 0.0f, -128.5f },
    { 0.0f, 1.0f, 0.0f, 64.25f },
    { 0.0f, 0.0f, 1.0f, -302.75f },
    { 0.0f, 0.0f, 0.0f, 1.0f },
};

// Register payload for VS c0..c3: transpose of the derived view-projection.
const float kExpectedTransposeViewProjection[4][4] = {
    { 0.867273211f, -0.972393155f, -0.279876441f, 4.88013029e-06f },
    { -0.286841959f, -0.779214263f, 1.81841719f, -1.07139349e-05f },
    { 0.745226443f, 0.56156832f, 0.35819295f, -3.99804664f },
    { 0.745590508f, 0.56184268f, 0.35836795f, -1.97440386e-06f },
};

// Clip-space NDC of the probe point, from a path that uses the basis and the projection
// scalars only. Frozen so the matrix path has something external to meet. The point is
// exactly representable and sits in front of this camera at eye-space distance about
// (24.9, 39.8, 120.0), so its NDC is inside the frustum rather than off to one side.
const float kProbePoint[3] = { 228.5f, -30.5f, 376.75f };
const float kExpectedProbeNdc[3] = { 0.2765583622f, 0.6629447299f, 0.9662058937f };

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

// The fixture basis must be the production one. If the transcription or the convention
// drifts, every frozen table below is meaningless, so this runs first.
void TestBasisProvenance()
{
    const Axis axis = ProductionAxis(kFixtureYaw, kFixturePitch, kFixtureRoll);
    for (int index = 0; index < 3; ++index)
    {
        CheckClose(axis.forward[index], kExpectedFixtureForward[index], "fixture forward");
        CheckClose(axis.left[index], kExpectedFixtureLeft[index], "fixture left");
        CheckClose(axis.up[index], kExpectedFixtureUp[index], "fixture up");
    }

    // left == up x forward, the production relation.
    const float expectedLeft[3] = {
        axis.up[1] * axis.forward[2] - axis.up[2] * axis.forward[1],
        axis.up[2] * axis.forward[0] - axis.up[0] * axis.forward[2],
        axis.up[0] * axis.forward[1] - axis.up[1] * axis.forward[0],
    };
    for (int index = 0; index < 3; ++index)
        CheckClose(axis.left[index], expectedLeft[index], "fixture left == up x forward");

    // ...and NOT forward x up (that is AngleVectors' right vector). Stating the
    // difference explicitly keeps a regression from silently reverting.
    const float rightAxis[3] = {
        axis.forward[1] * axis.up[2] - axis.forward[2] * axis.up[1],
        axis.forward[2] * axis.up[0] - axis.forward[0] * axis.up[2],
        axis.forward[0] * axis.up[1] - axis.forward[1] * axis.up[0],
    };
    float separation = 0.0f;
    for (int index = 0; index < 3; ++index)
        separation += std::fabs(axis.left[index] - rightAxis[index]);
    Check(separation > 1.0f, "production left axis is not the right axis");

    // Unit length and mutual orthogonality.
    for (int first = 0; first < 3; ++first)
    {
        const float* vector = (first == 0) ? axis.forward : (first == 1 ? axis.left : axis.up);
        const float length = vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2];
        CheckClose(length, 1.0f, "fixture axis is unit length");
    }
    const float forwardLeft = axis.forward[0] * axis.left[0] + axis.forward[1] * axis.left[1]
        + axis.forward[2] * axis.left[2];
    const float forwardUp = axis.forward[0] * axis.up[0] + axis.forward[1] * axis.up[1]
        + axis.forward[2] * axis.up[2];
    const float leftUp = axis.left[0] * axis.up[0] + axis.left[1] * axis.up[1]
        + axis.left[2] * axis.up[2];
    CheckClose(forwardLeft, 0.0f, "fixture forward . left is 0");
    CheckClose(forwardUp, 0.0f, "fixture forward . up is 0");
    CheckClose(leftUp, 0.0f, "fixture left . up is 0");
}

void TestFrozenMatrices()
{
    const Inputs input = MakeFixtureA();
    Context context;
    Error error = Error::None;
    const bool built = world_surface_context::Build(input, &context, &error);
    if (!built)
    {
        std::printf("FAIL  fixture A unexpectedly rejected: %s\n",
            world_surface_context::ErrorName(error));
        ++g_failures;
        ++g_checks;
        return;
    }

    CheckMatrix(context.world, kExpectedWorld, "world");
    CheckMatrix(context.view, kExpectedView, "view");
    CheckMatrix(context.projection, kExpectedProjection, "projection");
    CheckMatrix(context.viewProjection, kExpectedViewProjection, "viewProjection");
    CheckMatrix(context.derivedViewProjection, kExpectedDerivedViewProjection,
        "derivedViewProjection", kResidueFloor);
    CheckMatrix(context.vsTransposeWorld.rows, kExpectedTransposeWorld, "vsTransposeWorld");
    CheckMatrix(context.vsTransposeViewProjection.rows, kExpectedTransposeViewProjection,
        "vsTransposeViewProjection", kResidueFloor);

    // The infinite projection must not be confused with an ordinary one: z scale is
    // MAX_11BIT_FLT and the near term follows it.
    CheckClose(context.projection[2][2], world_surface_context::kMax11BitFloat,
        "projection z scale is MAX_11BIT_FLT");
    CheckClose(context.projection[3][2], -kFixtureZNear * world_surface_context::kMax11BitFloat,
        "projection near term is -(zNear * MAX_11BIT_FLT)");
    CheckClose(context.projection[2][3], 1.0f, "projection w row is 1");
    Check(context.projection[3][3] == 0.0f, "projection [3][3] is zero");

    // Unequal half-FOV tangents must stay on their own axes.
    CheckClose(context.projection[0][0],
        world_surface_context::kMax11BitFloat / kFixtureTanHalfFovX, "projection x scale");
    CheckClose(context.projection[1][1],
        world_surface_context::kMax11BitFloat / kFixtureTanHalfFovY, "projection y scale");
    Check(context.projection[0][0] != context.projection[1][1],
        "unequal FOV tangents produce unequal x/y scale");
}

void TestRegisterRouting()
{
    const Inputs input = MakeFixtureA();
    Context context;
    Error error = Error::None;
    Check(world_surface_context::Build(input, &context, &error), "routing fixture builds");

    // Codes as declared in src/gfx_d3d/r_state.h, and the destination span for each.
    Check(world_surface_context::kCodeMaterialColor == 40, "material color code is 40");
    Check(world_surface_context::kCodeFog == 41, "fog code is 41");
    Check(world_surface_context::kCodeFogColor == 42, "fog color code is 42");
    Check(world_surface_context::kCodeTransposeWorldMatrix == 60,
        "transpose world code is 60");
    Check(world_surface_context::kCodeTransposeViewProjectionMatrix == 76,
        "transpose view projection code is 76");

    Check(context.vsTransposeWorld.code == 60, "world payload carries code 60");
    Check(context.vsTransposeWorld.firstRegister == 4, "world payload starts at VS c4");
    Check(context.vsTransposeWorld.rowCount == 4, "world payload is four rows");

    Check(context.vsTransposeViewProjection.code == 76, "view projection payload carries code 76");
    Check(context.vsTransposeViewProjection.firstRegister == 0,
        "view projection payload starts at VS c0");
    Check(context.vsTransposeViewProjection.rowCount == 4,
        "view projection payload is four rows");

    // The four-row spans must not overlap, or one upload would overwrite the other.
    Check(context.vsTransposeViewProjection.firstRegister + context.vsTransposeViewProjection.rowCount
            <= context.vsTransposeWorld.firstRegister,
        "matrix register spans do not overlap");

    // Payload is the transpose, and exactly one transpose: row/column pairs that differ
    // in value pin the orientation.
    CheckClose(context.vsTransposeWorld.rows[0][3], -kFixtureOrigin[0],
        "world payload translation sits in the last column");
    Check(context.vsTransposeWorld.rows[3][0] == 0.0f,
        "world payload has no translation in the last row");
    Check(context.world[3][0] == -kFixtureOrigin[0],
        "world matrix itself keeps translation in the last row");

    CheckClose(context.vsTransposeViewProjection.rows[0][1], context.derivedViewProjection[1][0],
        "view projection payload is the transpose");
    Check(context.vsTransposeViewProjection.rows[0][1]
            != context.derivedViewProjection[0][1],
        "view projection payload is not the untransposed matrix");
    CheckClose(context.vsTransposeViewProjection.rows[0][1], -0.972393155f,
        "frozen discriminating element of the transposed payload");
}

// Two independent formulations of the same projection. Path A uses only the supplied
// basis and four projection scalars; path B uses only the built matrices, applied in the
// engine's two stages (world, then derived view-projection). Neither reuses the other's
// expressions, so an origin applied twice or a matrix stored transposed makes them
// disagree.
void TestIndependentProjectedPoint()
{
    const Inputs input = MakeFixtureA();
    Context context;
    Error error = Error::None;
    Check(world_surface_context::Build(input, &context, &error), "projection fixture builds");

    const Axis axis = ProductionAxis(kFixtureYaw, kFixturePitch, kFixtureRoll);
    const float* p = kProbePoint;
    const float dx = p[0] - kFixtureOrigin[0];
    const float dy = p[1] - kFixtureOrigin[1];
    const float dz = p[2] - kFixtureOrigin[2];

    // view x = -(axis[1] . d), view y = axis[2] . d, view z = axis[0] . d
    const float eyeX = -(axis.left[0] * dx + axis.left[1] * dy + axis.left[2] * dz);
    const float eyeY = axis.up[0] * dx + axis.up[1] * dy + axis.up[2] * dz;
    const float eyeZ = axis.forward[0] * dx + axis.forward[1] * dy
        + axis.forward[2] * dz;

    const float clipAx = context.projection[0][0] * eyeX;
    const float clipAy = context.projection[1][1] * eyeY;
    const float clipAz = context.projection[2][2] * eyeZ + context.projection[3][2];
    const float clipAw = eyeZ;
    Check(clipAw > 0.0f, "probe point is in front of the near plane");

    // Path A NDC, written out so a divide-by-zero cannot slip through silently.
    const float ndcAx = clipAx / clipAw;
    const float ndcAy = clipAy / clipAw;
    const float ndcAz = clipAz / clipAw;

    // Path B: transform through the world matrix, then through the derived
    // view-projection, both expanded inline rather than through a helper.
    float afterWorld[4];
    for (int column = 0; column < 4; ++column)
    {
        afterWorld[column] = p[0] * context.world[0][column]
            + p[1] * context.world[1][column]
            + p[2] * context.world[2][column]
            + 1.0f * context.world[3][column];
    }

    float clip[4];
    for (int column = 0; column < 4; ++column)
    {
        clip[column] = afterWorld[0] * context.derivedViewProjection[0][column]
            + afterWorld[1] * context.derivedViewProjection[1][column]
            + afterWorld[2] * context.derivedViewProjection[2][column]
            + afterWorld[3] * context.derivedViewProjection[3][column];
    }

    // The world matrix must place the point at (p - origin) with w still 1: this is the
    // single place the eye offset is applied.
    CheckClose(afterWorld[0], dx, "world matrix moves the point by -origin.x");
    CheckClose(afterWorld[1], dy, "world matrix moves the point by -origin.y");
    CheckClose(afterWorld[2], dz, "world matrix moves the point by -origin.z");
    CheckClose(afterWorld[3], 1.0f, "world matrix leaves w at 1");

    Check(std::fabs(clip[3]) > 0.0f, "clip w is nonzero");
    const float ndcBx = clip[0] / clip[3];
    const float ndcBy = clip[1] / clip[3];
    const float ndcBz = clip[2] / clip[3];

    // Allow a little slack for the float accumulation in path B; the two paths differ
    // only by rounding, so anything structural would exceed this by orders of magnitude.
    CheckClose(ndcBx, ndcAx, "independent paths agree on NDC x");
    CheckClose(ndcBy, ndcAy, "independent paths agree on NDC y");
    CheckClose(ndcBz, ndcAz, "independent paths agree on NDC z");

    // ...and both meet the frozen value from the separate model.
    CheckClose(ndcAx, kExpectedProbeNdc[0], "frozen NDC x");
    CheckClose(ndcAy, kExpectedProbeNdc[1], "frozen NDC y");
    CheckClose(ndcAz, kExpectedProbeNdc[2], "frozen NDC z");
}

// The eye offset is applied once in the world matrix and cancelled once in the derived
// view-projection. Composing them must recover the plain view-projection; a second
// subtraction or a second cancel shows up immediately as an O(origin) error.
void TestOriginAppliedOnce()
{
    const Inputs input = MakeFixtureA();
    Context context;
    Error error = Error::None;
    Check(world_surface_context::Build(input, &context, &error), "origin fixture builds");

    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float product = 0.0f;
            for (int inner = 0; inner < 4; ++inner)
                product += context.world[row][inner] * context.derivedViewProjection[inner][column];
            CheckClose(product, context.viewProjection[row][column],
                "world * derived view-projection recovers view-projection");
        }
    }

    // Derived view-projection keeps rows 0..2 of the plain product untouched.
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            Check(context.derivedViewProjection[row][column]
                    == context.viewProjection[row][column],
                "derived view-projection leaves the upper rows unchanged");
        }
    }

    // Only row 3 is rewritten, and it is rewritten (not left as the old translation).
    Check(context.derivedViewProjection[3][2] != context.viewProjection[3][2],
        "derived view-projection rewrites row 3");

    // With a nonzero origin the world matrix is not the identity: treating it as
    // identity would drop the eye offset and shift the result by a full origin.
    Check(context.world[3][0] == -kFixtureOrigin[0], "world row 3 x is -origin.x");
    Check(context.world[3][1] == -kFixtureOrigin[1], "world row 3 y is -origin.y");
    Check(context.world[3][2] == -kFixtureOrigin[2], "world row 3 z is -origin.z");
    Check(context.world[0][0] == 1.0f && context.world[1][1] == 1.0f
            && context.world[2][2] == 1.0f,
        "world rotation part is identity");
}

// A point displaced along the camera's left axis must land at negative view-space x,
// because MatrixForViewer puts -axis[1] in column 0. This is the observable consequence
// of the handedness convention, and it fails loudly if left and right are swapped.
void TestCameraLeftMapsToNegativeViewX()
{
    const float displacement = 7.5f;

    // Production basis at zero angles: forward (1,0,0), left (0,1,0), up (0,0,1).
    Inputs zeroInput = MakeFixtureA();
    const Axis zeroAxis = ProductionAxis(0.0f, 0.0f, 0.0f);
    for (int index = 0; index < 3; ++index)
    {
        zeroInput.camera.forward[index] = zeroAxis.forward[index];
        zeroInput.camera.left[index] = zeroAxis.left[index];
        zeroInput.camera.up[index] = zeroAxis.up[index];
    }
    Context zeroContext;
    Error error = Error::None;
    Check(world_surface_context::Build(zeroInput, &zeroContext, &error),
        "zero-angle production basis builds");

    const float zeroPoint[3] = {
        kFixtureOrigin[0] + displacement * zeroAxis.left[0],
        kFixtureOrigin[1] + displacement * zeroAxis.left[1],
        kFixtureOrigin[2] + displacement * zeroAxis.left[2],
    };
    const float zeroViewX = zeroPoint[0] * zeroContext.view[0][0]
        + zeroPoint[1] * zeroContext.view[1][0]
        + zeroPoint[2] * zeroContext.view[2][0]
        + 1.0f * zeroContext.view[3][0];
    CheckClose(zeroViewX, -displacement, "zero-angle camera left maps to negative view x");

    // Same check on the rotated fixture basis.
    const Inputs rotatedInput = MakeFixtureA();
    const Axis rotatedAxis = ProductionAxis(kFixtureYaw, kFixturePitch, kFixtureRoll);
    Context rotatedContext;
    Check(world_surface_context::Build(rotatedInput, &rotatedContext, &error),
        "rotated production basis builds");

    const float rotatedPoint[3] = {
        kFixtureOrigin[0] + displacement * rotatedAxis.left[0],
        kFixtureOrigin[1] + displacement * rotatedAxis.left[1],
        kFixtureOrigin[2] + displacement * rotatedAxis.left[2],
    };
    const float rotatedViewX = rotatedPoint[0] * rotatedContext.view[0][0]
        + rotatedPoint[1] * rotatedContext.view[1][0]
        + rotatedPoint[2] * rotatedContext.view[2][0]
        + 1.0f * rotatedContext.view[3][0];
    CheckClose(rotatedViewX, -displacement, "rotated camera left maps to negative view x");

    // A displacement along the camera's right axis (the negated left) must give the
    // opposite sign, which distinguishes the two axes rather than merely being nonzero.
    const float rightPoint[3] = {
        kFixtureOrigin[0] - displacement * rotatedAxis.left[0],
        kFixtureOrigin[1] - displacement * rotatedAxis.left[1],
        kFixtureOrigin[2] - displacement * rotatedAxis.left[2],
    };
    const float rightViewX = rightPoint[0] * rotatedContext.view[0][0]
        + rightPoint[1] * rotatedContext.view[1][0]
        + rightPoint[2] * rotatedContext.view[2][0]
        + 1.0f * rotatedContext.view[3][0];
    CheckClose(rightViewX, displacement, "camera right maps to positive view x");
}

// The zero-angle production basis is exactly representable, so the expected view matrix
// can be written from the formula alone rather than copied from a model run. This is the
// basis the earlier, wrong convention rejected.
void TestZeroAngleProductionBasis()
{
    const Axis axis = ProductionAxis(0.0f, 0.0f, 0.0f);
    CheckClose(axis.forward[0], 1.0f, "zero-angle forward is +x");
    CheckClose(axis.forward[1], 0.0f, "zero-angle forward y is 0");
    CheckClose(axis.forward[2], 0.0f, "zero-angle forward z is 0");
    CheckClose(axis.left[0], 0.0f, "zero-angle left x is 0");
    CheckClose(axis.left[1], 1.0f, "zero-angle left is +y");
    CheckClose(axis.left[2], 0.0f, "zero-angle left z is 0");
    CheckClose(axis.up[0], 0.0f, "zero-angle up x is 0");
    CheckClose(axis.up[1], 0.0f, "zero-angle up y is 0");
    CheckClose(axis.up[2], 1.0f, "zero-angle up is +z");

    Inputs input;
    input.camera.origin[0] = 10.0f;
    input.camera.origin[1] = 20.0f;
    input.camera.origin[2] = 30.0f;
    for (int index = 0; index < 3; ++index)
    {
        input.camera.forward[index] = axis.forward[index];
        input.camera.left[index] = axis.left[index];
        input.camera.up[index] = axis.up[index];
    }
    input.camera.tanHalfFovX = 1.0f;
    input.camera.tanHalfFovY = 1.0f;
    input.camera.zNear = 1.0f;

    input.materialColor.rgba[0] = 1.0f;
    input.materialColor.rgba[1] = 1.0f;
    input.materialColor.rgba[2] = 1.0f;
    input.materialColor.rgba[3] = 1.0f;

    input.fog.enabled = false;
    input.fog.density = 0.0f;
    input.fog.fogStart = 0.0f;
    for (int index = 0; index < 4; ++index)
        input.fog.colorBgra[index] = 0;
    for (int index = 0; index < 4; ++index)
        input.fog.previousColor[index] = 0.0f;

    Context context;
    Error error = Error::None;
    Check(world_surface_context::Build(input, &context, &error),
        "zero-angle production basis is accepted");

    // column 0 = -left = (0,-1,0), column 1 = up = (0,0,1), column 2 = forward = (1,0,0),
    // row 3 = (-(o . col0), -(o . col1), -(o . col2)) = (20, -30, -10)
    const float expected[4][4] = {
        { 0.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f, 0.0f },
        { 20.0f, -30.0f, -10.0f, 1.0f },
    };
    CheckMatrix(context.view, expected, "zero-angle production view");

    // This basis is exactly representable, so unlike the rotated fixture every entry here
    // is deterministic including the row-3 cancellation residue, which comes out exactly
    // zero. That is what makes the whole derived matrix freezable with no floor.
    //   viewProjection = view * projection, then row 3 replaced by eyeOffset * viewProjection
    const float expectedViewProjection[4][4] = {
        { 0.0f, 0.0f, 0.999511719f, 1.0f },
        { -0.999511719f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.999511719f, 0.0f, 0.0f },
        { 19.9902344f, -29.9853516f, -10.9946289f, -10.0f },
    };
    const float expectedDerived[4][4] = {
        { 0.0f, 0.0f, 0.999511719f, 1.0f },
        { -0.999511719f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.999511719f, 0.0f, 0.0f },
        { 0.0f, 0.0f, -0.999511719f, 0.0f },
    };
    CheckMatrix(context.viewProjection, expectedViewProjection, "zero-angle viewProjection");
    CheckMatrix(context.derivedViewProjection, expectedDerived, "zero-angle derived");

    // Row 3 is genuinely rewritten here: the residue is zero but the z term moved from
    // -10.99 to -1.0.
    Check(context.derivedViewProjection[3][2] != context.viewProjection[3][2],
        "zero-angle derived rewrites row 3");

    // Transposing swaps those entries: -10 sits in the last column of row 2.
    CheckClose(context.vsTransposeWorld.rows[0][3], -10.0f,
        "zero-angle world payload translation in last column");
    Check(context.vsTransposeWorld.rows[3][0] == 0.0f,
        "zero-angle world payload last row");

    // A view-space x of -5 means the point is 5 units to the camera's left.
    const float leftPoint[3] = { 10.0f, 25.0f, 30.0f };
    const float viewX = leftPoint[0] * context.view[0][0]
        + leftPoint[1] * context.view[1][0]
        + leftPoint[2] * context.view[2][0]
        + 1.0f * context.view[3][0];
    CheckClose(viewX, -5.0f, "zero-angle left displacement gives view x of -5");
}

// The accepted camera domain must match the production one: both production bases are
// accepted, and only a genuine convention violation is rejected.
void TestHandednessDomain()
{
    Error error = Error::None;

    {
        Inputs input = MakeFixtureA();
        const Axis axis = ProductionAxis(0.0f, 0.0f, 0.0f);
        for (int index = 0; index < 3; ++index)
        {
            input.camera.forward[index] = axis.forward[index];
            input.camera.left[index] = axis.left[index];
            input.camera.up[index] = axis.up[index];
        }
        Context context;
        Check(world_surface_context::Build(input, &context, &error),
            "zero-angle production basis accepted by validation");
        Check(error == Error::None, "zero-angle basis reports no error");
    }

    {
        Inputs input = MakeFixtureA();
        Context context;
        Check(world_surface_context::Build(input, &context, &error),
            "nonzero yaw/pitch/roll production basis accepted by validation");
    }

    {
        // Only the left axis negated: the un-negated right axis. Unit length and still
        // orthogonal, so only the handedness rule can catch it.
        Inputs input = MakeFixtureA();
        for (int index = 0; index < 3; ++index)
            input.camera.left[index] = -input.camera.left[index];
        Context context;
        Check(!world_surface_context::Build(input, &context, &error),
            "negated-left basis is rejected");
        Check(error == Error::AxisNotLeftHanded,
            "negated-left basis reports AxisNotLeftHanded");
    }
}

// A positive but tiny tangent used to pass every scalar check and still produce an
// infinite payload, and Build reported success. Both overflow routes must now be
// rejected, with the caller's packet left untouched.
void TestNonFinitePayloadRejection()
{
    Error error = Error::None;

    // (a) Reciprocal overflow: MAX_11BIT_FLT / 1e-40 exceeds the float range, so the
    // projection scale alone becomes infinity.
    {
        Inputs input = MakeFixtureA();
        input.camera.tanHalfFovX = 1.0e-40f;

        Context context;
        context.psMaterialColor[0] = -1234.5f;
        context.world[0][0] = -1234.5f;

        const bool built = world_surface_context::Build(input, &context, &error);
        Check(!built, "tiny tangent causing reciprocal overflow is rejected");
        if (built)
            std::printf("        builder accepted tanHalfFovX = 1e-40\n");
        else
            Check(error == Error::NonFinitePayload,
                "reciprocal overflow reports NonFinitePayload");
        Check(context.world[0][0] == -1234.5f,
            "reciprocal overflow leaves the output untouched");
        Check(context.psMaterialColor[0] == -1234.5f,
            "reciprocal overflow leaves the pixel constants untouched");
    }

    // (b) Translation overflow: the reciprocal is finite (about 1e34) and every scalar
    // check passes, but multiplying a permitted large origin through the
    // view-projection overflows the derived row 3. At 1e-33 the basis components happen
    // to cancel and the product stays just inside the float range, so 1e-34 is used.
    {
        Inputs input = MakeFixtureA();
        input.camera.origin[0] = 1.0e6f;
        input.camera.origin[1] = 1.0e6f;
        input.camera.origin[2] = 1.0e6f;
        input.camera.tanHalfFovX = 1.0e-34f;

        // The reciprocal itself is finite, so this case is not caught by scalar bounds.
        const float reciprocal = world_surface_context::kMax11BitFloat / input.camera.tanHalfFovX;
        Check(std::fabs(reciprocal) > 1.0e32f && reciprocal == reciprocal
                && reciprocal < 3.0e38f,
            "translation overflow case has a finite reciprocal");

        Context context;
        context.psMaterialColor[1] = -1234.5f;
        const bool built = world_surface_context::Build(input, &context, &error);
        Check(!built, "translation overflow is rejected");
        if (built)
            std::printf("        builder accepted a 1e-33 tangent with a 1e6 origin\n");
        else
            Check(error == Error::NonFinitePayload,
                "translation overflow reports NonFinitePayload");
        Check(context.psMaterialColor[1] == -1234.5f,
            "translation overflow leaves the output untouched");
    }

    // (c) A normal narrow field of view must still be accepted: the guard rejects
    // overflow, not narrowness. tan 0.0175 is a half-angle of about 2 degrees.
    {
        Inputs input = MakeFixtureA();
        input.camera.tanHalfFovX = 0.0175f;
        input.camera.tanHalfFovY = 0.0175f;

        Context context;
        const bool built = world_surface_context::Build(input, &context, &error);
        Check(built, "narrow but normal field of view is accepted");
        Check(error == Error::None, "narrow field of view reports no error");
        CheckClose(context.projection[0][0],
            world_surface_context::kMax11BitFloat / 0.0175f, "narrow FOV x scale");
        Check(std::fabs(context.projection[0][0]) < 1.0e4f,
            "narrow FOV scale stays usable");
        Check(context.vsFog[1] == 1.0f, "narrow FOV payload is intact");

        // Every payload word must be finite for the accepted narrow case.
        bool finite = true;
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                const float value = context.vsTransposeViewProjection.rows[row][column];
                finite = finite && value == value && value - value == 0.0f;
            }
        }
        Check(finite, "narrow FOV payload is finite");
    }
}

void TestFogEnabledConstants()
{
    const Inputs input = MakeFixtureA();
    Context context;
    Error error = Error::None;
    Check(world_surface_context::Build(input, &context, &error), "fog fixture builds");

    // r_fog.cpp writes (0, 1, -density, fogStart * density), in that order.
    Check(context.vsFog[0] == 0.0f, "fog parms x is 0");
    Check(context.vsFog[1] == 1.0f, "fog parms y is 1");
    Check(context.vsFog[2] == -kFixtureFogDensity, "fog parms z is -density");
    CheckClose(context.vsFog[3], kFixtureFogStart * kFixtureFogDensity,
        "fog parms w is fogStart * density");

    // Byte4UnpackBgra reads memory order B, G, R, A and yields R, G, B, A. With
    // asymmetric bytes any permutation or channel swap changes the expected value.
    const float scale = (float)world_surface_context::kByteToUnit;
    CheckClose(context.psFogColor[0], 0x33 * scale, "fog color R comes from byte 2");
    CheckClose(context.psFogColor[1], 0x22 * scale, "fog color G comes from byte 1");
    CheckClose(context.psFogColor[2], 0x11 * scale, "fog color B comes from byte 0");
    CheckClose(context.psFogColor[3], 0x44 * scale, "fog color A comes from byte 3");

    // The channel ordering claim, stated as an inequality so it cannot pass by accident
    // on a symmetric color: R must differ from B, and alpha must be neither 0 nor 1.
    Check(context.psFogColor[0] > context.psFogColor[1], "fog color channels stay distinct R>G");
    Check(context.psFogColor[1] > context.psFogColor[2], "fog color channels stay distinct G>B");
    Check(context.psFogColor[3] > 0.0f && context.psFogColor[3] < 1.0f,
        "fog color alpha is preserved, not forced opaque");

    // Frozen values from the separate model.
    CheckClose(context.psFogColor[0], 0.2000000179f, "frozen fog color R");
    CheckClose(context.psFogColor[1], 0.1333333403f, "frozen fog color G");
    CheckClose(context.psFogColor[2], 0.0666666701f, "frozen fog color B");
    CheckClose(context.psFogColor[3], 0.2666666806f, "frozen fog color A");

    CheckClose(context.vsFog[2], -0.0125000002f, "frozen fog density term");
    CheckClose(context.vsFog[3], 3.125f, "frozen fog start product");

    // When fog is enabled the previous color must not leak through.
    Check(context.psFogColor[0] != kPreviousFogColor[0],
        "enabled fog color replaces the previous value");
}

void TestFogDisabledPreservesColor()
{
    Inputs input = MakeFixtureA();
    input.fog.enabled = false;

    Context context;
    Error error = Error::None;
    Check(world_surface_context::Build(input, &context, &error), "disabled fog fixture builds");

    // R_SetFrameFog's disabled branch rewrites code 41 only.
    Check(context.vsFog[0] == 0.0f, "disabled fog parms x is 0");
    Check(context.vsFog[1] == 1.0f, "disabled fog parms y is 1");
    Check(context.vsFog[2] == 0.0f, "disabled fog parms z is 0");
    Check(context.vsFog[3] == 0.0f, "disabled fog parms w is 0");

    // ...and leaves code 42 exactly as it already stood.
    CheckSameBits(context.psFogColor, kPreviousFogColor, 4,
        "disabled fog preserves the previous fog color");

    // Explicitly not black: a packet that zeroes code 42 would pass a bit check only if
    // the previous color happened to be black.
    Check(!(context.psFogColor[0] == 0.0f && context.psFogColor[1] == 0.0f
            && context.psFogColor[2] == 0.0f && context.psFogColor[3] == 0.0f),
        "disabled fog does not zero the fog color");

    // The map fog bytes are ignored in this branch, so a wild color here changes nothing.
    Inputs otherInput = MakeFixtureA();
    otherInput.fog.enabled = false;
    otherInput.fog.colorBgra[0] = 0xFF;
    otherInput.fog.colorBgra[1] = 0x01;
    otherInput.fog.colorBgra[2] = 0x7F;
    otherInput.fog.colorBgra[3] = 0x00;
    Context otherContext;
    Check(world_surface_context::Build(otherInput, &otherContext, &error),
        "disabled fog with different bytes builds");
    CheckSameBits(otherContext.psFogColor, kPreviousFogColor, 4,
        "disabled fog ignores the map fog bytes");
}

void TestMaterialColorCopied()
{
    const Inputs input = MakeFixtureA();
    Context context;
    Error error = Error::None;
    Check(world_surface_context::Build(input, &context, &error), "material fixture builds");

    CheckSameBits(context.psMaterialColor, kFixtureMaterialColor, 4,
        "material color is copied verbatim");

    // Not a substituted default: neither an all-zero nor an all-one fill matches.
    bool isZero = true;
    bool isOne = true;
    for (int index = 0; index < 4; ++index)
    {
        isZero = isZero && context.psMaterialColor[index] == 0.0f;
        isOne = isOne && context.psMaterialColor[index] == 1.0f;
    }
    Check(!isZero, "material color is not a zero default");
    Check(!isOne, "material color is not an opaque white default");

    // Alpha is carried, not clamped or dropped.
    Check(context.psMaterialColor[3] == kFixtureMaterialColor[3],
        "material color alpha is carried");

    // A different supplied color must reach the payload unchanged, which rules out a
    // constant baked into the builder.
    Inputs changed = MakeFixtureA();
    changed.materialColor.rgba[0] = 0.25f;
    changed.materialColor.rgba[1] = 0.5f;
    changed.materialColor.rgba[2] = 0.75f;
    changed.materialColor.rgba[3] = 0.0f;
    Context changedContext;
    Check(world_surface_context::Build(changed, &changedContext, &error),
        "changed material fixture builds");
    const float expected[4] = { 0.25f, 0.5f, 0.75f, 0.0f };
    CheckSameBits(changedContext.psMaterialColor, expected, 4,
        "changed material color is copied verbatim");
}

// The builder must refuse inputs that would produce a NaN or a nonsensical basis, and
// must leave the caller's previous packet untouched when it does.
void TestValidation()
{
    const Inputs good = MakeFixtureA();

    struct Case
    {
        const char* label;
        Error expected;
        Inputs input;
    };

    Case cases[11];
    int count = 0;

    {
        Case& item = cases[count++];
        item.label = "NaN origin";
        item.expected = Error::NonFiniteOrigin;
        item.input = good;
        item.input.camera.origin[1] = std::nanf("");
    }
    {
        Case& item = cases[count++];
        item.label = "infinite origin";
        item.expected = Error::NonFiniteOrigin;
        item.input = good;
        item.input.camera.origin[2] = INFINITY;
    }
    {
        Case& item = cases[count++];
        item.label = "origin out of bounds";
        item.expected = Error::OriginOutOfBounds;
        item.input = good;
        item.input.camera.origin[0] = 1.0e9f;
    }
    {
        Case& item = cases[count++];
        item.label = "NaN axis component";
        item.expected = Error::NonFiniteAxis;
        item.input = good;
        item.input.camera.up[1] = std::nanf("");
    }
    {
        Case& item = cases[count++];
        item.label = "non unit axis";
        item.expected = Error::AxisNotUnitLength;
        item.input = good;
        item.input.camera.forward[0] *= 2.0f;
        item.input.camera.forward[1] *= 2.0f;
        item.input.camera.forward[2] *= 2.0f;
    }
    {
        Case& item = cases[count++];
        item.label = "non orthogonal axis";
        item.expected = Error::AxisNotOrthogonal;
        item.input = good;
        // Unit length (so the length check passes) but not orthogonal to anything.
        item.input.camera.left[0] = 0.0f;
        item.input.camera.left[1] = 0.0f;
        item.input.camera.left[2] = 1.0f;
    }
    {
        Case& item = cases[count++];
        item.label = "zero tan half fov";
        item.expected = Error::InvalidTanHalfFov;
        item.input = good;
        item.input.camera.tanHalfFovX = 0.0f;
    }
    {
        Case& item = cases[count++];
        item.label = "NaN tan half fov";
        item.expected = Error::InvalidTanHalfFov;
        item.input = good;
        item.input.camera.tanHalfFovY = std::nanf("");
    }
    {
        Case& item = cases[count++];
        item.label = "zero zNear";
        item.expected = Error::InvalidZNear;
        item.input = good;
        item.input.camera.zNear = 0.0f;
    }
    {
        Case& item = cases[count++];
        item.label = "NaN fog density";
        item.expected = Error::NonFiniteFogParameters;
        item.input = good;
        item.input.fog.density = std::nanf("");
    }
    {
        Case& item = cases[count++];
        item.label = "NaN material color";
        item.expected = Error::NonFiniteMaterialColor;
        item.input = good;
        item.input.materialColor.rgba[2] = std::nanf("");
    }

    for (int index = 0; index < count; ++index)
    {
        const Case& item = cases[index];

        // Poison the destination so an accidental write is detectable.
        Context context;
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                context.world[row][column] = -1234.5f;
        context.psMaterialColor[0] = -1234.5f;

        Error error = Error::None;
        const bool built = world_surface_context::Build(item.input, &context, &error);

        ++g_checks;
        if (built)
        {
            ++g_failures;
            std::printf("FAIL  validation accepted: %s\n", item.label);
            continue;
        }

        if (error != item.expected)
        {
            ++g_failures;
            std::printf("FAIL  validation error mismatch for %s: got %s expected %s\n",
                item.label, world_surface_context::ErrorName(error),
                world_surface_context::ErrorName(item.expected));
        }

        Check(context.world[0][0] == -1234.5f, "rejected input leaves the output untouched");
        Check(context.psMaterialColor[0] == -1234.5f,
            "rejected input leaves the pixel constants untouched");
    }

    // A null destination is reported, not dereferenced.
    Error error = Error::None;
    Check(!world_surface_context::Build(good, nullptr, &error), "null output is rejected");
    Check(error == Error::NullOutput, "null output reports NullOutput");

    // The same builder accepts the unmodified fixture, so the failures above are not a
    // builder that rejects everything.
    Context context;
    Check(world_surface_context::Build(good, &context, &error), "control input still builds");
}

// Legal edge values must be accepted rather than mistaken for a disabled or invalid state.
void TestLegalEdgeValues()
{
    Inputs input = MakeFixtureA();
    input.fog.density = 0.0f;
    input.fog.fogStart = 0.0f;

    Context context;
    Error error = Error::None;
    Check(world_surface_context::Build(input, &context, &error), "zero density builds");
    Check(context.vsFog[2] == 0.0f, "zero density gives a zero z term");
    Check(context.vsFog[3] == 0.0f, "zero density zeroes the start product");
    Check(context.vsFog[1] == 1.0f, "zero density still enables the fog vector");

    // Enabled fog with zero density still unpacks the color: enabled is decided by the
    // flag, not by the numeric values.
    Check(context.psFogColor[0] != kPreviousFogColor[0] ||
            context.psFogColor[1] != kPreviousFogColor[1] ||
            context.psFogColor[2] != kPreviousFogColor[2] ||
            context.psFogColor[3] != kPreviousFogColor[3],
        "enabled fog with zero density still unpacks a color");

    // A near plane other than 1 must scale the projection's near term linearly.
    Inputs near = MakeFixtureA();
    near.camera.zNear = 1.0f;
    Context nearContext;
    Check(world_surface_context::Build(near, &nearContext, &error), "zNear 1 builds");
    CheckClose(nearContext.projection[3][2],
        -1.0f * world_surface_context::kMax11BitFloat, "zNear 1 near term");
    CheckClose(nearContext.projection[3][2] * kFixtureZNear, context.projection[3][2] * 1.0f,
        "near term scales with zNear");

    // Only the near term depends on zNear; the scales must be untouched.
    Check(nearContext.projection[0][0] == context.projection[0][0],
        "zNear does not affect the x scale");
    Check(nearContext.projection[1][1] == context.projection[1][1],
        "zNear does not affect the y scale");
}

} // namespace

int main()
{
    std::printf("world_surface_context: asset-free numeric tests\n");

    TestBasisProvenance();
    TestFrozenMatrices();
    TestRegisterRouting();
    TestIndependentProjectedPoint();
    TestOriginAppliedOnce();
    TestCameraLeftMapsToNegativeViewX();
    TestZeroAngleProductionBasis();
    TestHandednessDomain();
    TestNonFinitePayloadRejection();
    TestFogEnabledConstants();
    TestFogDisabledPreservesColor();
    TestMaterialColorCopied();
    TestValidation();
    TestLegalEdgeValues();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures != 0)
    {
        std::printf("VERDICT: FAIL\n");
        return 1;
    }

    std::printf("VERDICT: PASS\n");
    return 0;
}

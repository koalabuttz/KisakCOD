// tests/linux_renderer/world_surface_context.h
//
// Bounded, engine-independent numeric context for one unlit world surface draw.
//
// Scope
// -----
// This header turns an explicitly supplied camera, material color and fog state into
// the exact float payloads the engine would upload for five shader constants:
//
//   VS c4..c7   code 60  CONST_SRC_CODE_TRANSPOSE_WORLD_MATRIX
//   VS c0..c3   code 76  CONST_SRC_CODE_TRANSPOSE_VIEW_PROJECTION_MATRIX
//   VS c21      code 41  CONST_SRC_CODE_FOG
//   PS c0       code 42  CONST_SRC_CODE_FOG_COLOR
//   PS c1       code 40  CONST_SRC_CODE_MATERIAL_COLOR
//
// It exists so the numeric layer can be tested without a device, an asset, or the
// renderer. It is not a renderer: it performs no D3D call, no draw, and no engine
// state change, and it does not model shader execution.
//
// Provenance of the formulas
// --------------------------
// Every formula below is an audited equivalent of the engine's own code path, copied
// operation by operation from:
//
//   src/universal/com_math.cpp   MatrixForViewer          (view matrix)
//   src/universal/com_math.cpp   InfinitePerspectiveMatrix(projection)
//   src/universal/com_math.cpp   MatrixMultiply44         (V*P, double accumulation)
//   src/universal/com_math.cpp   MatrixTransformVector44  (row-3 eye-offset transform)
//   src/universal/com_math.cpp   MatrixTranspose44        (register payload layout)
//   src/universal/com_math.h     MAX_11BIT_FLT = 0.99951172f
//   src/gfx_d3d/r_cmdbuf.cpp     R_CmdBufSet3D            (world = I, row3 = -eyeOffset)
//   src/gfx_d3d/r_state.cpp      R_GetCodeMatrix          (transpose variants)
//   src/gfx_d3d/r_state.cpp      R_DeriveViewProjectionMatrix (row-3 eye offset)
//   src/gfx_d3d/r_state_utils.cpp R_Set3D                (eyeOffset from origin)
//   src/gfx_d3d/rb_fog.cpp       R_SetFrameFog            (fog parms and fog color)
//   src/gfx_d3d/r_utils.cpp      Byte4UnpackBgra          (BGRA byte -> float)
//   src/gfx_d3d/r_state.h        constant-source codes 40/41/42/60/76
//
// The math is duplicated here rather than reused from src/universal/com_math.cpp
// because this test tree is deliberately standalone: tests/linux_renderer includes no
// engine headers and links no engine objects. The duplication is therefore an
// audited equivalent, not a shared implementation, and passing these tests is NOT a
// production-parity or image-parity claim. The production path remains the single
// source of truth until the numeric layer is extracted behind a shared helper.
//
// Precision rule
// --------------
// The engine keeps each four-term dot product in double and rounds once, when the
// float is stored (x87-era behaviour it deliberately preserves). Reproducing the
// stored values therefore requires the same rule; doing the arithmetic in float
// produces different low bits. Every accumulation below is written as
// (double)a*(double)b terms added left to right and cast once to float.
//
// Conventions
// -----------
// * Matrices are row-major float[4][4] and follow the engine's row-vector convention:
//   a point transforms as out[j] = sum_i v[i] * M[i][j], so translation lives in
//   row 3 and NDC/eye space come from the composition order the engine uses.
// * The camera basis is the engine's CoD basis as AnglesToAxis builds it:
//   forward = axis[0], left = axis[1], up = axis[2], with left == up x forward.
//   AnglesToAxis stores AngleVectors' right vector negated, so the engine's left is the
//   negated right and forward x up is the *right* axis, not the left one.
//   MatrixForViewer negates axis[1] so that view-space +x is screen right, which is the
//   left-handed D3D9 view.
// * No engine header, asset, device, or global state is touched.

#pragma once

#include <cstdint>

namespace world_surface_context {

// ---------------------------------------------------------------------------
// Constant-source codes. Values match src/gfx_d3d/r_state.h exactly.
// ---------------------------------------------------------------------------
constexpr std::uint32_t kCodeMaterialColor = 40;                     // 0x28
constexpr std::uint32_t kCodeFog = 41;                               // 0x29
constexpr std::uint32_t kCodeFogColor = 42;                          // 0x2A
constexpr std::uint32_t kCodeTransposeWorldMatrix = 60;              // 0x3C
constexpr std::uint32_t kCodeTransposeViewProjectionMatrix = 76;     // 0x4C

// Destination register spans. The matrix constants are four consecutive registers
// (firstRow 0); the vertex fog vector and both pixel constants are single registers.
constexpr std::uint32_t kVsDestTransposeViewProjection = 0;
constexpr std::uint32_t kVsDestTransposeWorld = 4;
constexpr std::uint32_t kVsDestFog = 21;
constexpr std::uint32_t kPsDestFogColor = 0;
constexpr std::uint32_t kPsDestMaterialColor = 1;

// MAX_11BIT_FLT (src/universal/com_math.h). The infinite projection uses this
// value, not exactly 1.0, for the z scale.
constexpr float kMax11BitFloat = 0.99951172f;

// Scale that Byte4UnpackBgra multiplies each byte by; the double literal is the one
// written in the engine source, so the products match bit for bit.
constexpr double kByteToUnit = 0.003921568859368563;

// ---------------------------------------------------------------------------
// Validation limits.
//
// The engine only asserts zNear > 0 and relies on its own callers for a sane basis.
// A standalone builder has to be stricter, because a NaN or a skewed basis would
// silently produce a NaN register payload that is very hard to read back.
// ---------------------------------------------------------------------------

// Mirrors com_math.h's EQUAL_EPSILON, widened slightly: an engine basis assembled
// from float trigonometry is orthonormal to roughly 1e-7, so this only rejects bases
// that are genuinely wrong rather than merely rounded.
constexpr float kAxisTolerance = 1.0e-3f;

// Origins beyond this keep the double products meaningful but make the single float
// rounding coarse; reject them instead of pretending the result is usable. This is a
// guard chosen here, not an engine limit.
constexpr float kMaxCoordinateMagnitude = 1.0e6f;

// tanHalfFov is used as a divisor, so it must be strictly positive; the upper bound
// keeps 1/tan finite and the projection scale interpretable.
constexpr float kMaxTanHalfFov = 1.0e3f;

// zNear must be positive (InfinitePerspectiveMatrix asserts this); the upper bound
// matches the coordinate guard above.
constexpr float kMaxZNear = 1.0e6f;

// Fog parameters and colors are only checked for finiteness and this magnitude, since
// the engine applies no further restriction (r_fog disabled writes zeros).
constexpr float kMaxFogMagnitude = 1.0e6f;

// ---------------------------------------------------------------------------
// Inputs. Every field is supplied explicitly; nothing has a usable engine default.
// ---------------------------------------------------------------------------

// Camera supplied by the caller. There is no captured refdef here: a camera declared
// this way is legitimate view context, but it is not a replay of any recorded frame.
struct CameraInputs
{
    // World-space eye position. The engine's camera path sets origin.w = 1.0, so the
    // "origin.w == 0 means no eye offset" branch of R_Set3D does not apply.
    float origin[3];

    // Orthonormal CoD basis, each a unit vector, left == up x forward (the convention
    // AnglesToAxis produces; see the note in the validation section).
    float forward[3];
    float left[3];
    float up[3];

    // Half-angle tangents. Deliberately separate values: a real viewport is usually
    // asymmetric, and a single uniform value would hide a swapped x/y assignment.
    float tanHalfFovX;
    float tanHalfFovY;

    // Must be strictly positive.
    float zNear;
};

// Material color for code 40. The engine copies an externally supplied vec4 here; it
// derives nothing from the material or the map, so this struct has no default and no
// initializer: the caller must state the value. Ordered RGBA as uploaded.
struct MaterialColorInput
{
    float rgba[4];
};

struct FogInputs
{
    // r_fog. When false the engine leaves code 42 untouched, so previousColor is what
    // the pixel stage actually keeps reading.
    bool enabled;

    // Used only when enabled: code 41 becomes (0, 1, -density, fogStart * density).
    float density;
    float fogStart;

    // Fog color as the B, G, R, A bytes stored in the frame fog settings. Only used
    // when enabled.
    unsigned char colorBgra[4];

    // Code 42 as it already stands in the constant buffer. Copied verbatim when
    // enabled is false. Callers that need the disabled case must supply the real
    // previous value; inventing a black default would misreport the engine state.
    float previousColor[4];
};

struct Inputs
{
    CameraInputs camera;
    MaterialColorInput materialColor;
    FogInputs fog;
};

// ---------------------------------------------------------------------------
// Outputs.
// ---------------------------------------------------------------------------

// One constant span as it would be handed to the shader.
struct ConstantRows
{
    std::uint32_t code;
    std::uint32_t firstRegister;
    std::uint32_t rowCount;
    float rows[4][4]; // only rowCount rows are meaningful
};

// Intermediate matrices are kept so tests can compare each stage, and so a reader can
// see which one is uploaded in transposed form.
struct Context
{
    // Identity with row 3 = -origin (R_CmdBufSet3D). Ordinary world geometry uses this
    // placement, so it is NOT the identity matrix whenever the camera origin is
    // nonzero, and the eye offset appears exactly once.
    float world[4][4];

    // MatrixForViewer output.
    float view[4][4];

    // InfinitePerspectiveMatrix output.
    float projection[4][4];

    // view * projection (MatrixMultiply44).
    float viewProjection[4][4];

    // viewProjection with row 3 replaced by the caller-supplied eye offset transformed
    // through it (R_DeriveViewProjectionMatrix). The depth-hack branch is not modeled:
    // the BSP/world path selects depthHackFlags 0.
    float derivedViewProjection[4][4];

    // Upload payload, already transposed as R_GetCodeMatrix returns it.
    ConstantRows vsTransposeWorld;            // VS c4..c7
    ConstantRows vsTransposeViewProjection;   // VS c0..c3

    // Single-register constants.
    float vsFog[4];          // VS c21
    float psFogColor[4];     // PS c0
    float psMaterialColor[4];// PS c1
};

enum class Error
{
    None = 0,
    NullOutput,
    NonFiniteOrigin,
    OriginOutOfBounds,
    NonFiniteAxis,
    AxisNotUnitLength,
    AxisNotOrthogonal,
    AxisNotLeftHanded,
    InvalidTanHalfFov,
    InvalidZNear,
    NonFiniteFogParameters,
    NonFiniteFogColor,
    NonFiniteMaterialColor,
    // Scalar checks passed but the derived matrices or payload overflowed. See
    // AllFinite below.
    NonFinitePayload,
};

inline const char* ErrorName(Error error)
{
    switch (error)
    {
    case Error::None: return "None";
    case Error::NullOutput: return "NullOutput";
    case Error::NonFiniteOrigin: return "NonFiniteOrigin";
    case Error::OriginOutOfBounds: return "OriginOutOfBounds";
    case Error::NonFiniteAxis: return "NonFiniteAxis";
    case Error::AxisNotUnitLength: return "AxisNotUnitLength";
    case Error::AxisNotOrthogonal: return "AxisNotOrthogonal";
    case Error::AxisNotLeftHanded: return "AxisNotLeftHanded";
    case Error::InvalidTanHalfFov: return "InvalidTanHalfFov";
    case Error::InvalidZNear: return "InvalidZNear";
    case Error::NonFiniteFogParameters: return "NonFiniteFogParameters";
    case Error::NonFiniteFogColor: return "NonFiniteFogColor";
    case Error::NonFiniteMaterialColor: return "NonFiniteMaterialColor";
    case Error::NonFinitePayload: return "NonFinitePayload";
    }
    return "Unknown";
}

// Builds the five constant spans. Returns false and sets *error on invalid input; on
// failure *output is left untouched, so a caller can keep a previous packet.
inline bool Build(const Inputs& input, Context* output, Error* error);

// ---------------------------------------------------------------------------
// Implementation.
// ---------------------------------------------------------------------------

namespace detail {

inline bool IsFinite(float value)
{
    // Written as a comparison rather than std::isfinite so the check cannot be folded
    // away by a fast-math flag on a test build.
    return value == value && value - value == 0.0f;
}

struct Vec3
{
    float value[3];
};

inline float Dot(const Vec3& a, const Vec3& b)
{
    return a.value[0] * b.value[0] + a.value[1] * b.value[1] + a.value[2] * b.value[2];
}

inline Vec3 Cross(const Vec3& a, const Vec3& b)
{
    Vec3 out;
    out.value[0] = a.value[1] * b.value[2] - a.value[2] * b.value[1];
    out.value[1] = a.value[2] * b.value[0] - a.value[0] * b.value[2];
    out.value[2] = a.value[0] * b.value[1] - a.value[1] * b.value[0];
    return out;
}

inline void Zero44(float m[4][4])
{
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            m[row][column] = 0.0f;
}

// com_math.cpp MatrixMultiply44: each element is a four-term dot product accumulated
// in double and rounded once.
inline void Multiply44(const float a[4][4], const float b[4][4], float out[4][4])
{
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            out[row][column] = (float)((double)a[row][0] * (double)b[0][column]
                + (double)a[row][1] * (double)b[1][column]
                + (double)a[row][2] * (double)b[2][column]
                + (double)a[row][3] * (double)b[3][column]);
        }
    }
}

// com_math.cpp MatrixTransformVector44: out = vec (as a row) * mat, again with double
// accumulators and a single rounding.
inline void Transform44(const float vec[4], const float mat[4][4], float out[4])
{
    float result[4];
    for (int column = 0; column < 4; ++column)
    {
        result[column] = (float)((double)vec[0] * (double)mat[0][column]
            + (double)vec[1] * (double)mat[1][column]
            + (double)vec[2] * (double)mat[2][column]
            + (double)vec[3] * (double)mat[3][column]);
    }
    for (int column = 0; column < 4; ++column)
        out[column] = result[column];
}

// com_math.cpp MatrixTranspose44.
inline void Transpose44(const float in[4][4], float out[4][4])
{
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            out[row][column] = in[column][row];
}

// com_math.cpp MatrixForViewer. Column 0 is -axis[1], column 1 is axis[2], column 2
// is axis[0]; row 3 is the negated origin projected onto those columns, computed from
// the already-stored float entries exactly as the engine does.
inline void ForViewer(const float origin[3], const float axis[3][3], float out[4][4])
{
    out[0][0] = (float)(-(double)axis[1][0]);
    out[1][0] = (float)(-(double)axis[1][1]);
    out[2][0] = (float)(-(double)axis[1][2]);
    out[3][0] = (float)(-((double)origin[0] * (double)out[0][0]
        + (double)origin[1] * (double)out[1][0]
        + (double)origin[2] * (double)out[2][0]));

    out[0][1] = axis[2][0];
    out[1][1] = axis[2][1];
    out[2][1] = axis[2][2];
    out[3][1] = (float)(-((double)origin[0] * (double)out[0][1]
        + (double)origin[1] * (double)out[1][1]
        + (double)origin[2] * (double)out[2][1]));

    out[0][2] = axis[0][0];
    out[1][2] = axis[0][1];
    out[2][2] = axis[0][2];
    out[3][2] = (float)(-((double)origin[0] * (double)out[0][2]
        + (double)origin[1] * (double)out[1][2]
        + (double)origin[2] * (double)out[2][2]));

    out[0][3] = 0.0f;
    out[1][3] = 0.0f;
    out[2][3] = 0.0f;
    out[3][3] = 1.0f;
}

// com_math.cpp InfinitePerspectiveMatrix. Not a finite-far projection: the z scale is
// kMax11BitFloat itself and the translation term is -(zNear * kMax11BitFloat).
inline void InfinitePerspective(float tanHalfFovX, float tanHalfFovY, float zNear, float out[4][4])
{
    Zero44(out);
    out[0][0] = (float)((double)kMax11BitFloat / (double)tanHalfFovX);
    out[1][1] = (float)((double)kMax11BitFloat / (double)tanHalfFovY);
    out[2][2] = kMax11BitFloat;
    out[2][3] = 1.0f;
    out[3][2] = (float)(-(double)zNear * (double)kMax11BitFloat);
}

// r_cmdbuf.cpp R_CmdBufSet3D: identity, then row 3 xyz = 0 - eyeOffset.xyz. Row 3 w
// stays 1.0.
inline void WorldMatrix(const float origin[3], float out[4][4])
{
    Zero44(out);
    out[0][0] = 1.0f;
    out[1][1] = 1.0f;
    out[2][2] = 1.0f;
    out[3][0] = 0.0f - origin[0];
    out[3][1] = 0.0f - origin[1];
    out[3][2] = 0.0f - origin[2];
    out[3][3] = 1.0f;
}

// r_utils.cpp Byte4UnpackBgra: memory order B, G, R, A becomes R, G, B, A.
inline void UnpackBgra(const unsigned char from[4], float out[4])
{
    out[0] = (float)((double)from[2] * kByteToUnit);
    out[1] = (float)((double)from[1] * kByteToUnit);
    out[2] = (float)((double)from[0] * kByteToUnit);
    out[3] = (float)((double)from[3] * kByteToUnit);
}

// True only if every stored intermediate and every payload word is finite.
//
// Scalar range checks cannot catch two real overflow routes:
//   * a positive but tiny tangent makes MAX_11BIT_FLT / tangent overflow to infinity,
//     and an upper bound on the tangent does not bound its reciprocal;
//   * a finite reciprocal can still overflow the translation product
//     (eye offset times view-projection) when combined with a permitted large origin.
// Checking the finished packet covers both, and covers any future change to the scalar
// bounds, so this is the guarantee rather than a redundant assertion.
inline bool AllFinite(const Context& context)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            if (!IsFinite(context.world[row][column])
                || !IsFinite(context.view[row][column])
                || !IsFinite(context.projection[row][column])
                || !IsFinite(context.viewProjection[row][column])
                || !IsFinite(context.derivedViewProjection[row][column])
                || !IsFinite(context.vsTransposeWorld.rows[row][column])
                || !IsFinite(context.vsTransposeViewProjection.rows[row][column]))
                return false;
        }
    }

    for (int index = 0; index < 4; ++index)
    {
        if (!IsFinite(context.vsFog[index])
            || !IsFinite(context.psFogColor[index])
            || !IsFinite(context.psMaterialColor[index]))
            return false;
    }

    return true;
}

} // namespace detail

inline bool Build(const Inputs& input, Context* output, Error* error)
{
    const CameraInputs& camera = input.camera;

    auto fail = [error](Error value) {
        if (error)
            *error = value;
        return false;
    };

    if (!output)
        return fail(Error::NullOutput);

    // --- Camera origin -----------------------------------------------------
    for (int index = 0; index < 3; ++index)
    {
        if (!detail::IsFinite(camera.origin[index]))
            return fail(Error::NonFiniteOrigin);
        if (camera.origin[index] > kMaxCoordinateMagnitude
            || camera.origin[index] < -kMaxCoordinateMagnitude)
            return fail(Error::OriginOutOfBounds);
    }

    // --- Camera basis ------------------------------------------------------
    // all three vectors finite, unit length, mutually orthogonal, and left == up x forward
    detail::Vec3 basis[3];
    const float* supplied[3] = { camera.forward, camera.left, camera.up };
    for (int axisIndex = 0; axisIndex < 3; ++axisIndex)
    {
        for (int component = 0; component < 3; ++component)
        {
            if (!detail::IsFinite(supplied[axisIndex][component]))
                return fail(Error::NonFiniteAxis);
            basis[axisIndex].value[component] = supplied[axisIndex][component];
        }
    }

    for (int axisIndex = 0; axisIndex < 3; ++axisIndex)
    {
        const float lengthSquared = detail::Dot(basis[axisIndex], basis[axisIndex]);
        if (lengthSquared < (1.0f - kAxisTolerance) || lengthSquared > (1.0f + kAxisTolerance))
            return fail(Error::AxisNotUnitLength);
    }

    const int pairs[3][2] = { { 0, 1 }, { 0, 2 }, { 1, 2 } };
    for (int pair = 0; pair < 3; ++pair)
    {
        const float dot = detail::Dot(basis[pairs[pair][0]], basis[pairs[pair][1]]);
        if (dot > kAxisTolerance || dot < -kAxisTolerance)
            return fail(Error::AxisNotOrthogonal);
    }

    // The engine's camera basis is the one AnglesToAxis produces, which equals
    // AngleVectors with its right vector negated:
    //
    //     axis[1][0] = sr*sp*cy + -sy*cr      (src/universal/com_math_anglevectors.cpp)
    //
    // so the relation to check is left == up x forward. At zero angles the engine yields
    // forward (1,0,0), left (0,1,0), up (0,0,1), which satisfies up x forward. Checking
    // left == forward x up instead would reject every real camera, because forward x up
    // is the un-negated right vector (0,-1,0).
    {
        const detail::Vec3 expectedLeft = detail::Cross(basis[2], basis[0]);
        for (int component = 0; component < 3; ++component)
        {
            const float difference = expectedLeft.value[component] - basis[1].value[component];
            if (difference > kAxisTolerance || difference < -kAxisTolerance)
                return fail(Error::AxisNotLeftHanded);
        }
    }

    // --- Projection parameters --------------------------------------------
    if (!detail::IsFinite(camera.tanHalfFovX) || !detail::IsFinite(camera.tanHalfFovY))
        return fail(Error::InvalidTanHalfFov);
    if (camera.tanHalfFovX <= 0.0f || camera.tanHalfFovY <= 0.0f)
        return fail(Error::InvalidTanHalfFov);
    if (camera.tanHalfFovX > kMaxTanHalfFov || camera.tanHalfFovY > kMaxTanHalfFov)
        return fail(Error::InvalidTanHalfFov);

    if (!detail::IsFinite(camera.zNear) || camera.zNear <= 0.0f || camera.zNear > kMaxZNear)
        return fail(Error::InvalidZNear);

    // --- Fog --------------------------------------------------------------
    if (!detail::IsFinite(input.fog.density) || !detail::IsFinite(input.fog.fogStart))
        return fail(Error::NonFiniteFogParameters);
    if (input.fog.density > kMaxFogMagnitude || input.fog.density < -kMaxFogMagnitude)
        return fail(Error::NonFiniteFogParameters);
    if (input.fog.fogStart > kMaxFogMagnitude || input.fog.fogStart < -kMaxFogMagnitude)
        return fail(Error::NonFiniteFogParameters);

    // previousColor is used verbatim when fog is disabled, so it has to be usable
    // state either way; a NaN there would silently poison the packet.
    for (int index = 0; index < 4; ++index)
    {
        if (!detail::IsFinite(input.fog.previousColor[index]))
            return fail(Error::NonFiniteFogColor);
    }

    // --- Material color ---------------------------------------------------
    for (int index = 0; index < 4; ++index)
    {
        if (!detail::IsFinite(input.materialColor.rgba[index]))
            return fail(Error::NonFiniteMaterialColor);
    }

    // --- All validated: compute into a temporary ---------------------------
    // Nothing is written to *output until the whole packet is known to be finite, so a
    // rejected input leaves the caller's previous packet intact.
    Context staged;

    const float axis[3][3] = {
        { camera.forward[0], camera.forward[1], camera.forward[2] },
        { camera.left[0], camera.left[1], camera.left[2] },
        { camera.up[0], camera.up[1], camera.up[2] },
    };

    detail::WorldMatrix(camera.origin, staged.world);
    detail::ForViewer(camera.origin, axis, staged.view);
    detail::InfinitePerspective(camera.tanHalfFovX, camera.tanHalfFovY, camera.zNear,
        staged.projection);
    detail::Multiply44(staged.view, staged.projection, staged.viewProjection);

    // R_DeriveViewProjectionMatrix: copy viewProjection, then replace row 3 with the
    // eye offset (w = 1) transformed through it. The eye offset must not be applied a
    // second time anywhere else; the world matrix above already carries the negated
    // origin and the two cancel algebraically.
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            staged.derivedViewProjection[row][column] = staged.viewProjection[row][column];

    const float eyeOffset[4] = { camera.origin[0], camera.origin[1], camera.origin[2], 1.0f };
    detail::Transform44(eyeOffset, staged.viewProjection, staged.derivedViewProjection[3]);

    // R_GetCodeMatrix hands back the transposed matrix for these two codes, so the
    // payload is transposed once and once only.
    detail::Transpose44(staged.world, staged.vsTransposeWorld.rows);
    staged.vsTransposeWorld.code = kCodeTransposeWorldMatrix;
    staged.vsTransposeWorld.firstRegister = kVsDestTransposeWorld;
    staged.vsTransposeWorld.rowCount = 4;

    detail::Transpose44(staged.derivedViewProjection, staged.vsTransposeViewProjection.rows);
    staged.vsTransposeViewProjection.code = kCodeTransposeViewProjectionMatrix;
    staged.vsTransposeViewProjection.firstRegister = kVsDestTransposeViewProjection;
    staged.vsTransposeViewProjection.rowCount = 4;

    // r_fog.cpp R_SetFrameFog. The negate and the multiply are float operations in the
    // engine, not double, so they stay float here.
    if (input.fog.enabled)
    {
        staged.vsFog[0] = 0.0f;
        staged.vsFog[1] = 1.0f;
        staged.vsFog[2] = -input.fog.density;
        staged.vsFog[3] = input.fog.fogStart * input.fog.density;
        detail::UnpackBgra(input.fog.colorBgra, staged.psFogColor);
    }
    else
    {
        // Only code 41 is rewritten; code 42 keeps whatever state it already had.
        staged.vsFog[0] = 0.0f;
        staged.vsFog[1] = 1.0f;
        staged.vsFog[2] = 0.0f;
        staged.vsFog[3] = 0.0f;
        for (int index = 0; index < 4; ++index)
            staged.psFogColor[index] = input.fog.previousColor[index];
    }

    // Code 40 is copied from the caller. Nothing is derived and no default is applied.
    for (int index = 0; index < 4; ++index)
        staged.psMaterialColor[index] = input.materialColor.rgba[index];

    // Commit only a fully finite packet.
    if (!detail::AllFinite(staged))
        return fail(Error::NonFinitePayload);

    *output = staged;

    if (error)
        *error = Error::None;
    return true;
}

} // namespace world_surface_context

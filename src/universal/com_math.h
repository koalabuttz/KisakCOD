#pragma once

#include <universal/assertive.h>

#include <math.h>
#include <cstdint>

#ifdef __linux__
// This header is included on its own (database, sound, UI), so it carries the
// same non-Clang compiler spellings as q_shared.h: Clang has __cdecl/__int32 as
// keywords under -fms-extensions, GCC needs the GNU attributes. The #ifndef
// guards make a preceding q_shared.h include win.
#if !defined(__clang__)
#if defined(__i386__) || defined(__x86_64__)
#ifndef __cdecl
#define __cdecl __attribute__((cdecl))
#endif
#ifndef __stdcall
#define __stdcall __attribute__((stdcall))
#endif
#ifndef __declspec
#define __declspec(x) __attribute__((x))
#endif
#endif
#ifndef __int32
#define __int32 int
#endif
#endif // !__clang__
#endif // __linux__

#define EQUAL_EPSILON 0.001f
#define ZERO_EPSILON 0.000001f
#define WEIGHT_EPSILON EQUAL_EPSILON

#define MAX_11BIT_FLT 0.99951172f // not a real name

#define CLAMP(x, low, high) ((x) < (low) ? (low) : ((x) > (high) ? (high) : (x)))
#ifdef __linux__
// _isnan is an MSVC CRT intrinsic; the C library's isnan is the same predicate
// (nonzero iff the value is a NaN).
#define IS_NAN(x) isnan(x)
// __forceinline is a Microsoft keyword: Clang accepts it under -fms-extensions,
// GCC needs the equivalent attribute spelled out.
#if !defined(__clang__) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif
#else
#define IS_NAN(x) _isnan(x)
#endif

static const float MPH_TO_INCHES_PER_SEC = 17.6f;

struct cplane_s // sizeof=0x14
{                                       // ...
    float normal[3];                    // ...
    float dist;
    uint8_t type;
    uint8_t signbits;
    uint8_t pad[2];
};

union PackedUnitVec // sizeof=0x4
{                                       // ...
    operator uint32_t()
    {
        return packed;
    }
    operator int()
    {
        return packed;
    }
    uint32_t packed;
    uint8_t array[4];
};

using vec2 = float[2];
using vec3 = float[3];
using vec4 = float[4];

// TODO change if we ever actually use classes
#define vec2r float*
#define vec3r float*
#define vec4r float*

// note - row major order
using mat3x3 = float[3][3];
using mat4x3 = float[4][3];
using mat4x4 = float[4][4];

struct GfxMatrix; // sizeof=0x40

// TODO fun fact: if we initialize these to -0.0 instead the compiler can remove the float addition without -ffast-math or some equivalent
constexpr vec2 vec2_origin = { 0.0, 0.0 };
constexpr vec3 vec3_origin = { 0.0, 0.0, 0.0 };
constexpr vec4 vec4_origin = { 0.0, 0.0, 0.0, 0.0 };

using uint4 = uint32_t[4];

// NOTE: yes, these really seem to be different matrix types specific to the FX system for some reason..
union float4 {
    vec4 v;
    uint4 u;
    PackedUnitVec unitVec[4];
};

union float4x3 {
    union {
        float4 x;
        float4 y;
        float4 z;
    };

    mat4x3 mat;
};

union float4x4 {
    struct {
        float4 x;
        float4 y;
        float4 z;
        float4 w;
    };

    mat4x4 mat;
};

constexpr float4 g_zero = {
    .v = {
        0.0, 0.0, 0.0, 0.0
    }
};

constexpr float4 g_unit = {
    .v = {
        0.0, 0.0, 0.0, 1.0
    }
};

constexpr float4 g_2xunit = {
    .v = {
        0.0, 0.0, 0.0, 1.0
    }
};

constexpr float4 g_swizzleXYZA = {
    .u = {
        0x00010203,
        0x04050607,
        0x08090A0B,
        0x10111213 // this row goes over 16
    }
};
// each individual member never goes over 16
constexpr float4 g_swizzleYZXW = {
    .u = {
        0x04050607,
        0x08090A0B,
        0x00010203,
        0x0C0D0E0F
    }
};

constexpr float4 g_keepXYW = {
    .u = {
        0xFFFFFFFF,
        0xFFFFFFFF,
        0,
        0xFFFFFFFF
    }
};

constexpr float4 g_keepXYZ = {
    .u = {
        0xFFFFFFFF,
        0xFFFFFFFF,
        0xFFFFFFFF,
        0
    }
};


struct vector3
{
    float4 x;
    float4 y;
    float4 z;
};
struct vector4 : vector3
{
    float4 w;
};


bool Vec4IsNormalized(const vec4r v);

void TRACK_com_math();

// == RANDOM == 
void Rand_Init(int seed);

#ifdef __linux__
// POSIX <stdlib.h> declares `long int random(void)`. C++ cannot overload on
// return type, so the engine's `float random()` cannot carry that name in the
// same translation unit. The alias below keeps the call sites unchanged while
// giving the engine's function its own symbol at compile time; the definition in
// com_math.cpp is renamed by the same macro, so declaration and definition stay
// in agreement without touching any other file.
//
// <cstdlib> is included before the alias is defined so the C library's own
// declaration is parsed under its real name; stdlib.h's include guard then makes
// any later include of it a no-op. A proper cross-file rename (definition plus
// call sites) would let this alias disappear entirely.
#include <cstdlib>
float Q_random();
#else
float random();
#endif
float crandom();

#ifdef __linux__
#define random Q_random
#endif

float flrand(float min, float max);
int irand(int min, int max);

uint32_t RandWithSeed(int* seed);
void GaussianRandom(float* f0, float* f1);
void PointInCircleFromUniformDeviates(float radiusDeviate, float yawDeviate, float* point);

// == SCALAR FUNCTIONS ==
float Q_rint(float in);
float Q_acos(float c);
float Q_rsqrt(float number);

float DiffTrack(float tgt, float cur, float rate, float deltaTime);
float DiffTrackAngle(float tgt, float cur, float rate, float deltaTime);

float LinearTrack(float tgt, float cur, float rate, float deltaTime);
float LinearTrackAngle(float tgt, float cur, float rate, float deltaTime);

float GraphGetValueFromFraction(int knotCount, const float (*knots)[2], float fraction);

float AngleDelta(float a1, float a2);
void AnglesSubtract(float* v1, float* v2, float* v3);

void vectosignedangles(const float *vec, float *angles);

float RotationToYaw(const float* rot);
float AngleNormalize360(float angle);
float AngleNormalize180(float angle);
float AngleSubtract(float a1, float a2);

// Angle unit factors, with the exact values the game code uses. A turn is
// 360 degrees; a "short angle" (network and usercmd angles) splits a turn into
// 65536 steps. The _F constants are floats; the plain ones are doubles for
// expressions that multiply in double.
//
// TURNS_PER_DEGREE and SHORTS_PER_DEGREE are the float factors as the
// decompiler printed them (16 digits), which is one double ulp above the float
// widened exactly. The code has always multiplied by these doubles, so they keep
// that value; changing it changes a handful of results in the last bit.
inline constexpr float TURNS_PER_DEGREE_F = 1.0f / 360.0f;
inline constexpr double TURNS_PER_DEGREE = 0.002777777845039964;
inline constexpr float DEGREES_PER_SHORT_F = 360.0f / 65536.0f;
inline constexpr double DEGREES_PER_SHORT = 360.0 / 65536.0;
inline constexpr float SHORTS_PER_DEGREE_F = 65536.0f / 360.0f;
inline constexpr double SHORTS_PER_DEGREE = 182.0444488525391;
static_assert(TURNS_PER_DEGREE_F == 0.0027777778f && TURNS_PER_DEGREE_F == (float)0.0027777778);
static_assert(DEGREES_PER_SHORT_F == 0.0054931641f && DEGREES_PER_SHORT == 0.0054931640625);
static_assert(SHORTS_PER_DEGREE_F == 182.04445f && SHORTS_PER_DEGREE_F == (float)182.04445);
static_assert((float)TURNS_PER_DEGREE == TURNS_PER_DEGREE_F && (float)SHORTS_PER_DEGREE == SHORTS_PER_DEGREE_F);

// Wraps an angle in degrees into [-180, 180] by removing the nearest whole
// turn: the angle wrap the game code inlines (decompiled as "x * 0.0027777778",
// "floor(+ 0.5)", "* 360"). It is not AngleNormalize180(), which uses fmodf
// and can give a different last bit for the same input. Inline copies that
// scale by the double TURNS_PER_DEGREE differ from this for two inputs out of
// 2^32, so they stay written out.
inline float AngleWrap180(float angle)
{
    const float turns = angle * TURNS_PER_DEGREE_F;
    return (turns - floorf(turns + 0.5f)) * 360.0f;
}

void FastSinCos(float radians, float *s, float *c);
void MatrixRotationX(float mat[3][3], float degree);
void MatrixRotationY(float mat[3][3], float degree);
void MatrixRotationZ(float mat[3][3], float degree);

void Vec3Basis_LeftHanded(const float *forward, float *right, float *up);
float Vec3Distance(const float *v1, const float *v2);
float Vec3DistanceSq(const float *p1, const float *p2);


float ColorNormalize(const float *in, float *out);

float PitchForYawOnNormal(float fYaw, const float* normal);

// == PACKING ==
uint8_t DirToByte(const float *dir);
void ByteToDir(uint32_t b, float *dir);

// == ARCs ==
int IsPosInsideArc(
    const float *pos,
    float posRadius,
    const float *arcOrigin,
    float arcRadius,
    float arcAngle0,
    float arcAngle1,
    float arcHalfHeight);

// == VECTOR FUNCTIONS ==
float Vec2Distance(const vec2r v1, const vec2r v2);
float Vec2DistanceSq(const vec2r p1, const vec2r p2);
float Vec2Normalize(vec2r v);
float Vec2NormalizeTo(const vec2r v, vec2r out);
float Vec2Length(const vec2r v);
float Vec2LengthSq(const vec2r v);
void YawVectors2D(float yaw, vec2r forward, vec2r right);

void Vec2NormalizeFast(float *v);

void Vec3Add(const vec3r a, const vec3r b, vec3r sum);
void Vec3Sub(const vec3r a, const vec3r b, vec3r diff);
void Vec3Mul(const vec3r a, const vec3r b, vec3r product);

void Vec3Negate(const vec3r from, vec3r to);
void Vec3Clear(vec3r v);
void Vec3Avg(const vec3r a, const vec3r b, vec3r result);

float Vec3Dot(const vec3r a, const vec3r b);
void Vec3Cross(const vec3r v0, const vec3r v1, vec3r cross);
float Vec3LengthSq(const vec3r v);
void Vec3Scale(const vec3r v, float scale, vec3r result);
// :)
inline float I_fres(float val)
{
    iassert(val != 0.0);
    return (1.0f / val);
}

void Vec3ScaleMad(float scale0, const vec3r dir0, float scale1, const vec3r dir1, vec3r result);
float Vec3Normalize(float* v);
bool Vec3IsNormalized(const vec3r v);

bool Vec3Compare(const float *a, const float *b);

float Vec3Length(const vec3r v);
void Vec2Copy(const vec2r from, vec2r to);
void Vec3Copy(const vec3r from, vec3r to);
#define VectorCopy(...) \
    typedef char VectorCopy_From_Quake_Is_Vec3Copy_same_args_though[-1]

void Vec3ProjectionCoords(const float *dir, int *xCoord, int *yCoord);
void Vec3NormalizeFast(float *v);
float Vec3NormalizeTo(const vec3r v, vec3r out);
void Vec3Mad(const vec3r start, float scale, const vec3r dir, vec3r result);
#define VectorMA(...) \
    typedef char VectorMA_From_Quake_Is_Vec3Mad[-1]
void Vec3Accum(const float *subTotal, const float *weight, const float *added, float *total);
void Vec3Rotate(const vec3r in, const mat3x3& matrix, vec3r out);
void Vec3RotateTranspose(const vec3r in, const mat3x3& matrix, vec3r out);

void Vec3AddScalar(const float* a, float s, float* sum);

void Vec3Basis_RightHanded(const float *forward, float *left, float *up);

void Vec3Lerp(const float* start, const float* end, float fraction, float* endpos);

int VecNCompareCustomEpsilon(const float* v0, const float* v1, float epsilon, int coordCount);
void RotatePointAroundVector(float* dst, const float* dir, const float* point, float degrees);

void YawVectors(float yaw, float* forward, float* right);

void AngleVectors(const float* angles, float* forward, float* right, float* up);
void AnglesToAxis(const float* angles, float axis[3][3]);
void AxisToQuat(const float (*mat)[3], float* out);
void AxisToSignedAngles(const float (*axis)[3], float *angles);

float PointToBoxDistSq(const float* pt, const float* mins, const float* maxs);

void Vec4Set(float *v, float x, float y, float z, float w);
bool Vec4Compare(const float *a, const float *b);
float Vec4Dot(const float* a, const float* b);
float Vec4Normalize(float* v);
void Vec4Mul(const float* a, const float* b, float* product);
float Vec4LengthSq(const float* v);
void Vec4Scale(const float* v, float scale, float* result);
void Vec4Mad(const float *start, float scale, const float *dir, float *result);
void Vec4Add(const float *a, const float *b, float *sum);
void Vec4Sub(const float *a, const float *b, float *diff);
void Vec4MadMad(
    const float *start,
    float scale0,
    const float *dir0,
    float scale1,
    const float *dir1,
    float *result);

void Vec4Lerp(const float* from, const float* to, float frac, float* result);
float Vec4Length(const float *v);

float vectoyaw(const float *vec);
float vectosignedyaw(const float *vec);
float vectopitch(const float *vec);
float vectosignedpitch(const float *vec);
void vectoangles(const float *vec, float *angles);

void PerpendicularVector(const float *src, float *dst);

void VectorAngleMultiply(float* vec, float angle);

void AxisClear(mat3x3& axis);
void AxisTranspose(const mat3x3& in, mat3x3& out);
void AxisTransformVec3(const mat3x3& axis, const float* vec, float* out);

void YawToAxis(float yaw, mat3x3& axis);
void AxisToAngles(const mat3x3& axis, vec3r angles);

void ProjectPointOntoVector(const float *point, const float *start, const float *end, float *vProj);

// == MATRICES ==
void OrthographicMatrix(mat4x4 &mtx, float width, float height, float depth);

void MatrixIdentity33(mat3x3 &out);
void MatrixIdentity44(mat4x4 &out);

void MatrixSet44(mat4x4& out, const vec3r origin, const mat3x3& axis, float scale);

void MatrixMultiply(const mat3x3& in1, const mat3x3& in2, mat3x3& out);
void MatrixMultiply43(const mat4x3& in1, const mat4x3& in2, mat4x3& out);
void MatrixMultiply44(const mat4x4& in1, const mat4x4& in2, mat4x4& out);

void MatrixTranspose(const mat3x3& in, mat3x3& out);
void MatrixTranspose44(const mat4x4& in, mat4x4& out);

void MatrixInverseOrthogonal43(const mat4x3& in, mat4x3& out);
void MatrixInverse44(const mat4x4& mat, mat4x4& dst);

void MatrixTransformVector(const vec3r in1, const mat3x3& in2, vec3r out);
void MatrixTransformVector43(const vec3r in1, const mat4x3& in2, vec3r out);
void MatrixTransformVector44(const vec4r vec, const mat4x4& mat, vec4r out);

void InvMatrixTransformVectorQuatTrans(const float *in, const struct DObjAnimMat *mat, float *out);

void MatrixTransposeTransformVector(const vec3r in1, const mat3x3& in2, vec3r out);
void MatrixTransposeTransformVector43(const vec3r in1, const mat4x3&, vec3r out);

void MatrixTransformVectorQuatTrans(const vec3r in, const struct DObjAnimMat* mat, vec3r out);

void MatrixForViewer(mat4x4 &mtx, const vec3r origin, const mat3x3 &axis);
void InfinitePerspectiveMatrix(float (*mtx)[4], float tanHalfFovX, float tanHalfFovY, float zNear);

void FinitePerspectiveMatrix(float (*mtx)[4], float tanHalfFovX, float tanHalfFovY, float zNear, float zFar);

float PointToLineDistSq2D(const float *point, const float *start, const float *end);

float LerpAngle(float from, float to, float frac);

void ClearBounds(float *mins, float *maxs);

float RadiusFromBounds(const float *mins, const float *maxs);
float RadiusFromBounds2D(const float *mins, const float *maxs);

float RadiusFromBoundsSq(const float *mins, const float *maxs);
float RadiusFromBounds2DSq(const float *mins, const float *maxs);

void ExtendBounds(float *mins, float *maxs, const float *offset);
void ExpandBoundsToWidth(float *mins, float *maxs);
void ShrinkBoundsToHeight(float *mins, float *maxs);
void ClearBounds2D(float *mins, float *maxs);
void AddPointToBounds(const float *v, float *mins, float *maxs);
void AddPointToBounds2D(const float *v, float *mins, float *maxs);
bool PointInBounds(const float *v, const float *mins, const float *maxs);
bool BoundsOverlap(const float *mins0, const float *maxs0, const float *mins1, const float *maxs1);
void ExpandBounds(const float *addedmins, const float *addedmaxs, float *mins, float *maxs);

int ProjectedWindingContainsCoplanarPoint(
    const float (*verts)[3],
    int vertCount,
    int x,
    int y,
    const float *point);

// == PLANES ==

int IntersectPlanes(const float** plane, float* xyz);
void SnapPointToIntersectingPlanes(const float** planes, float* xyz, float snapGrid, float snapEpsilon);
int PlaneFromPoints(float *plane, const float *v0, const float *v1, const float *v2);
void ProjectPointOnPlane(const float * const f1, const float * const normal, float * const result);
void SetPlaneSignbits(cplane_s *out);
void NearestPitchAndYawOnPlane(const float *angles, const float *normal, float *result);

// == BOXES ==
bool BoxDistSqrdExceeds(const float* absmin, const float* absmax, const float* org, float fogOpaqueDistSqrd);

bool CullBoxFromCone(
    const float *coneOrg,
    const float *coneDir,
    float cosHalfFov,
    const float *boxCenter,
    const float *boxHalfSize);
bool CullBoxFromSphere(const float *sphereOrg, float radius, const float *boxCenter, const float *boxHalfSize);
bool CullBoxFromConicSectionOfSphere(
    const float *coneOrg,
    const float *coneDir,
    float cosHalfFov,
    float radius,
    const float *boxCenter,
    const float *boxHalfSize);
bool CullSphereFromCone(
    const float *coneOrg,
    const float *coneDir,
    float cosHalfFov,
    const float *sphereCenter,
    float radius);
void AxisCopy(const mat3x3 &in, mat3x3& out);

// == QUATERNIONS ==
void AnglesToQuat(const float* angles, float* quat);
void UnitQuatToAngles(const float* quat, float* angles);

void QuatToAxis(const float* quat, mat3x3& axis);
void UnitQuatToAxis(const float* quat, mat3x3& axis);
void UnitQuatToForward(const float* quat, float* forward);
void QuatSlerp(const float* from, const float* to, float frac, float* result);
void QuatMultiply(const float* in1, const float* in2, float* out);
void QuatLerp(const float* qa, const float* qb, float frac, float* out);
void QuatInverse(const float *in, float *out);

// == MISC ==
signed char ClampChar(int i);

void ClosestApproachOfTwoLines(
    const float* p1,
    const float* dir1,
    const float* p2,
    const float* dir2,
    float* s,
    float* t);

// KISAK ADDITION: pray that the optimizer doesn't shit the bed
__forceinline static float COERCE_FLOAT(unsigned val) {
    union {
        unsigned v;
        float f;
    } lol = { val };
    return lol.f;
}

__forceinline static uint32_t COERCE_UNSIGNED_INT(float val) {
    union {
        float f;
        unsigned v;
    } lol = { val };
    return lol.v;
}

__forceinline static int COERCE_INT(float val) {
    union {
        float f;
        int v;
    } lol = { val };
    return lol.v;
}



// LWSS: There appear to be a lot more functions on XBox.
//__Eg_fltMin@@YAXXZ       8278c5f0 f   com_math_anglevectors.obj
//__Eg_negativeZero@@YAXXZ 8278c610 f   com_math_anglevectors.obj
//__Eg_inc@@YAXXZ          8278c630 f   com_math_anglevectors.obj
//__Eg_swizzleXYZW@@YAXXZ  8278c650 f   com_math_anglevectors.obj
//__Eg_swizzleYXZW@@YAXXZ  8278c670 f   com_math_anglevectors.obj
//__Eg_swizzleXZYW@@YAXXZ  8278c690 f   com_math_anglevectors.obj
//__Eg_swizzleYXWZ@@YAXXZ  8278c6b0 f   com_math_anglevectors.obj
//__Eg_swizzleXAZC@@YAXXZ  8278c6d0 f   com_math_anglevectors.obj
//__Eg_swizzleYBWD@@YAXXZ  8278c6f0 f   com_math_anglevectors.obj
//__Eg_swizzleXYAB@@YAXXZ  8278c710 f   com_math_anglevectors.obj
//__Eg_swizzleZWCD@@YAXXZ  8278c730 f   com_math_anglevectors.obj
//__Eg_swizzleXYZA@@YAXXZ  8278c750 f   com_math_anglevectors.obj
//__Eg_swizzleYZXW@@YAXXZ  8278c770 f   com_math_anglevectors.obj
//__Eg_swizzleZXYW@@YAXXZ  8278c790 f   com_math_anglevectors.obj
//__Eg_swizzleWABW@@YAXXZ  8278c7b0 f   com_math_anglevectors.obj
//__Eg_swizzleZWAW@@YAXXZ  8278c7d0 f   com_math_anglevectors.obj
//__Eg_swizzleYZWA@@YAXXZ  8278c7f0 f   com_math_anglevectors.obj
//__Eg_swizzleXXXX@@YAXXZ  8278c810 f   com_math_anglevectors.obj
//__Eg_swizzleYYYY@@YAXXZ  8278c830 f   com_math_anglevectors.obj
//__Eg_swizzleZZZZ@@YAXXZ  8278c850 f   com_math_anglevectors.obj
//__Eg_swizzleWWWW@@YAXXZ  8278c870 f   com_math_anglevectors.obj
//__Eg_selectX@@YAXXZ      8278c890 f   com_math_anglevectors.obj
//__Eg_selectY@@YAXXZ      8278c8b0 f   com_math_anglevectors.obj
//__Eg_selectZ@@YAXXZ      8278c8d0 f   com_math_anglevectors.obj
//__Eg_selectW@@YAXXZ      8278c8f0 f   com_math_anglevectors.obj
//__Eg_keepYZW@@YAXXZ      8278c910 f   com_math_anglevectors.obj
//__Eg_keepXZW@@YAXXZ      8278c930 f   com_math_anglevectors.obj
//__Eg_keepXYW@@YAXXZ      8278c950 f   com_math_anglevectors.obj
//__Eg_keepXYZ@@YAXXZ      8278c970 f   com_math_anglevectors.obj
//__Eg_keepXY@@YAXXZ       8278c990 f   com_math_anglevectors.obj
//__Eg_keepZW@@YAXXZ       8278c9b0 f   com_math_anglevectors.obj
//__Eg_keepX@@YAXXZ        8278c9d0 f   com_math_anglevectors.obj
//__Eg_keepZ@@YAXXZ        8278c9f0 f   com_math_anglevectors.obj
//__Eg_packedMaskQuat@@YAXXZ 8278ca10 f   com_math_anglevectors.obj
//__Eg_unpackSignBitQuat@@YAXXZ 8278ca30 f   com_math_anglevectors.obj
//__Eg_unpackAndQuat@@YAXXZ 8278ca50 f   com_math_anglevectors.obj
//__Eg_packMantissa@@YAXXZ 8278ca70 f   com_math_anglevectors.obj
//__Eg_packedMaskSimpleQuat@@YAXXZ 8278ca90 f   com_math_anglevectors.obj
//__Eg_unpackSignBitSimpleQuat@@YAXXZ 8278cab0 f   com_math_anglevectors.obj
//__Eg_unpackAndSimpleQuat@@YAXXZ 8278cad0 f   com_math_anglevectors.obj
//__Eg_unpackQuatRotateMask@@YAXXZ 8278caf0 f   com_math_anglevectors.obj
//__Eg_unpackQuatSignBit@@YAXXZ 8278cb10 f   com_math_anglevectors.obj
//__Eg_unpackSimpleQuatRotateMask@@YAXXZ 8278cb30 f   com_math_anglevectors.obj
//__Eg_unpackQuatRotatePerm@@YAXXZ 8278cb50 f   com_math_anglevectors.obj
//__Eg_unpackQuatRotateAdd@@YAXXZ 8278cb70 f   com_math_anglevectors.obj
//__Eg_unpackSimpleQuatRotatePerm@@YAXXZ 8278cb90 f   com_math_anglevectors.obj
//__Eg_unpackSimpleQuatRotateAdd@@YAXXZ 8278cbb0 f   com_math_anglevectors.obj
//__Eg_unpackShiftQuat@@YAXXZ 8278cbd0 f   com_math_anglevectors.obj
//__Eg_unpackSignShiftQuat@@YAXXZ 8278cbf0 f   com_math_anglevectors.obj
//__Eg_unpackShiftSimpleQuat@@YAXXZ 8278cc10 f   com_math_anglevectors.obj

//__int128 _mask__NegFloat_ = 0x80000000800000008000000080000000;

struct DObjAnimMat;
struct DObjSkelMat;

void ConvertQuatToSkelMat(const DObjAnimMat *const mat, DObjSkelMat *skelMat);
void ConvertQuatToInverseSkelMat(const DObjAnimMat *const mat, DObjSkelMat *skelMat);
void QuatMultiplyInverse(const float *in1, const float *in2, float *out);
void QuatMultiplyReverseInverse(const float *in1, const float *in2, float *out);
void QuatMultiplyReverseEquals(const float *in, float *inout);
void ConvertQuatToMat(const struct DObjAnimMat *mat, float (*axis)[3]);
void ConvertQuatToInverseMat(const DObjAnimMat *mat, float (*axis)[3]);
void QuatMultiplyEquals(const float *in, float *inout);
void MatrixTransformVectorQuatTransEquals(const struct DObjAnimMat *in, float *inout);
void R_TransformSkelMat(const float *origin, const DObjSkelMat *mat, float *out);


inline float ClampFloat(float x, float minval, float maxval)
{
    iassert(minval < maxval);

    return CLAMP(x, minval, maxval);
}

/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
inline float Q_rsqrt(float number)
{
    iassert(number);
    iassert(!isnan(number));

    union standards_compliant_fp_bit_hack {
        int i;
        float f;
    };

    standards_compliant_fp_bit_hack v;
    float x2, y;
    const float threehalfs = 1.5F;

    x2 = number * 0.5F;
    y = number;
    v.f = y;						// evil floating point bit level hacking
    v.i = 0x5f3759df - (v.i >> 1);               // what the fuck?
    y = v.f;
    y = y * (threehalfs - (x2 * y * y));   // 1st iteration
    //	y  = y * ( threehalfs - ( x2 * y * y ) );   // 2nd iteration, this can be removed

    // TODO: use rsqrtss instead since it's not 1990 anymore
    return y;
}

float Q_fabs(float f);

#define I_rsqrt Q_rsqrt
#define I_fabs Q_fabs
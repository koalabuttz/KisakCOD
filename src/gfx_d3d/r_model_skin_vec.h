#pragma once

// Vertex skinning four floats at a time, for the targets that have no SSE
// path (r_model_skin_sse.cpp): Arm, where the scalar code in r_model_skin.cpp
// was the only one.
//
// It is written with the compiler's own vector types, not with one CPU's
// intrinsics, so the same source is NEON on Arm and SSE on an x86 host, where
// tests/skinning/skin_scalar_test.cpp holds it against the scalar code. Every
// sum is made in the scalar code's order, so the two write the same bytes (as
// long as no multiply-add is fused, which the build's -ffp-contract=off sees
// to). One difference is left on 32-bit Arm: NEON takes a denormal for zero.
// A skinned vertex is drawn and nothing else, so that is allowed here and
// nowhere near the movement and collision maths (docs/linux-armv7.md).
//
// The scalar code writes a vertex field by field into the skinned cache and,
// for a weighted vertex, reads the position back from it. On the PS Vita that
// cache is memory the CPU does not cache. Here a vertex is put together in
// registers and written once, as two 16-byte halves.
//
// This header and r_model_skin_vec.cpp include nothing of the engine: on the
// Vita that one file is compiled for NEON (the rest of the engine is not), and
// no shared inline function should come out of it with other code than
// everywhere else. The engine's types are checked against the two below in
// r_model_skin.cpp.

#include <cstdint>

// GCC and Clang have the vector types. On 32-bit Arm the file has to be built
// for NEON to be worth it, and only the Vita's build does that
// (scripts/platform/vita/mp_client.cmake).
#if defined(__GNUC__) && (!defined(__arm__) || defined(__vita__))
#define KISAK_SKIN_VEC 1
#else
#define KISAK_SKIN_VEC 0
#endif

#if KISAK_SKIN_VEC
namespace skin_vec
{
// GfxPackedVertex.
struct Vertex
{
    float xyz[3];
    float binormalSign;
    uint32_t color;
    uint32_t texCoord;
    uint32_t normal; // three bytes of direction, one of scale (PackedUnitVec)
    uint32_t tangent;
};

// DObjSkelMat.
struct Bone
{
    float axis[3][4];
    float origin[4];
};

// A surface whose vertices hang on up to four bones each (XSurface::deformed).
// counts are the vertices with none, one, two and three more bones than the
// first, in that order in `in`; blend holds, per vertex, the first bone's
// offset in bytes from `bones`, then an offset and a weight (of 65536) for
// each further bone.
void Weighted(const Vertex *in, const uint16_t *blend, const int16_t counts[4], const Bone *bones, Vertex *out);

// A run of vertices on one bone.
void Rigid(const Vertex *in, int count, const Bone *bone, Vertex *out);
} // namespace skin_vec
#endif

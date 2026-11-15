// See r_model_skin_vec.h. Nothing of the engine is included here.
#include "r_model_skin_vec.h"

#if KISAK_SKIN_VEC

#include <cstring>

#if defined(__arm__) && !defined(__ARM_NEON)
#error "r_model_skin_vec.cpp is built for NEON on 32-bit Arm (-mfpu=neon): without it this is slower than the scalar code"
#endif

namespace skin_vec
{
namespace
{
typedef float F4 __attribute__((vector_size(16)));
typedef int32_t I4 __attribute__((vector_size(16)));
typedef uint32_t U4 __attribute__((vector_size(16)));
typedef uint8_t B4 __attribute__((vector_size(4)));

// Vec3UnpackUnitVec's scale for each value of a packed vector's fourth byte,
// by its own expression.
struct DecodeScales
{
    float of[256];
    constexpr DecodeScales() : of()
    {
        for (int i = 0; i < 256; ++i)
            of[i] = (i - -192.0f) / 32385.0f;
    }
};
constexpr DecodeScales kDecodeScale;

struct Rows
{
    F4 axis0, axis1, axis2, origin;
};

inline __attribute__((always_inline)) F4 Load(const float *from)
{
    F4 v;
    std::memcpy(&v, from, sizeof(v));
    return v;
}

inline __attribute__((always_inline)) Rows RowsOf(const Bone *bones, uint32_t offset)
{
    const float *row = reinterpret_cast<const float *>(reinterpret_cast<const char *>(bones) + offset);
    return Rows{Load(row), Load(row + 4), Load(row + 8), Load(row + 12)};
}

inline __attribute__((always_inline)) F4 Lane(F4 v, int lane)
{
    return F4{v[lane], v[lane], v[lane], v[lane]};
}

// MatrixTransformVertexAndBasis's position: ((x a0 + y a1) + z a2) + origin.
inline __attribute__((always_inline)) F4 FirstBone(const Rows &m, F4 xyz)
{
    return ((Lane(xyz, 0) * m.axis0 + Lane(xyz, 1) * m.axis1) + Lane(xyz, 2) * m.axis2) + m.origin;
}

// R_TransformSkelMat, which sums x as x a0 + (z a2 + y a1) and y and z as
// y a1 + (x a0 + z a2).
inline __attribute__((always_inline)) F4 FurtherBone(const Rows &m, F4 xyz)
{
    const F4 x = Lane(xyz, 0) * m.axis0;
    const F4 y = Lane(xyz, 1) * m.axis1;
    const F4 z = Lane(xyz, 2) * m.axis2;
    const F4 forX = x + (z + y);
    const F4 forYz = y + (x + z);
    F4 sum = forYz;
    sum[0] = forX[0];
    return sum + m.origin;
}

// A packed normal or tangent turned by the bone and packed again.
inline __attribute__((always_inline)) uint32_t Turned(const Rows &m, const uint32_t *packed)
{
    B4 bytes;
    std::memcpy(&bytes, packed, sizeof(bytes));
    const F4 unit = (__builtin_convertvector(bytes, F4) - 127.0f) *
                    kDecodeScale.of[reinterpret_cast<const uint8_t *>(packed)[3]];
    const F4 turned = (Lane(unit, 0) * m.axis0 + Lane(unit, 1) * m.axis1) + Lane(unit, 2) * m.axis2;
    B4 out = __builtin_convertvector(__builtin_convertvector(turned * 127.0f + 127.5f, I4), B4);
    out[3] = 63;
    uint32_t result;
    std::memcpy(&result, &out, sizeof(result));
    return result;
}

// The vertex goes out whole: the position with the input's binormal sign,
// then the colour and texture coordinates as they came with the turned normal
// and tangent.
inline __attribute__((always_inline)) void Store(Vertex *out, F4 position, F4 inFirst, const Vertex *in,
                                                 const Rows &m)
{
    position[3] = inFirst[3];
    const U4 second = {in->color, in->texCoord, Turned(m, &in->normal), Turned(m, &in->tangent)};
    std::memcpy(out, &position, sizeof(position));
    std::memcpy(&out->color, &second, sizeof(second));
}

inline __attribute__((always_inline)) float Weight(uint16_t of65536)
{
    return static_cast<float>(of65536) * 0.0000152587890625f;
}
} // namespace

void Rigid(const Vertex *in, int count, const Bone *bone, Vertex *out)
{
    const Rows m = RowsOf(bone, 0);
    for (int i = 0; i < count; ++i, ++in, ++out)
    {
        const F4 xyz = Load(in->xyz);
        Store(out, FirstBone(m, xyz), xyz, in, m);
    }
}

void Weighted(const Vertex *in, const uint16_t *blend, const int16_t counts[4], const Bone *bones, Vertex *out)
{
    for (int i = 0; i < counts[0]; ++i, ++in, ++out, blend += 1)
    {
        const Rows m = RowsOf(bones, blend[0]);
        const F4 xyz = Load(in->xyz);
        Store(out, FirstBone(m, xyz), xyz, in, m);
    }
    // With further bones the first bone's position counts for what the other
    // weights leave: offset + (1 - weights) * position, the offset summed in
    // the scalar code's order.
    for (int i = 0; i < counts[1]; ++i, ++in, ++out, blend += 3)
    {
        const Rows m = RowsOf(bones, blend[0]);
        const F4 xyz = Load(in->xyz);
        const float weight = Weight(blend[2]);
        const F4 offset = weight * FurtherBone(RowsOf(bones, blend[1]), xyz);
        Store(out, offset + (1.0f - weight) * FirstBone(m, xyz), xyz, in, m);
    }
    for (int i = 0; i < counts[2]; ++i, ++in, ++out, blend += 5)
    {
        const Rows m = RowsOf(bones, blend[0]);
        const F4 xyz = Load(in->xyz);
        float total = Weight(blend[2]);
        F4 offset = total * FurtherBone(RowsOf(bones, blend[1]), xyz);
        float weight = Weight(blend[4]);
        total = total + weight;
        offset = offset + weight * FurtherBone(RowsOf(bones, blend[3]), xyz);
        Store(out, offset + (1.0f - total) * FirstBone(m, xyz), xyz, in, m);
    }
    for (int i = 0; i < counts[3]; ++i, ++in, ++out, blend += 7)
    {
        const Rows m = RowsOf(bones, blend[0]);
        const F4 xyz = Load(in->xyz);
        float total = Weight(blend[2]);
        F4 offset = total * FurtherBone(RowsOf(bones, blend[1]), xyz);
        float weight = Weight(blend[4]);
        total = total + weight;
        offset = offset + weight * FurtherBone(RowsOf(bones, blend[3]), xyz);
        weight = Weight(blend[6]);
        total = total + weight;
        offset = offset + weight * FurtherBone(RowsOf(bones, blend[5]), xyz);
        Store(out, offset + (1.0f - total) * FirstBone(m, xyz), xyz, in, m);
    }
}
} // namespace skin_vec

#endif // KISAK_SKIN_VEC

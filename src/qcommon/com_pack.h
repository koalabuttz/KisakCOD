#pragma once

#include <universal/com_math.h>
#include <cstdint>

struct PackedTexCoords // sizeof=0x4
{                                       // ...
    PackedTexCoords()
    {
        packed = 0;
    }
    PackedTexCoords(uint32_t i)
    {
        packed = i;
    }
    uint32_t packed;
};

void Vec2UnpackTexCoords(PackedTexCoords in, float *out);
void Vec3UnpackUnitVec(PackedUnitVec in, float *out);
PackedUnitVec Vec3PackUnitVec(const float *unitVec);
PackedTexCoords Vec2PackTexCoords(const float *in);
void Byte4PackVertexColor(const float *from, uint8_t *to);
void Byte4PackRgba(const float *from, uint8_t *to);
void Byte4UnpackRgba(const uint8_t *from, float *to);
void Byte4CopyRgbaToVertexColor(const uint8_t *rgbaFrom, uint8_t *nativeTo);
void Byte4CopyBgraToVertexColor(const uint8_t *rgbaFrom, uint8_t *nativeTo);
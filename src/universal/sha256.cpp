#include "sha256.h"

#include <cstring>

namespace {
const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

uint32_t Rotr(uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

void Transform(Sha256 *ctx, const unsigned char *data)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
    {
        w[i] = (uint32_t(data[i * 4]) << 24) | (uint32_t(data[i * 4 + 1]) << 16)
            | (uint32_t(data[i * 4 + 2]) << 8) | uint32_t(data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i)
    {
        const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; ++i)
    {
        const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + k[i] + w[i];
        const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}
}

void Sha256_Init(Sha256 *ctx)
{
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    std::memcpy(ctx->state, initial, sizeof(initial));
    ctx->length = 0;
    ctx->used = 0;
}

void Sha256_Update(Sha256 *ctx, const void *data, size_t size)
{
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    ctx->length += uint64_t(size) * 8;
    while (size)
    {
        size_t take = sizeof(ctx->block) - ctx->used;
        if (take > size)
            take = size;
        std::memcpy(ctx->block + ctx->used, bytes, take);
        ctx->used += take;
        bytes += take;
        size -= take;
        if (ctx->used == sizeof(ctx->block))
        {
            Transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

void Sha256_Final(Sha256 *ctx, unsigned char digest[32])
{
    const uint64_t length = ctx->length;
    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56)
    {
        std::memset(ctx->block + ctx->used, 0, sizeof(ctx->block) - ctx->used);
        Transform(ctx, ctx->block);
        ctx->used = 0;
    }
    std::memset(ctx->block + ctx->used, 0, 56 - ctx->used);
    for (int i = 0; i < 8; ++i)
        ctx->block[56 + i] = static_cast<unsigned char>(length >> (56 - i * 8));
    Transform(ctx, ctx->block);
    for (int i = 0; i < 8; ++i)
    {
        digest[i * 4] = static_cast<unsigned char>(ctx->state[i] >> 24);
        digest[i * 4 + 1] = static_cast<unsigned char>(ctx->state[i] >> 16);
        digest[i * 4 + 2] = static_cast<unsigned char>(ctx->state[i] >> 8);
        digest[i * 4 + 3] = static_cast<unsigned char>(ctx->state[i]);
    }
    std::memset(ctx, 0, sizeof(*ctx));
}

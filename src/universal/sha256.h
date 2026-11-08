#pragma once

#include <cstddef>
#include <cstdint>

// Plain SHA-256 (FIPS 180-4). Used to derive identifiers, not for passwords.
struct Sha256
{
    uint32_t state[8];
    uint64_t length;
    unsigned char block[64];
    size_t used;
};

void Sha256_Init(Sha256 *ctx);
void Sha256_Update(Sha256 *ctx, const void *data, size_t size);
void Sha256_Final(Sha256 *ctx, unsigned char digest[32]);

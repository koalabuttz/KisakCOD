// TEST-ONLY stand-in for <universal/q_shared.h> (see tests/linux_net/README.md).
// Mirrors the engine layouts src/linux/net.cpp depends on so the POSIX network
// layer can be built and exercised without the Windows-only engine headers.
// This file is not part of any build target.
#pragma once

#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef uint8_t byte;
typedef uint16_t word;
typedef uint32_t uint32;

typedef enum { qfalse, qtrue } qboolean;

#define MAX_STRING_CHARS 1024

// Values copied from src/universal/q_shared.h so a flag the code under test
// passes can be told apart from a zero-initialised one.  Only the flags the
// POSIX network layer uses are mirrored.
enum DvarFlags : uint16_t
{
    DVAR_NOFLAG  = 0x0,
    DVAR_ARCHIVE = 0x1,
    DVAR_INIT    = 0x10,    // startup only: never settable from the console afterwards
    DVAR_LATCH   = 0x20,    // takes effect on the next dvar read, not on set
};

struct DvarValue
{
    bool enabled;
    int integer;
    uint32_t unsignedInt;
    float value;
    float vector[4];
    const char *string;
    byte color[4];
};

struct DvarLimits_Integer { int min; int max; };
struct DvarLimits_Value { float min; float max; };

union DvarLimits
{
    DvarLimits() { integer.min = INT32_MIN; integer.max = INT32_MAX; }
    explicit DvarLimits(uint64_t val) { integer.max = (int)(val >> 32); integer.min = (int)(val & 0xFFFFFFFFu); }
    DvarLimits(int min, int max) { integer.min = min; integer.max = max; }
    DvarLimits_Integer integer;
    DvarLimits_Value value;
};

struct dvar_s
{
    const char *name;
    const char *description;
    word flags;
    byte type;
    bool modified;
    DvarValue current;
    DvarValue latched;
    DvarValue reset;
    DvarLimits domain;
};

using dvar_t = dvar_s;

int I_stricmp(const char *s0, const char *s1);
void I_strncpyz(char *dest, const char *src, int destsize);

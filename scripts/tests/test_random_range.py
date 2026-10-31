#!/usr/bin/env python3
"""Check that the Linux random helpers stay in their documented ranges.

Both random families in the Linux build scale a CRT ``rand()`` that returns a
full 31-bit value, while every consumer assumes a 15-bit one:

  * ``src/universal/com_math.cpp``'s ``random()``/``crandom()``, which
    ``game/g_items.cpp``'s ``Drop_Item`` uses to jitter a dropped item's launch
    velocity;
  * ``src/game_mp/g_utils_mp.cpp``'s ``G_rand()`` family, which backs the script
    ``randomint``/``randomfloat``/``randomintrange``/``randomfloatrange``
    builtins.

``RAND_MAX`` is 2147483647 under glibc but 32767 under the MSVCRT the original
Windows build used, so ``rand()/32768.0`` is only a unit-interval generator on
Windows. On Linux the same expression produced values near 100000, and
``Drop_Item`` multiplied that by ``g_dropHorzSpeedRand`` (100) into velocities in
the millions -- the ``ET_ITEM`` ``trDelta`` seen in game.

The production function bodies are extracted from the real sources and compiled
into a small native host program. Only the CRT ``rand()`` call itself is
redirected to a controlled double, so the values under test are the shipped
arithmetic. Everything is asset-free: no game data, no server, no Windows
runtime. ``--com-math``/``--g-utils``/``--g-items`` accept alternative files so a
pre-fix body can be checked to still fail.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def block(source: str, start: int) -> str:
    """Return a brace-delimited declaration/function, ignoring quoted braces."""
    opening = source.index("{", start)
    depth = 1
    tokens = re.compile(
        r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]''',
        re.DOTALL,
    )
    for token in tokens.finditer(source, opening + 1):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise ValueError("Unterminated production declaration")


CALL = re.compile(r"(?<![A-Za-z0-9_])rand\(\)")

# Comments and literals are skipped: prose may legitimately mention rand(), and a
# string literal is not the CRT call.
NON_CODE = re.compile(
    r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*' ''',
    re.DOTALL,
)


def redirect_rand(body: str) -> tuple[str, int]:
    """Point the CRT rand() call at the controlled double, in code only."""
    pieces, count, cursor = [], 0, 0
    for match in NON_CODE.finditer(body):
        code, found = CALL.subn("kisak_rand()", body[cursor:match.start()])
        count += found
        pieces += [code, match.group()]
        cursor = match.end()
    code, found = CALL.subn("kisak_rand()", body[cursor:])
    count += found
    pieces.append(code)
    return "".join(pieces), count


def function(source: str, signature: str, draws: int = 1) -> str:
    """Extract one production definition and redirect its CRT rand() call.

    ``rand()`` becomes ``kisak_rand()``, the controlled double that returns the
    value the test selected. It is the only edit applied to the shipped text:
    the masking, scaling and ordering arithmetic stays byte-for-byte as compiled.
    ``draws`` is how many CRT calls that body is expected to contain -- one for the
    two base generators, none for the helpers that compose them.
    """
    body, count = redirect_rand(block(source, source.index(signature)))
    if count != draws:
        raise ValueError(f"{signature} calls rand() {count} times, expected {draws}")
    return body + "\n"


HARNESS = r"""
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

// Only the platform spelling changes for the host compiler. <cstdlib> is
// already included above (if at all) under its own prototype, so the production
// Linux name mapping can be applied here exactly as com_math.h applies it.
#ifndef __cdecl
#define __cdecl
#endif
#define random Q_random

// Declarations only; the bodies below are the production definitions.
float random();
float crandom();
int G_rand();
float G_flrand(float min, float max);
int G_irand(int min, int max);
float G_random();
float G_crandom();

// ---------------------------------------------------------------- test doubles
// The single CRT dependency: every helper under test reaches randomness through
// rand(), so one controlled value drives all of them.
static int g_randValue;
static int g_randCalls;

int kisak_rand()
{
    ++g_randCalls;
    return g_randValue;
}

static void selectRand(int value)
{
    g_randValue = value;
    g_randCalls = 0;
}

// ---------------------------------------------------------------- bookkeeping
static int g_checks;
static int g_failures;
static const char *g_group = "";

static void expect(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL [%s] %s\n", g_group, what);
    }
}

static void expectInt(long long got, long long want, const char *what)
{
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("FAIL [%s] %s: got %lld want %lld\n", g_group, what, got, want);
    }
}

static void expectFloat(float got, float want, const char *what)
{
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("FAIL [%s] %s: got %.9g want %.9g\n", g_group, what,
                    static_cast<double>(got), static_cast<double>(want));
    }
}

static void expectRange(float got, float low, float high, const char *what)
{
    ++g_checks;
    if (!(got >= low && got <= high)) {
        ++g_failures;
        std::printf("FAIL [%s] %s: got %.9g outside [%.9g, %.9g]\n", g_group, what,
                    static_cast<double>(got), static_cast<double>(low),
                    static_cast<double>(high));
    }
}

// ------------------------------------------------------- values the CRT returns
// MSVCRT only ever returned 0..32767; glibc returns the whole 0..RAND_MAX range.
// The sweep covers every boundary the difference introduces plus a deterministic
// wide sample.
static const std::vector<int> &randSweep()
{
    static std::vector<int> values;
    if (!values.empty()) return values;

    const int fixed[] = {
        0, 1, 2, 3, 7, 15, 31, 63, 255, 1023, 2047, 4095, 8191, 16383,
        32766, 32767, 32768, 32769, 49151, 65534, 65535, 65536, 65537, 99999,
        100000, 123456789, 1000000007, 1073741823, 1073741824, 2147483000,
        2147483646, 2147483647,
    };
    for (int value : fixed) values.push_back(value);
    for (int value = 0; value <= 1024; ++value) values.push_back(value);
    uint32_t state = 0x9e3779b9u;
    for (int index = 0; index < 512; ++index) {
        state = state * 1664525u + 1013904223u;
        values.push_back(static_cast<int>(state >> 1));
    }
    return values;
}

// -------------------------------------------------------------- Drop_Item doubles
// Drop_Item's only engine dependencies are the dvar table, angle/vector maths and
// LaunchItem. These are stand-ins for those, not for the code under test.
struct gitem_s {
    int placeholder;
};

struct entityStateLike {
    int number;
};

struct gentity_s {
    struct {
        float currentAngles[3];
        float currentOrigin[3];
        float maxs[3];
        float mins[3];
    } r;
    entityStateLike s;
};

struct dvarValueLike {
    struct {
        float value;
    };
};

struct dvar_s {
    dvarValueLike current;
};

static const float DEG2RAD = 0.017453292519943295f;

void AngleVectors(const float *angles, float *forward, float *right, float *up)
{
    const float pitch = angles[0] * DEG2RAD;
    const float yaw = angles[1] * DEG2RAD;
    const float roll = angles[2] * DEG2RAD;
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float sr = std::sin(roll), cr = std::cos(roll);
    if (forward) {
        forward[0] = cp * cy;
        forward[1] = cp * sy;
        forward[2] = -sp;
    }
    if (right) {
        right[0] = -sr * sp * cy + cr * sy;
        right[1] = -sr * sp * sy - cr * cy;
        right[2] = -sr * cp;
    }
    if (up) {
        up[0] = cr * sp * cy + sr * sy;
        up[1] = cr * sp * sy - sr * cy;
        up[2] = cr * cp;
    }
}

void Vec3Scale(const float *in, float scale, float *out)
{
    out[0] = in[0] * scale;
    out[1] = in[1] * scale;
    out[2] = in[2] * scale;
}

static const gitem_s *g_launchItem;
static float g_launchOrigin[3];
static float g_launchAngles[3];
static float g_launchVelocity[3];
static int g_launchOwner;
static int g_launchCalls;
static gentity_s g_launchResult;

gentity_s *LaunchItem(const gitem_s *item, float *origin, float *angles, float *velocity,
                      int ownerNum)
{
    g_launchItem = item;
    for (int axis = 0; axis < 3; ++axis) {
        g_launchOrigin[axis] = origin[axis];
        g_launchAngles[axis] = angles[axis];
        g_launchVelocity[axis] = velocity[axis];
    }
    g_launchOwner = ownerNum;
    ++g_launchCalls;
    return &g_launchResult;
}

// The values G_InitGame registers for the dropped-item speeds.
static dvar_s g_dropForwardSpeedDvar{{10.0f}};
static dvar_s g_dropUpSpeedBaseDvar{{10.0f}};
static dvar_s g_dropUpSpeedRandDvar{{5.0f}};
static dvar_s g_dropHorzSpeedRandDvar{{100.0f}};

static const dvar_s *g_dropForwardSpeed = &g_dropForwardSpeedDvar;
static const dvar_s *g_dropUpSpeedBase = &g_dropUpSpeedBaseDvar;
static const dvar_s *g_dropUpSpeedRand = &g_dropUpSpeedRandDvar;
static const dvar_s *g_dropHorzSpeedRand = &g_dropHorzSpeedRandDvar;

// The one production path under test for Drop_Item.
#define KISAK_MP 1
"""

ASSERTIONS = r"""
// =========================================================== com_math.cpp random
static const float TWO_POW_15 = 32768.0f;

static void checkUniversalRandom()
{
    g_group = "com_math random";

    float lowest = 2.0f, highest = -1.0f;
    for (int value : randSweep()) {
        selectRand(value);
        const float got = random();
        expectInt(g_randCalls, 1, "random() draws exactly one rand()");
        expectRange(got, 0.0f, 1.0f, "random() stays in [0,1)");
        expect(got < 1.0f, "random() is strictly below 1");
        expectFloat(got, static_cast<float>(value & 0x7fff) / TWO_POW_15,
                    "random() is (rand() & 0x7fff) / 32768");
        if (got < lowest) lowest = got;
        if (got > highest) highest = got;
    }
    expectFloat(lowest, 0.0f, "random() reaches 0");
    expectFloat(highest, 32767.0f / TWO_POW_15, "random() peaks just below 1");
}

static void checkUniversalCrandom()
{
    g_group = "com_math crandom";

    bool sawNegative = false, sawPositive = false;
    float lowest = 2.0f, highest = -2.0f;
    for (int value : randSweep()) {
        selectRand(value);
        const float got = crandom();
        expectInt(g_randCalls, 1, "crandom() draws exactly one rand()");
        expectRange(got, -1.0f, 1.0f, "crandom() stays in [-1,1]");
        const float expected = static_cast<float>(
            2.0 * (static_cast<double>(static_cast<float>(value & 0x7fff) / TWO_POW_15)) - 1.0);
        expectFloat(got, expected, "crandom() is 2*random() - 1");
        if (got < 0.0f) sawNegative = true;
        if (got > 0.0f) sawPositive = true;
        if (got < lowest) lowest = got;
        if (got > highest) highest = got;
    }
    expect(sawNegative, "crandom() produces negative jitter");
    expect(sawPositive, "crandom() produces positive jitter");
    expectFloat(lowest, -1.0f, "crandom() reaches -1");
    expect(highest > 0.99f, "crandom() reaches the positive end of its range");
}

// The mask must be a no-op for every value the Windows CRT could return, so the
// Windows build's mapping is untouched by the Linux range fix.
static void checkWindowsFifteenBitParity()
{
    g_group = "windows 15-bit parity";

    for (int value = 0; value <= 32767; ++value) {
        selectRand(value);
        const float masked = random();
        const float windows = static_cast<float>(value) / TWO_POW_15;
        expectFloat(masked, windows, "masked mapping equals the Windows rand()/32768");
        if (masked != windows) break;
    }

    for (int value = 0; value <= 32767; ++value) {
        selectRand(value);
        const float masked = crandom();
        const float windows = static_cast<float>(2.0 * (value / 32768.0) - 1.0);
        expectFloat(masked, windows, "crandom() equals the Windows mapping");
        if (masked != windows) break;
    }
}

// ==================================================== game_mp G_rand G_* helpers
static void checkMpRandomFamily()
{
    g_group = "game_mp G_rand family";

    for (int value : randSweep()) {
        selectRand(value);
        const int got = G_rand();
        expectInt(g_randCalls, 1, "G_rand() draws exactly one rand()");
        expectInt(got, value & 0x7fff, "G_rand() is rand() & 0x7fff");
        expectRange(static_cast<float>(got), 0.0f, 32767.0f, "G_rand() stays 15-bit");

        selectRand(value);
        const float real = G_random();
        expectInt(g_randCalls, 1, "G_random() draws exactly one rand()");
        expectRange(real, 0.0f, 1.0f, "G_random() stays in [0,1)");
        expectFloat(real, static_cast<float>(value & 0x7fff) / TWO_POW_15,
                    "G_random() is G_rand() / 32768");

        selectRand(value);
        const float centred = G_crandom();
        expectInt(g_randCalls, 1, "G_crandom() draws exactly one rand()");
        expectRange(centred, -1.0f, 1.0f, "G_crandom() stays in [-1,1]");
        expectFloat(centred, static_cast<float>(
                        2.0 * (static_cast<double>(static_cast<float>(value & 0x7fff) / TWO_POW_15))
                        - 1.0),
                    "G_crandom() is 2*G_random() - 1");
    }
}

static void checkMpScaledHelpers()
{
    g_group = "game_mp flrand/irand";

    const std::pair<float, float> floatRanges[] = {
        {0.0f, 1.0f}, {0.0f, 100.0f}, {0.0f, 0.5f}, {-1.0f, 1.0f},
        {-50.0f, -10.0f}, {10.0f, 10.5f}, {0.0f, 32768.0f}, {-1000.0f, 1000.0f},
    };
    for (const auto &range : floatRanges) {
        for (int value : randSweep()) {
            selectRand(value);
            const float got = G_flrand(range.first, range.second);
            expectInt(g_randCalls, 1, "G_flrand() draws exactly one rand()");
            expectRange(got, range.first, range.second, "G_flrand() stays inside its range");
            expectFloat(got, static_cast<float>(value & 0x7fff)
                                 * (range.second - range.first) / TWO_POW_15 + range.first,
                        "G_flrand() is G_rand() scaled into the range");
        }
    }

    // Modest, non-empty intervals only. G_irand() multiplies (max - min) by the
    // 15-bit draw in 32-bit arithmetic, so spans near INT_MAX overflow the
    // intermediate; that is a separate, documented limit and not covered here.
    const std::pair<int, int> intRanges[] = {
        {0, 1}, {0, 2}, {0, 7}, {0, 100}, {0, 1000}, {0, 32768}, {5, 6},
        {-5, 5}, {100, 200}, {-32768, 32768},
    };
    for (const auto &range : intRanges) {
        for (int value : randSweep()) {
            selectRand(value);
            const int got = G_irand(range.first, range.second);
            expectInt(g_randCalls, 1, "G_irand() draws exactly one rand()");
            expect(got >= range.first, "G_irand() does not fall below its minimum");
            expect(got < range.second, "G_irand() does not reach its maximum");
            expectInt(got, range.first + (range.second - range.first) * (value & 0x7fff) / 0x8000,
                      "G_irand() is G_rand() scaled into the range");
        }
    }

    // The wrapper builtins pass these two argument shapes; the whole 15-bit
    // domain must be reachable or a wide requested range would lose values.
    int lowest = 0x7fffffff, highest = -0x7fffffff - 1;
    float floatLowest = 2.0f, floatHighest = -1.0f;
    for (int value : randSweep()) {
        selectRand(value);
        const int drawn = G_irand(0, 32768);
        expectRange(static_cast<float>(drawn), 0.0f, 32767.0f, "int draw stays below its bound");
        if (drawn < lowest) lowest = drawn;
        if (drawn > highest) highest = drawn;

        selectRand(value);
        const float real = G_flrand(0.0f, 1.0f);
        expectRange(real, 0.0f, 1.0f, "float draw stays below its bound");
        if (real < floatLowest) floatLowest = real;
        if (real > floatHighest) floatHighest = real;
    }
    expectInt(lowest, 0, "the integer draw reaches its minimum");
    expectInt(highest, 32767, "the integer draw reaches its maximum");
    expectFloat(floatLowest, 0.0f, "the float draw reaches 0");
    expectFloat(floatHighest, 32767.0f / TWO_POW_15, "the float draw peaks just below 1");
}

// ============================================== g_items.cpp Drop_Item launch speed
static void dropItemScene(gentity_s *ent)
{
    ent->r.currentAngles[0] = 0.0f;
    ent->r.currentAngles[1] = 35.0f;
    ent->r.currentAngles[2] = 0.0f;
    ent->r.currentOrigin[0] = 100.0f;
    ent->r.currentOrigin[1] = -200.0f;
    ent->r.currentOrigin[2] = 50.0f;
    ent->r.maxs[2] = 32.0f;
    ent->r.mins[2] = -8.0f;
    ent->s.number = 3;
}

static void checkDropItemVelocity()
{
    g_group = "Drop_Item velocity";

    const gitem_s item{0};
    const float forward = 10.0f, upBase = 10.0f, upRand = 5.0f, horz = 100.0f;
    const float angleParam = 12.5f;

    float worst = 0.0f;
    bool sawMotion = false;

    for (int value : randSweep()) {
        gentity_s ent{};
        dropItemScene(&ent);
        const int launches = g_launchCalls;

        selectRand(value);
        const gentity_s *launched = Drop_Item(&ent, &item, angleParam, 0);

        expectInt(g_launchCalls, launches + 1, "Drop_Item launches the item once");
        expect(launched == &g_launchResult, "Drop_Item returns what LaunchItem built");
        expect(g_launchItem == &item, "Drop_Item launches the item it was given");
        expectInt(g_randCalls, 3, "Drop_Item draws exactly three crandom() values");

        const float vx = g_launchVelocity[0];
        const float vy = g_launchVelocity[1];
        const float vz = g_launchVelocity[2];
        expect(std::isfinite(vx) && std::isfinite(vy) && std::isfinite(vz),
               "dropped item velocity is finite");

        // horizontal speed is the yaw-scaled forward speed plus one jitter term
        expectRange(std::fabs(vx), 0.0f, forward + horz,
                    "dropped item X velocity stays inside the configured range");
        expectRange(std::fabs(vy), 0.0f, forward + horz,
                    "dropped item Y velocity stays inside the configured range");
        // pitch and roll are zeroed, so the forward vector contributes nothing to Z
        expectRange(vz, upBase - upRand, upBase + upRand,
                    "dropped item Z velocity stays inside the configured range");

        const float magnitude = std::fabs(vx) + std::fabs(vy) + std::fabs(vz);
        if (magnitude > worst) worst = magnitude;
        if (magnitude > 0.0f) sawMotion = true;
    }

    expect(sawMotion, "dropped item actually receives launch jitter");
    // The field failure multiplied the raw 31-bit scale into velocities in the
    // millions; the configured speeds cap the total far below this.
    expect(worst < 1000.0f, "dropped item speed never reaches the millions-scale failure");
}

static void checkDropItemKinematics()
{
    g_group = "Drop_Item kinematics";

    const gitem_s item{0};
    const float angleParam = 12.5f;
    const float dropHeight = (32.0f - -8.0f) * 0.5f;

    for (int novelocity : {0, 1}) {
        for (int value : randSweep()) {
            gentity_s ent{};
            dropItemScene(&ent);
            const int launches = g_launchCalls;

            selectRand(value);
            Drop_Item(&ent, &item, angleParam, novelocity);

            expectInt(g_launchCalls, launches + 1, "Drop_Item launches the item once");

            // origin: the entity's feet-centred position, unchanged by novelty
            expectFloat(g_launchOrigin[0], 100.0f, "drop origin X is the ent origin");
            expectFloat(g_launchOrigin[1], -200.0f, "drop origin Y is the ent origin");
            expectFloat(g_launchOrigin[2], 50.0f + dropHeight,
                        "drop origin Z is raised by half the entity height");

            // angles: level, yawed by the caller's angle, no roll
            expectFloat(g_launchAngles[0], 0.0f, "drop pitch is cleared");
            expectFloat(g_launchAngles[1], 35.0f + angleParam, "drop yaw is offset by the angle");
            expectFloat(g_launchAngles[2], 0.0f, "drop roll is cleared");

            expectInt(g_launchOwner, 3, "drop owner is the entity number");

            if (novelocity) {
                expectFloat(g_launchVelocity[0], 0.0f, "novelocity zeroes X");
                expectFloat(g_launchVelocity[1], 0.0f, "novelocity zeroes Y");
                expectFloat(g_launchVelocity[2], 0.0f, "novelocity zeroes Z");
                expectInt(g_randCalls, 0, "novelocity draws no random values");
            } else {
                expect(g_randCalls == 3, "a normal drop draws three random values");
            }
            if (novelocity && g_randCalls != 0) break;
        }
    }
}

// ============================================ g_combat_mp grenade launch velocity
// Both the held-grenade and the death-perk path draw three G_crandom() values and
// scale them by 160 before G_FireGrenade(). The combat function itself is not
// extracted -- it needs a client, weapon defs and the item spawner -- so this
// reproduces exactly the three draws and the scale that shape the velocity.
static void checkGrenadeLaunchVelocity()
{
    g_group = "grenade launch velocity";

    const float scale = 160.0f;
    float worst = 0.0f;

    for (int value : randSweep()) {
        float launchvel[3];
        for (int axis = 0; axis < 3; ++axis) {
            selectRand(value);
            launchvel[axis] = G_crandom();
            expectInt(g_randCalls, 1, "a grenade component draws exactly one rand()");
        }
        Vec3Scale(launchvel, scale, launchvel);

        for (int axis = 0; axis < 3; ++axis) {
            expectRange(launchvel[axis], -scale, scale,
                        "grenade component stays inside the +/-160 scale");
            expect(launchvel[axis] < scale, "grenade component is strictly below the scale");
            expectFloat(launchvel[axis],
                        scale * static_cast<float>(
                            2.0 * (static_cast<double>(
                                       static_cast<float>(value & 0x7fff) / TWO_POW_15)) - 1.0),
                        "grenade component is 160 * G_crandom()");
            const float magnitude = std::fabs(launchvel[axis]);
            if (magnitude > worst) worst = magnitude;
        }
    }

    expect(worst < 1000.0f, "grenade launch speed never reaches the millions-scale failure");
}

int main()
{
    checkUniversalRandom();
    checkUniversalCrandom();
    checkWindowsFifteenBitParity();
    checkMpRandomFamily();
    checkMpScaledHelpers();
    checkDropItemVelocity();
    checkDropItemKinematics();
    checkGrenadeLaunchVelocity();

    std::printf("%d runtime checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
"""


def crandom_draws(source: str, signature: str, stop: str) -> int:
    """Count crandom() draws between a signature and the branch that ends it."""
    start = source.index(signature)
    return len(re.findall(r"(?<![A-Za-z0-9_])crandom\(\)",
                          source[start:source.index(stop, start)]))


def source_checks(header, com_math, g_utils, g_items):
    """Shape assertions that the extracted code is still the code that ships."""
    checks = [
        ("com_math.h maps random to Q_random on Linux", "#define random Q_random" in header),
        ("com_math.cpp defines the random() that Linux alias names",
         "float __cdecl random()" in com_math),
        # The MP branch of Drop_Item is the Linux dedicated-server path; it draws
        # one jitter per velocity axis.
        ("Drop_Item's MP branch draws three crandom() values",
         crandom_draws(g_items, "gentity_s *__cdecl Drop_Item", "#elif") == 3),
    ]
    passed = sum(1 for _, ok in checks if ok)
    for name, ok in checks:
        if not ok:
            print(f"FAIL [source] {name}")
    print(f"{passed}/{len(checks)} source checks passed")
    return 0 if passed == len(checks) else 1


def build_program(header, com_math, g_utils, g_items):
    return "".join(
        [
            HARNESS,
            function(com_math, "float __cdecl random()"),
            function(com_math, "float __cdecl crandom()", draws=0),
            function(g_utils, "int __cdecl G_rand()"),
            function(g_utils, "float __cdecl G_flrand(float min, float max)", draws=0),
            function(g_utils, "int __cdecl G_irand(int min, int max)", draws=0),
            function(g_utils, "float __cdecl G_random()", draws=0),
            function(g_utils, "float __cdecl G_crandom()", draws=0),
            function(g_items, "gentity_s *__cdecl Drop_Item(gentity_s *ent, const gitem_s *item, "
                              "float angle, int32_t novelocity)", draws=0),
            ASSERTIONS,
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("--header", type=Path, default=root / "src/universal/com_math.h")
    parser.add_argument("--com-math", type=Path, default=root / "src/universal/com_math.cpp")
    parser.add_argument("--g-utils", type=Path, default=root / "src/game_mp/g_utils_mp.cpp")
    parser.add_argument("--g-items", type=Path, default=root / "src/game/g_items.cpp")
    parser.add_argument("--arch", choices=("32", "64", "both"), default="both",
                        help="production word size is 32; 64 is checked for the arithmetic itself")
    parser.add_argument("--sanitize", action="store_true",
                        help="also run the 64-bit build under ASan/UBSan")
    args = parser.parse_args()

    header, com_math, g_utils, g_items = (
        args.header.read_text(), args.com_math.read_text(),
        args.g_utils.read_text(), args.g_items.read_text(),
    )

    status = source_checks(header, com_math, g_utils, g_items)
    program = build_program(header, com_math, g_utils, g_items)

    variants = []
    if args.arch in ("32", "both"):
        variants.append(("32", []))
    if args.arch in ("64", "both"):
        variants.append(("64", []))
        if args.sanitize:
            variants.append(("64-asan", ["-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all"]))

    with tempfile.TemporaryDirectory(prefix="kisakcod-random-range-") as temp:
        cpp = Path(temp) / "random_range.cpp"
        cpp.write_text(program)
        for arch, extra in variants:
            executable = Path(temp) / f"random_range_{arch}"
            target = arch.split("-")[0]
            # -ffp-contract=off and SSE float maths keep single-precision results
            # identical between the 32-bit and 64-bit builds.
            flags = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unused-parameter", "-ffp-contract=off"]
            if target == "32":
                flags += ["-m32", "-msse2", "-mfpmath=sse"]
            command = shlex.split(args.cxx) + flags + extra
            print(f"building {arch}-bit check:", " ".join(shlex.quote(p) for p in command))
            subprocess.run(command + [str(cpp), "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)])
            if result.returncode != 0:
                status = 1
    return status


if __name__ == "__main__":
    raise SystemExit(main())

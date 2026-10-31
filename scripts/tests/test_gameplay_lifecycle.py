#!/usr/bin/env python3
"""Exercise the production death-grenade drop and item launch/trajectory paths.

Two shipped bodies are extracted unchanged from the real sources and compiled
into a small native host program:

  * ``DeathGrenadeDrop`` (``src/game_mp/g_combat_mp.cpp``), which throws the
    grenade a player was still holding and, unless the death was a suicide,
    throws one more for the grenade-death perk;
  * ``LaunchItem``/``G_RunItem`` (``src/game/g_items.cpp``), which spawn a
    dropped item and then step it once per frame through its trajectory,
    collision response and ground settling.

``test_random_range.py`` already bounds the *shape* of a grenade launch
velocity, but it reproduces the three draws by hand; it never enters
``DeathGrenadeDrop``, so weapon selection, the perk gate and the fuse plumbing
were unmeasured. These tests extract the real bodies instead. Where they reuse
the shipped ``G_crandom()`` the draw counts, the draw-to-axis order and the
resulting component bounds are asserted against the production arithmetic.

Everything is asset-free: no game data, no server, no Windows runtime. The
doubles stand in only for engine services the bodies call into (spawning,
tracing, dvars, weapon defs), never for the code under test. ``--g-combat`` and
``--g-items`` accept alternative files so a pre-fix body can be checked to still
fail.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def extract(source: str, name: str, signature: str) -> str:
    """Return the production definition for *name*, including its body.

    Braces inside comments and string/character literals are ignored, so prose
    that mentions ``{`` cannot unbalance the scan.
    """
    start = source.index(signature)
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
                # Only the platform calling-convention spelling changes for the
                # host compiler; the body stays byte-for-byte as shipped.
                return source[start : token.end()].replace("__cdecl ", "") + "\n"
    raise ValueError(f"Unterminated production function {name}")


CALL = re.compile(r"(?<![A-Za-z0-9_])rand\(\)")

# Comments and literals are skipped: a comment may legitimately mention rand(),
# and a string literal is not the CRT call.
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


def function(source: str, name: str, signature: str, draws: int) -> str:
    """Extract one production definition and redirect its CRT rand() call."""
    body, count = redirect_rand(extract(source, name, signature))
    if count != draws:
        raise ValueError(f"{name} calls rand() {count} times, expected {draws}")
    return body + "\n"


HARNESS = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef NDEBUG
#error Assertions must remain enabled
#endif

#ifndef __cdecl
#define __cdecl
#endif

// The production body reads float bits to detect NaN; memcpy keeps that
// type-punning off the aliasing rules the host compiler optimises under.
static inline uint32_t kisak_bits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

#define LODWORD(x) kisak_bits(x)
#define IS_NAN(x) std::isnan(x)
#define iassert(x) assert(x)

// The shipped trajectory integrator calls sin() unqualified.
using std::sin;

// The Linux dedicated server is the multiplayer build; LaunchItem's MP branch is
// the one that ships here, and it is the branch these tests exercise.
#define KISAK_MP 1

// Production definitions are appended verbatim below, after the doubles they
// need; nothing here forward-declares them, so every call site sees the real
// signature.

// ------------------------------------------------------------------ constants
// Transcribed from the shipping headers; source_checks() re-reads them and fails
// if any transcription here drifts. The handler and flag blocks are the KISAK_MP
// ones (bg_public.h guards the first block with #ifdef KISAK_MP).
constexpr int MAX_GENTITIES = 1024;
constexpr int ENTITYNUM_NONE = 1023;
constexpr int ET_ITEM = 3;                  // q_shared/bg_public entityType name table
constexpr int MOD_SUICIDE = 0xC;            // game/g_local.h
constexpr int MOD_HEADSHOT = 3;
constexpr int CON_CHANNEL_CLIENT = 4;
constexpr int ANIM_COND_PERK = 0x9;         // bg_public.h
constexpr int ANIM_PERK_GRENADEDEATH = 0x2; // bg_public.h
constexpr int CONTENTS_ITEM = 0x00000100;   // q_shared.h
constexpr int CONTENTS_ANY_TRIGGER = 0x405C0008; // q_shared.h (static_assert there)
constexpr int CONTENTS_USE = 0x00200000;    // q_shared.h
constexpr int FL_DROPPED_ITEM = 0x10;       // bg_public.h, KISAK_MP block
constexpr int FL_GROUND_ENT = 0x100000;     // bg_public.h
constexpr int ENT_HANDLER_DROPPED_ITEM = 0x10; // bg_public.h, KISAK_MP block
constexpr int TR_STATIONARY = 0x0;          // q_shared.h trType_t
constexpr int TR_INTERPOLATE = 0x1;
constexpr int TR_LINEAR = 0x2;
constexpr int TR_LINEAR_STOP = 0x3;
constexpr int TR_SINE = 0x4;
constexpr int TR_GRAVITY = 0x5;
constexpr int TR_ACCELERATE = 0x6;
constexpr int TR_DECELERATE = 0x7;
constexpr int TR_PHYSICS = 0x8;
constexpr int TR_FIRST_RAGDOLL = 0x9;
constexpr int TR_RAGDOLL_GRAVITY = 0xA;
constexpr int TR_RAGDOLL_INTERPOLATE = 0xB;
constexpr int IT_WEAPON = 0x1;              // bg_local.h itemType_t
constexpr int ERR_DROP = 0x1;               // qcommon.h
constexpr uint32_t PERK_GRENADE_DEATH = 0x40;
constexpr uint32_t WEAPFLAG_OFFHAND = 2;
constexpr float EQUAL_EPSILON = 0.001f;     // com_math.h

// ------------------------------------------------------- engine service doubles
// These stand in for services the shipped bodies call into. None is the code
// under test: each records what it was handed so the assertions can check the
// production body's own arguments and ordering.
struct XModel {
    char name[64];
};

struct WeaponDef {
    XModel *worldModel[16];
    int fuseTime;
};

struct dvarValue_t {
    const char *string;
};

struct dvar_t {
    dvarValue_t current;
};

struct playerState_s {
    int32_t grenadeTimeLeft;
    int32_t weapFlags;
    int32_t offHandIndex;
    int32_t weapon;
    int32_t perks;
    int32_t clientNum;
    uint8_t weaponmodels[128];
};

struct gclient_s {
    playerState_s ps;
};

struct Item {
    int32_t index;
};

// The shipped list is 4-byte elements: LaunchItem recovers the item index with
// pointer subtraction shifted right by two, so the double must stay that size or
// the extracted arithmetic would index a wider stride.
struct gitem_s {
    int32_t giType;
};

static_assert(sizeof(gitem_s) == 4, "LaunchItem shifts the item offset by two");
static gitem_s bg_itemlist[16];

struct trajectory_t {
    int trType;
    int trTime;
    int trDuration;
    float trBase[3];
    float trDelta[3];
};

struct EntityState {
    int number;
    int eType;
    int clientNum;
    int groundEntityNum;
    struct {
        int brushmodel;
    } index;
    struct {
        trajectory_t pos;
    } lerp;
};

struct gentity_s;

// The engine handle the item body calls isDefined()/entnum() on.
struct Handle {
    gentity_s *value = nullptr;
    bool isDefined() const { return value != nullptr; }
    gentity_s *ent() const { assert(value); return value; }
    int entnum() const;
};

struct SharedEntity {
    bool inuse = true;
    Handle ownerNum;
    float currentOrigin[3];
    float mins[3];
    float maxs[3];
    int contents = 0;
};

struct gentity_s {
    EntityState s;
    SharedEntity r;
    gclient_s *client = nullptr;
    int classname = 0;
    Item item[1];
    int flags = 0;
    int nextthink = 0;
    int handler = 0;
    int spawnflags = 0;
    void *tagInfo = nullptr;
};

struct dropCue_t {
    gentity_s *ent = nullptr;
    void setEnt(gentity_s *value) { ent = value; }
};
'''

DOUBLES = r'''
// ---------------------------------------------------------------- world state
static gentity_s g_entities[MAX_GENTITIES];

static struct {
    int time = 0;
    gentity_s *gentities = g_entities;
    dropCue_t droppedWeaponCue[8];
} level;

// gentity_s is fully declared by now, so the handle can reach its number.
inline int Handle::entnum() const { return ent()->s.number; }

struct trace_t {
    float fraction = 1.0f;
    float normal[3] = {0.0f, 0.0f, 1.0f};
    bool startsolid = false;
};

// --------------------------------------------------------------- spawn service
static int g_spawnCalls;
static gentity_s g_spawnResult;

static gentity_s *G_Spawn(void)
{
    ++g_spawnCalls;
    return &g_spawnResult;
}

static int g_dropCueIdx;
static int GetFreeDropCueIdx(void) { return g_dropCueIdx; }

// ---------------------------------------------------------------- weapon data
static WeaponDef *g_weaponDefs[128];
static int g_weaponDefLookups;

static WeaponDef *BG_GetWeaponDef(int index)
{
    ++g_weaponDefLookups;
    if (index < 0 || index >= 128) return nullptr;
    return g_weaponDefs[index];
}

static int g_perkGrenadeIndex;
static int g_perkNameLookups;

static int BG_FindWeaponIndexForName(const char *name)
{
    ++g_perkNameLookups;
    (void)name;
    return g_perkGrenadeIndex;
}

static dvar_t g_perkGrenadeDeathDvar = {"perk_grenadeDeath"};
static const dvar_t *perk_grenadeDeath = &g_perkGrenadeDeathDvar;

// ---------------------------------------------------------- condition / prints
static int g_conditionCalls;
static int g_conditionClient;
static int g_conditionType;
static int g_conditionValue;

static void BG_SetConditionValue(int clientNum, int condition, int value)
{
    ++g_conditionCalls;
    g_conditionClient = clientNum;
    g_conditionType = condition;
    g_conditionValue = value;
}

static int g_warningCalls;
static char g_warningText[256];

static void Com_PrintWarning(int channel, const char *format, ...)
{
    (void)channel;
    ++g_warningCalls;
    std::snprintf(g_warningText, sizeof(g_warningText), "%s", format);
}

// ------------------------------------------------------------ grenade service
static int g_fireCalls;
static gentity_s *g_fireParent;
static float g_fireSpot[3];
static float g_fireVel[3];
static uint32_t g_fireWeaponIndex;
static uint8_t g_fireWeaponModel;
static int g_fireRotate;
static int g_fireTime;

static gentity_s *G_FireGrenade(gentity_s *parent, float *start, float *dir,
                                uint32_t grenadeWPID, uint8_t grenModel,
                                int32_t rotate, int32_t time)
{
    ++g_fireCalls;
    g_fireParent = parent;
    for (int axis = 0; axis < 3; ++axis) {
        g_fireSpot[axis] = start[axis];
        g_fireVel[axis] = dir[axis];
    }
    g_fireWeaponIndex = grenadeWPID;
    g_fireWeaponModel = grenModel;
    g_fireRotate = rotate;
    g_fireTime = time;
    return nullptr;
}

// ------------------------------------------------------------ item services
static const char *g_setModelName;
static int g_setModelCalls;
static int g_dobjUpdateCalls;
static int g_linkEntityCalls;
static int g_freeEntityCalls;
static int g_runThinkCalls;
static int g_orientCalls;
static int g_setAngleCalls;
static float g_setAngleValue[3];

static void G_SetModel(gentity_s *ent, const char *name)
{
    ++g_setModelCalls;
    g_setModelName = name;
    ent->classname = 1;
}

static char *XModelGetName(XModel *model) { return model->name; }

static void G_DObjUpdate(gentity_s *ent) { (void)ent; ++g_dobjUpdateCalls; }
static void SV_LinkEntity(gentity_s *ent) { (void)ent; ++g_linkEntityCalls; }

static void G_FreeEntity(gentity_s *ent)
{
    (void)ent;
    ++g_freeEntityCalls;
    ent->r.inuse = false;
}

static void G_OrientItemToGround(gentity_s *ent, trace_t *trace)
{
    (void)ent;
    (void)trace;
    ++g_orientCalls;
}

static void G_SetAngle(gentity_s *ent, const float *angles)
{
    (void)ent;
    ++g_setAngleCalls;
    for (int axis = 0; axis < 3; ++axis) g_setAngleValue[axis] = angles[axis];
}

// A freed entity stops being updated: the item test drives that by clearing
// r.inuse from inside the double, exactly as G_FreeEntity would.
static bool g_runThinkFreesEntity;

static void G_RunThink(gentity_s *ent)
{
    ++g_runThinkCalls;
    if (g_runThinkFreesEntity) ent->r.inuse = false;
}

static void G_GetItemClassname(const gitem_s *item, int *classname)
{
    (void)item;
    *classname = 5;
}

// --------------------------------------------------------------- trace service
static float g_traceFraction = 1.0f;
static bool g_traceStartsolid;
static float g_traceNormal[3] = {0.0f, 0.0f, 1.0f};
static int g_traceCalls;
static int g_tracePassEntity;
static float g_traceStart[3];
static float g_traceEnd[3];

static void G_TraceCapsule(trace_t *tr, float *start, float *mins, float *maxs,
                           float *end, int passEntityNum, int mask)
{
    (void)mins;
    (void)maxs;
    (void)mask;
    ++g_traceCalls;
    g_tracePassEntity = passEntityNum;
    for (int axis = 0; axis < 3; ++axis) {
        g_traceStart[axis] = start[axis];
        g_traceEnd[axis] = end[axis];
    }
    tr->fraction = g_traceFraction;
    tr->startsolid = g_traceStartsolid;
    for (int axis = 0; axis < 3; ++axis) tr->normal[axis] = g_traceNormal[axis];
}

static int g_traceHitId = ENTITYNUM_NONE;
static int Trace_GetEntityHitId(trace_t *tr) { (void)tr; return g_traceHitId; }

static int g_itemClipMask = 0x80;
static int G_ItemClipMask(gentity_s *ent) { (void)ent; return g_itemClipMask; }

static int g_pointContents;
static int g_pointContentsCalls;

static int SV_PointContents(float *origin, int entityNum, uint32_t mask)
{
    (void)origin;
    (void)entityNum;
    (void)mask;
    ++g_pointContentsCalls;
    return g_pointContents;
}

// G_SetOrigin and BG_EvaluateTrajectory are the shipped definitions, extracted
// verbatim below, so the trajectory is really integrated over the frame instead of
// being handed a canned sample.

// ---------------------------------------------------------- trajectory services
static float Vec3Dot(const float *a, const float *b);

static float Vec3Length(const float *v)
{
    return static_cast<float>(std::sqrt(static_cast<double>(Vec3Dot(v, v))));
}

// Returns the original length, as the shared helper does. Normalisation keeps
// double intermediates so a unit vector stays unit within float precision.
static float Vec3NormalizeTo(const float *v, float *out)
{
    const double length = std::sqrt(static_cast<double>(v[0]) * v[0]
                                    + static_cast<double>(v[1]) * v[1]
                                    + static_cast<double>(v[2]) * v[2]);
    if (length > 0.0) {
        const double inverse = 1.0 / length;
        out[0] = static_cast<float>(v[0] * inverse);
        out[1] = static_cast<float>(v[1] * inverse);
        out[2] = static_cast<float>(v[2] * inverse);
    }
    return static_cast<float>(length);
}

static int g_comErrorCalls;
static char g_comErrorText[192];

static void Com_Error(int code, const char *format, ...)
{
    (void)code;
    ++g_comErrorCalls;
    std::snprintf(g_comErrorText, sizeof(g_comErrorText), "%s", format);
}

static void MyAssertHandler(const char *file, int line, int fatal,
                           const char *format, ...)
{
    (void)file;
    (void)line;
    (void)fatal;
    (void)format;
    std::printf("FAIL [assert] a production assertion was reached\n");
    std::fflush(stdout);
    std::abort();
}

// ----------------------------------------------------------- vector arithmetic
// Projections of the shared helpers the bodies call. Each is the documented
// formula, independently written so the assertions do not restate the body.
static void Vec3Scale(const float *in, float scale, float *out)
{
    for (int axis = 0; axis < 3; ++axis) out[axis] = in[axis] * scale;
}

static float Vec3Dot(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float Vec3LengthSq(const float *v) { return Vec3Dot(v, v); }

static void Vec3Sub(const float *a, const float *b, float *out)
{
    for (int axis = 0; axis < 3; ++axis) out[axis] = a[axis] - b[axis];
}

static void Vec3Lerp(const float *from, const float *to, float t, float *out)
{
    for (int axis = 0; axis < 3; ++axis) out[axis] = from[axis] + t * (to[axis] - from[axis]);
}

static void Vec3Mad(const float *start, float scale, const float *dir, float *out)
{
    for (int axis = 0; axis < 3; ++axis) out[axis] = start[axis] + scale * dir[axis];
}

// -------------------------------------------------------- controlled randomness
// The single CRT dependency: the shipped G_rand() reaches randomness through
// rand(), so one controlled value drives the whole G_crandom() chain.
static int g_randCalls;
static int g_randValue;
static std::vector<int> g_randQueue;

extern "C" int kisak_rand();

static void selectRand(int value)
{
    g_randValue = value;
    g_randCalls = 0;
    g_randQueue.clear();
}

// Queue one value per upcoming draw so a test can tell the axes apart.
static void selectRandSequence(const std::vector<int> &values)
{
    g_randValue = 0;
    g_randCalls = 0;
    g_randQueue = values;
}

extern "C" int kisak_rand()
{
    ++g_randCalls;
    if (!g_randQueue.empty()) {
        const int value = g_randQueue.front();
        g_randQueue.erase(g_randQueue.begin());
        return value;
    }
    return g_randValue;
}

// The shipped chain, restated from the raw CRT value: G_rand() masks to 15 bits,
// G_random() scales by 32768, G_crandom() maps to [-1,1] and the caller scales
// that by the launch speed.
static float expectedComponent(int randValue, float scale)
{
    const float fifteenBit = static_cast<float>(randValue & 0x7fff);
    return scale * (fifteenBit / 32768.0f * 2.0f - 1.0f);
}
'''

ASSERTIONS = r'''
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

static void expectFloat(float got, float want, float tolerance, const char *what)
{
    ++g_checks;
    if (!(std::fabs(got - want) <= tolerance)) {
        ++g_failures;
        std::printf("FAIL [%s] %s: got %.9g want %.9g\n", g_group, what,
                    static_cast<double>(got), static_cast<double>(want));
    }
}

static void expectVec3(const float *got, const float *want, float tolerance, const char *what)
{
    ++g_checks;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(got[axis] - want[axis]) <= tolerance) continue;
        ++g_failures;
        std::printf("FAIL [%s] %s: axis %d got %.9g want %.9g\n", g_group, what, axis,
                    static_cast<double>(got[axis]), static_cast<double>(want[axis]));
        return;
    }
}

// ================================================== DeathGrenadeDrop scenarios
static gentity_s g_dropper;
static gclient_s g_dropperClient;

static WeaponDef g_perkWeaponDef;
static WeaponDef g_itemWeaponDef;
static WeaponDef g_heldWeaponDef = {};

static void resetDropScene()
{
    static const char *const perkName = "perk_grenadeDeath";
    g_perkGrenadeDeathDvar.current.string = perkName;

    g_dropper = gentity_s{};
    g_dropperClient = gclient_s{};
    g_dropper.client = &g_dropperClient;
    g_dropper.s.number = 3;
    g_dropper.r.currentOrigin[0] = 100.0f;
    g_dropper.r.currentOrigin[1] = -200.0f;
    g_dropper.r.currentOrigin[2] = 50.0f;
    g_dropperClient.ps.clientNum = 3;

    g_fireCalls = 0;
    g_conditionCalls = 0;
    g_warningCalls = 0;
    g_perkNameLookups = 0;
    g_weaponDefLookups = 0;

    for (int index = 0; index < 128; ++index) g_weaponDefs[index] = &g_heldWeaponDef;
    g_perkWeaponDef = WeaponDef{};
    g_perkWeaponDef.fuseTime = 3000;
    g_weaponDefs[20] = &g_perkWeaponDef;
    g_perkGrenadeIndex = 0;
}

// A player still holding a grenade throws the grenade in the active weapon slot,
// with the model, rotate flag and remaining fuse of that slot.
static void checkHeldGrenadeUsesActiveWeapon()
{
    g_group = "held grenade";
    resetDropScene();

    g_dropperClient.ps.grenadeTimeLeft = 1000;
    g_dropperClient.ps.weapFlags = 0;
    g_dropperClient.ps.weapon = 7;
    g_dropperClient.ps.offHandIndex = 11;
    g_dropperClient.ps.weaponmodels[7] = 42;
    g_dropperClient.ps.weaponmodels[11] = 99;
    g_dropperClient.ps.perks = 0;

    const std::vector<int> draws = {1000, 20000, 32000};
    selectRandSequence(draws);
    DeathGrenadeDrop(&g_dropper, MOD_HEADSHOT);

    expectInt(g_fireCalls, 1, "a held grenade is thrown once");
    expectInt(static_cast<int>(g_fireWeaponIndex), 7, "the active weapon slot is the grenade");
    expectInt(g_fireWeaponModel, 42, "the model comes from the active weapon slot");
    expectInt(g_fireRotate, 1, "the thrown grenade rotates");
    expectInt(g_fireTime, 1000, "the thrown grenade carries the remaining fuse");
    expect(g_fireParent == &g_dropper, "the grenade is owned by the dying player");

    const float wantedSpot[3] = {100.0f, -200.0f, 90.0f};
    expectVec3(g_fireSpot, wantedSpot, 0.0f, "the launch spot is the origin raised by 40");

    expectInt(g_randCalls, 3, "the held grenade draws exactly three values");
    for (int axis = 0; axis < 3; ++axis) {
        expectFloat(g_fireVel[axis], expectedComponent(draws[axis], 160.0f), 1e-4f,
                    "each axis consumes one draw in X,Y,Z order");
        expect(g_fireVel[axis] > -160.0f && g_fireVel[axis] < 160.0f,
               "a launch component stays strictly inside the +/-160 scale");
    }

    expectInt(g_conditionCalls, 0, "no perk condition without the perk");
    expectInt(g_warningCalls, 0, "no warning when no perk grenade is requested");
}

// The offhand slot is used only while the offhand weapon flag is set.
static void checkOffhandSelection()
{
    g_group = "offhand grenade";
    resetDropScene();

    g_dropperClient.ps.grenadeTimeLeft = 250;
    g_dropperClient.ps.weapFlags = WEAPFLAG_OFFHAND;
    g_dropperClient.ps.weapon = 7;
    g_dropperClient.ps.offHandIndex = 11;
    g_dropperClient.ps.weaponmodels[7] = 42;
    g_dropperClient.ps.weaponmodels[11] = 99;

    selectRand(0);
    DeathGrenadeDrop(&g_dropper, MOD_HEADSHOT);

    expectInt(g_fireCalls, 1, "the offhand grenade is thrown once");
    expectInt(static_cast<int>(g_fireWeaponIndex), 11, "the offhand slot wins while the flag is set");
    expectInt(g_fireWeaponModel, 99, "the model comes from the offhand slot");
    expectInt(g_fireTime, 250, "the offhand grenade carries the remaining fuse");
}

// Nothing to drop when no grenade is held and the perk is absent.
static void checkNoHeldGrenade()
{
    g_group = "no held grenade";
    resetDropScene();

    g_dropperClient.ps.grenadeTimeLeft = 0;
    g_dropperClient.ps.perks = 0;

    selectRand(1234);
    DeathGrenadeDrop(&g_dropper, MOD_HEADSHOT);

    expectInt(g_fireCalls, 0, "nothing is thrown without a held grenade");
    expectInt(g_randCalls, 0, "nothing is drawn without a held grenade");
    expectInt(g_conditionCalls, 0, "no condition is raised");
    expectInt(g_warningCalls, 0, "no warning is raised");
}

// The perk throws a second grenade using the configured perk weapon and fuse.
static void checkPerkGrenade()
{
    g_group = "perk grenade";
    resetDropScene();

    g_dropperClient.ps.grenadeTimeLeft = 0;
    g_dropperClient.ps.perks = PERK_GRENADE_DEATH;
    g_dropperClient.ps.weaponmodels[20] = 77;
    g_perkGrenadeIndex = 20;

    const std::vector<int> draws = {100, 30000, 16000};
    selectRandSequence(draws);
    DeathGrenadeDrop(&g_dropper, MOD_HEADSHOT);

    expectInt(g_fireCalls, 1, "the perk throws one grenade");
    expectInt(static_cast<int>(g_fireWeaponIndex), 20, "the perk grenade is the named weapon");
    expectInt(g_fireWeaponModel, 77, "the perk grenade uses the perk slot's model");
    expectInt(g_fireTime, 3000, "the perk grenade uses the weapon def's fuse time");

    const float wantedSpot[3] = {100.0f, -200.0f, 90.0f};
    expectVec3(g_fireSpot, wantedSpot, 0.0f, "the perk grenade launches from the raised origin");

    expectInt(g_randCalls, 3, "the perk grenade draws exactly three values");
    for (int axis = 0; axis < 3; ++axis) {
        expectFloat(g_fireVel[axis], expectedComponent(draws[axis], 160.0f), 1e-4f,
                    "the perk grenade scales its draws like the held one");
    }

    expectInt(g_conditionCalls, 1, "the perk condition is raised once");
    expectInt(g_conditionClient, 3, "the condition names the dying client");
    expectInt(g_conditionType, ANIM_COND_PERK, "the condition is the perk condition");
    expectInt(g_conditionValue, ANIM_PERK_GRENADEDEATH,
              "the condition selects the grenade-death perk");
    expectInt(g_warningCalls, 0, "a known perk grenade raises no warning");
}

// An unknown perk grenade name is reported and throws nothing.
static void checkPerkGrenadeUnknownName()
{
    g_group = "perk grenade unknown";
    resetDropScene();

    g_dropperClient.ps.grenadeTimeLeft = 0;
    g_dropperClient.ps.perks = PERK_GRENADE_DEATH;
    g_perkGrenadeIndex = 0;

    selectRand(777);
    DeathGrenadeDrop(&g_dropper, MOD_HEADSHOT);

    expectInt(g_perkNameLookups, 1, "the perk grenade name is resolved once");
    expectInt(g_fireCalls, 0, "an unknown perk grenade throws nothing");
    expectInt(g_randCalls, 0, "an unknown perk grenade draws nothing");
    expectInt(g_conditionCalls, 0, "an unknown perk grenade raises no condition");
    expectInt(g_warningCalls, 1, "an unknown perk grenade is reported");
    expect(std::strstr(g_warningText, "perk_grenadeDeath") != nullptr,
           "the warning names the offending dvar");
}

// A suicide keeps the held-grenade drop but suppresses the perk grenade.
static void checkSuicideSuppressesPerk()
{
    g_group = "suicide suppression";
    resetDropScene();

    g_dropperClient.ps.grenadeTimeLeft = 900;
    g_dropperClient.ps.weapon = 7;
    g_dropperClient.ps.weaponmodels[7] = 42;
    g_dropperClient.ps.perks = PERK_GRENADE_DEATH;
    g_dropperClient.ps.weaponmodels[20] = 77;
    g_perkGrenadeIndex = 20;

    selectRand(4000);
    DeathGrenadeDrop(&g_dropper, MOD_SUICIDE);

    expectInt(g_fireCalls, 1, "a suicide still throws the grenade being held");
    expectInt(static_cast<int>(g_fireWeaponIndex), 7, "the held grenade is the one thrown");
    expectInt(g_randCalls, 3, "a suicide draws only the held grenade's three values");
    expectInt(g_conditionCalls, 0, "a suicide raises no perk condition");
    expectInt(g_warningCalls, 0, "a suicide reports no unknown perk grenade");
    expectInt(g_perkNameLookups, 0, "a suicide never resolves the perk grenade name");

    // ...and the same death with nothing held has no effect at all.
    resetDropScene();
    g_dropperClient.ps.grenadeTimeLeft = 0;
    g_dropperClient.ps.perks = PERK_GRENADE_DEATH;
    g_perkGrenadeIndex = 20;

    selectRand(4000);
    DeathGrenadeDrop(&g_dropper, MOD_SUICIDE);

    expectInt(g_fireCalls, 0, "a suicide holding no grenade throws nothing");
    expectInt(g_conditionCalls, 0, "a suicide holding no grenade raises no condition");
}

// Holding a grenade and carrying the perk are independent: both are thrown, with
// their own weapon, model and fuse.
static void checkHeldAndPerkAreIndependent()
{
    g_group = "held plus perk";
    resetDropScene();

    g_dropperClient.ps.grenadeTimeLeft = 500;
    g_dropperClient.ps.weapon = 7;
    g_dropperClient.ps.weaponmodels[7] = 42;
    g_dropperClient.ps.perks = PERK_GRENADE_DEATH;
    g_dropperClient.ps.weaponmodels[20] = 77;
    g_perkGrenadeIndex = 20;

    // One draw per component per grenade; the held grenade is thrown first.
    const std::vector<int> draws = {1000, 2000, 3000, 4000, 5000, 6000};
    selectRandSequence(draws);
    DeathGrenadeDrop(&g_dropper, MOD_HEADSHOT);

    expectInt(g_fireCalls, 2, "a held grenade and the perk both throw");
    expectInt(g_randCalls, 6, "both grenades draw three components each");

    // The recorder keeps the last call, which is the perk grenade.
    expectInt(static_cast<int>(g_fireWeaponIndex), 20, "the perk grenade is thrown second");
    expectInt(g_fireTime, 3000, "the perk grenade uses the perk fuse");
    expectFloat(g_fireVel[0], expectedComponent(draws[3], 160.0f), 1e-4f,
                "the perk grenade starts at the fourth draw");
    expectFloat(g_fireVel[2], expectedComponent(draws[5], 160.0f), 1e-4f,
                "the perk grenade ends at the sixth draw");

    expectInt(g_conditionCalls, 1, "the perk condition is raised once");
}

// A plain death with no grenade held and no perk changes nothing.
static void checkNormalDeathHasNoEffect()
{
    g_group = "normal death";
    resetDropScene();

    g_dropperClient.ps.grenadeTimeLeft = 0;
    g_dropperClient.ps.perks = 0;

    selectRand(12345);
    DeathGrenadeDrop(&g_dropper, MOD_HEADSHOT);

    expectInt(g_fireCalls, 0, "a normal death throws no grenade");
    expectInt(g_randCalls, 0, "a normal death draws nothing");
    expectInt(g_conditionCalls, 0, "a normal death raises no condition");
    expectInt(g_warningCalls, 0, "a normal death reports nothing");
    expectInt(g_weaponDefLookups, 0, "a normal death resolves no weapon def");
    expectInt(g_perkNameLookups, 0, "a normal death resolves no perk name");
}

// ============================================== LaunchItem / G_RunItem scenarios
static gentity_s g_runEnt;

static void resetItemScene()
{
    for (int index = 0; index < MAX_GENTITIES; ++index) g_entities[index] = gentity_s{};
    g_runEnt = gentity_s{};
    g_spawnResult = gentity_s{};
    g_spawnResult.r.inuse = true;

    level.time = 2000;
    level.gentities = g_entities;
    for (int cue = 0; cue < 8; ++cue) level.droppedWeaponCue[cue] = dropCue_t{};

    g_spawnCalls = 0;
    g_dropCueIdx = 2;
    g_weaponDefLookups = 0;
    g_setModelCalls = 0;
    g_setModelName = nullptr;
    g_dobjUpdateCalls = 0;
    g_linkEntityCalls = 0;
    g_freeEntityCalls = 0;
    g_runThinkCalls = 0;
    g_orientCalls = 0;
    g_setAngleCalls = 0;
    g_comErrorCalls = 0;
    g_traceCalls = 0;
    g_tracePassEntity = ENTITYNUM_NONE;
    g_traceFraction = 1.0f;
    g_traceStartsolid = false;
    g_traceNormal[0] = 0.0f;
    g_traceNormal[1] = 0.0f;
    g_traceNormal[2] = 1.0f;
    g_traceHitId = ENTITYNUM_NONE;
    g_pointContents = 0;
    g_pointContentsCalls = 0;
    g_runThinkFreesEntity = false;
    g_itemClipMask = 0x80;

    g_itemWeaponDef = WeaponDef{};
    static XModel worldModel;
    std::snprintf(worldModel.name, sizeof(worldModel.name), "%s", "mp_weapon_dropped");
    g_itemWeaponDef.worldModel[0] = &worldModel;
    for (int index = 0; index < 128; ++index) g_weaponDefs[index] = &g_itemWeaponDef;
    bg_itemlist[3].giType = IT_WEAPON;
}

// LaunchItem seeds the spawned drop: gravity trajectory, velocity, model, owner,
// bounds and the dropped-item lifetime.
static void checkLaunchItemInitialization()
{
    g_group = "LaunchItem initialization";
    resetItemScene();

    float origin[3] = {10.0f, 20.0f, 30.0f};
    float angles[3] = {0.0f, 45.0f, 0.0f};
    float velocity[3] = {100.0f, -50.0f, 200.0f};

    gentity_s *dropped = LaunchItem(&bg_itemlist[3], origin, angles, velocity, 5);

    expect(dropped == &g_spawnResult, "LaunchItem returns the entity it spawned");
    expectInt(g_spawnCalls, 1, "launching an item spawns one entity");

    expectInt(dropped->s.eType, ET_ITEM, "the drop is an item entity");
    expectInt(dropped->s.index.brushmodel, 3, "the brush model names the item index");
    expectInt(dropped->item[0].index, 3, "the item slot records the item index");
    expectInt(dropped->s.clientNum, 5, "the drop records the owner it was given");
    expectInt(dropped->handler, ENT_HANDLER_DROPPED_ITEM, "the drop uses the dropped-item handler");

    expectInt(dropped->r.contents,
              CONTENTS_ITEM | CONTENTS_ANY_TRIGGER | CONTENTS_USE,
              "the drop is a usable, triggerable item");
    const float wantedMins[3] = {-1.0f, -1.0f, -1.0f};
    const float wantedMaxs[3] = {1.0f, 1.0f, 1.0f};
    expectVec3(dropped->r.mins, wantedMins, 0.0f, "the drop shrinks to a small box");
    expectVec3(dropped->r.maxs, wantedMaxs, 0.0f, "the drop shrinks to a small box");

    expectInt(g_setModelCalls, 1, "the world model is applied once");
    expect(g_setModelName != nullptr && std::strcmp(g_setModelName, "mp_weapon_dropped") == 0,
           "the drop takes the weapon def's world model name");
    expectInt(g_dobjUpdateCalls, 1, "the drop's dynamic model is updated");

    expectInt(g_setAngleCalls, 1, "the drop is oriented once");
    expectVec3(g_setAngleValue, angles, 0.0f, "the drop is oriented along the launch angles");

    // G_SetOrigin runs first in the shipped body and would reset the trajectory to
    // TR_STATIONARY with no velocity. The gravity seeding that follows must survive
    // it: these assertions fail if that ordering is ever inverted.
    expectInt(dropped->s.lerp.pos.trType, TR_GRAVITY, "the drop falls under gravity");
    expectInt(dropped->s.lerp.pos.trTime, level.time, "the trajectory starts now");
    expectInt(dropped->s.lerp.pos.trDuration, 0, "a gravity fall has no fixed duration");
    expectVec3(dropped->s.lerp.pos.trBase, origin, 0.0f,
               "the trajectory is based at the launch origin, as G_SetOrigin left it");
    expectVec3(dropped->r.currentOrigin, origin, 0.0f, "the drop is placed at the launch origin");
    expectVec3(dropped->s.lerp.pos.trDelta, velocity, 0.0f,
               "the drop keeps the launch velocity");

    expectInt(dropped->flags, FL_DROPPED_ITEM, "the drop is flagged as a dropped item");
    expectInt(dropped->nextthink, level.time + 1000, "the drop gets a one-second next think");
    expectInt(g_linkEntityCalls, 1, "the drop is linked once");
    expect(level.droppedWeaponCue[2].ent == dropped, "the drop claims a weapon drop cue");
}

// Without a world model the drop keeps the default model instead of inventing one.
static void checkLaunchItemWithoutWorldModel()
{
    g_group = "LaunchItem no world model";
    resetItemScene();

    g_itemWeaponDef.worldModel[0] = nullptr;

    float origin[3] = {1.0f, 2.0f, 3.0f};
    float angles[3] = {0.0f, 0.0f, 0.0f};
    float velocity[3] = {0.0f, 0.0f, 0.0f};

    gentity_s *dropped = LaunchItem(&bg_itemlist[3], origin, angles, velocity, 0);

    expectInt(g_setModelCalls, 0, "a weapon def without a world model sets no model");
    expectInt(dropped->s.lerp.pos.trType, TR_GRAVITY, "the drop still falls under gravity");
    expectInt(g_dobjUpdateCalls, 1, "the dynamic model is updated regardless");
}

// One frame of a falling item: the shipped BG_EvaluateTrajectory is asked for the
// position 50 ms ahead and the item moves there when nothing blocks it.
static void checkRunItemStepsOneFrame()
{
    g_group = "G_RunItem frame step";
    resetItemScene();

    gentity_s &ent = g_runEnt;
    ent.s.groundEntityNum = ENTITYNUM_NONE;
    ent.s.lerp.pos.trType = TR_GRAVITY;
    ent.s.lerp.pos.trTime = level.time;
    ent.s.lerp.pos.trBase[0] = 5.0f;
    ent.s.lerp.pos.trBase[1] = 6.0f;
    ent.s.lerp.pos.trBase[2] = 7.0f;
    // 20 units per second along X: exactly one unit over a 50 ms frame.
    ent.s.lerp.pos.trDelta[0] = 20.0f;
    for (int axis = 0; axis < 3; ++axis) ent.r.currentOrigin[axis] = ent.s.lerp.pos.trBase[axis];

    g_traceFraction = 1.0f;

    G_RunItem(&ent);

    expectInt(g_traceCalls, 1, "the item traces its step once");

    // x = base + dt*delta = 5 + 0.05*20 = 6; z = base - dt*400*dt = 7 - 1 = 6.
    // A 100 ms sample would instead give (7, 6, 3), so this pins the time unit.
    const float wantedOrigin[3] = {6.0f, 6.0f, 6.0f};
    expectVec3(ent.r.currentOrigin, wantedOrigin, 1e-4f,
               "a clear step lands on the 50 ms trajectory sample");

    expectInt(g_linkEntityCalls, 1, "a moved item is relinked");
    expectInt(g_runThinkCalls, 1, "a moved item runs its think");
    expectInt(g_orientCalls, 0, "a clear step never reorients the item");
    expectInt(g_freeEntityCalls, 0, "a clear step never frees the item");
    expectInt(ent.s.groundEntityNum, ENTITYNUM_NONE, "a clear step sets no ground entity");
}

// An item that starts a frame on moving ground re-arms its own gravity fall from
// its current origin, with no inherited velocity.
static void checkRunItemMovingGround()
{
    g_group = "G_RunItem moving ground";
    resetItemScene();

    gentity_s &ent = g_runEnt;
    ent.s.groundEntityNum = 7;
    ent.s.lerp.pos.trType = TR_STATIONARY;
    ent.s.lerp.pos.trBase[0] = 1.0f;
    ent.s.lerp.pos.trDelta[0] = 9.0f;
    ent.r.currentOrigin[0] = 40.0f;
    ent.r.currentOrigin[1] = 50.0f;
    ent.r.currentOrigin[2] = 60.0f;
    level.gentities[7].s.lerp.pos.trType = TR_LINEAR_STOP;

    g_traceFraction = 1.0f;

    G_RunItem(&ent);

    expectInt(ent.s.lerp.pos.trType, TR_GRAVITY, "moving ground re-arms a gravity fall");
    expectInt(ent.s.lerp.pos.trTime, level.time, "the re-armed trajectory starts now");
    const float wantedBase[3] = {40.0f, 50.0f, 60.0f};
    expectVec3(ent.s.lerp.pos.trBase, wantedBase, 0.0f,
               "the re-armed trajectory starts at the item's origin");
    const float noVelocity[3] = {0.0f, 0.0f, 0.0f};
    expectVec3(ent.s.lerp.pos.trDelta, noVelocity, 0.0f,
               "re-arming drops the inherited velocity");

    // With no velocity the only motion over 50 ms is the 1-unit gravity drop.
    const float fallenOrigin[3] = {40.0f, 50.0f, 59.0f};
    expectVec3(ent.r.currentOrigin, fallenOrigin, 1e-4f,
               "the re-armed item falls one unit over the frame");
}

// A blocked step settles the item: it stops linearly over 50 ms, and the velocity
// it stores covers the remaining displacement in exactly that time.
static void checkRunItemSettlesAgainstGeometry()
{
    g_group = "G_RunItem settle";
    resetItemScene();

    gentity_s &ent = g_runEnt;
    ent.s.groundEntityNum = ENTITYNUM_NONE;
    ent.s.lerp.pos.trType = TR_GRAVITY;
    ent.s.lerp.pos.trTime = level.time;
    // 2000 units per second along X: the 50 ms sample is 100 units out.
    ent.s.lerp.pos.trDelta[0] = 2000.0f;
    ent.r.ownerNum.value = &g_entities[9];
    g_entities[9].s.number = 9;

    g_traceFraction = 0.5f;

    G_RunItem(&ent);

    expectInt(g_tracePassEntity, 9, "the trace passes the item's owner");
    expectInt(ent.s.lerp.pos.trType, TR_LINEAR_STOP, "a blocked item stops linearly");
    expectInt(ent.s.lerp.pos.trDuration, 50, "the settle lasts one 50 ms frame");
    expectInt(ent.s.lerp.pos.trTime, level.time, "the settle starts now");

    const float wantedOrigin[3] = {50.0f, 0.0f, -0.5f};
    expectVec3(ent.r.currentOrigin, wantedOrigin, 1e-4f,
               "the item advances to the clipped fraction of its step");

    // trDelta is expressed per second, so it must cover the displacement over the
    // whole stored duration: a settled item neither overshoots nor stalls.
    for (int axis = 0; axis < 3; ++axis) {
        const float covered = ent.s.lerp.pos.trDelta[axis] * (50.0f / 1000.0f);
        const float moved = ent.r.currentOrigin[axis] - ent.s.lerp.pos.trBase[axis];
        expectFloat(covered, moved, 1e-3f,
                    "the settle velocity covers the frame's displacement");
    }
    expectInt(g_freeEntityCalls, 0, "a mid-air settle frees nothing");
    expectInt(g_orientCalls, 0, "a mid-air settle orients nothing");
}

// A short blocked step that lands on a floor settles the item onto the ground, and
// the landing leaves it stationary so the next frame only thinks.
static void checkRunItemLandsOnGround()
{
    g_group = "G_RunItem ground landing";
    resetItemScene();

    gentity_s &ent = g_runEnt;
    ent.s.groundEntityNum = ENTITYNUM_NONE;
    ent.s.lerp.pos.trType = TR_GRAVITY;
    ent.s.lerp.pos.trTime = level.time;
    ent.s.lerp.pos.trDelta[2] = -80.0f;
    ent.r.currentOrigin[2] = 10.0f;
    ent.s.lerp.pos.trBase[2] = 10.0f;  // a drop's trajectory is based at its origin

    g_traceFraction = 0.005f;
    g_traceNormal[2] = 0.9f;
    g_pointContents = 0;
    g_traceHitId = 7;

    G_RunItem(&ent);

    expectInt(g_orientCalls, 1, "a landed item is oriented to the ground");
    // G_SetOrigin leaves the item stationary at the clipped end of its step.
    expectInt(ent.s.lerp.pos.trType, TR_STATIONARY, "landing stops the item");
    expectInt(ent.s.lerp.pos.trTime, 0, "a stationary item has no trajectory start");
    expectInt(ent.s.lerp.pos.trDuration, 0, "a stationary item has no trajectory duration");
    expectFloat(ent.r.currentOrigin[2], 9.975f, 1e-3f,
                "the landed item sits at the clipped end of its step");
    expectVec3(ent.s.lerp.pos.trBase, ent.r.currentOrigin, 0.0f,
               "the landed item's trajectory is based where it came to rest");
    expectInt(ent.s.groundEntityNum, 7, "the landed item records the ground it hit");
    expect(g_entities[7].flags & FL_GROUND_ENT, "the ground entity is flagged as ground");
    expectInt(g_linkEntityCalls, 2, "a landed item is relinked after settling");
    expectInt(g_freeEntityCalls, 0, "landing on a floor never frees the item");

    // Second frame: the item is stationary on stationary ground, so the shipped
    // body takes its think-only branch and does not re-sample the trajectory.
    const float restingOrigin[3] = {ent.r.currentOrigin[0], ent.r.currentOrigin[1],
                                    ent.r.currentOrigin[2]};
    const int traces = g_traceCalls;
    const int thinks = g_runThinkCalls;

    G_RunItem(&ent);

    expectInt(g_traceCalls, traces, "a resting landed item traces nothing on the next frame");
    expectInt(g_runThinkCalls, thinks + 1, "a resting landed item still thinks");
    expectVec3(ent.r.currentOrigin, restingOrigin, 0.0f,
               "a resting landed item does not move on the next frame");
    expectInt(ent.s.lerp.pos.trType, TR_STATIONARY, "a resting landed item stays stationary");
}

// The item stops being updated once it has been freed: the ground branch is
// guarded by r.inuse, which G_RunThink may have cleared.
static void checkRunItemFreedEntitySkipsGroundUpdate()
{
    g_group = "G_RunItem freed entity";
    resetItemScene();

    gentity_s &ent = g_runEnt;
    ent.s.groundEntityNum = ENTITYNUM_NONE;
    ent.s.lerp.pos.trType = TR_GRAVITY;
    ent.s.lerp.pos.trTime = level.time;
    ent.s.lerp.pos.trDelta[2] = -80.0f;
    ent.r.currentOrigin[2] = 10.0f;

    g_traceFraction = 0.005f;
    g_traceNormal[2] = 0.9f;
    g_pointContents = 0;
    g_runThinkFreesEntity = true;

    G_RunItem(&ent);

    expect(!ent.r.inuse, "the entity was freed during its think");
    expectInt(g_runThinkCalls, 1, "the think still ran");
    expectInt(g_orientCalls, 0, "a freed item is not reoriented");
    // Landing would have run G_SetOrigin, which forces TR_STATIONARY; the settle
    // state surviving proves the ground branch was skipped entirely.
    expectInt(ent.s.lerp.pos.trType, TR_LINEAR_STOP,
              "a freed item keeps its settle trajectory instead of being placed");
    expectInt(g_freeEntityCalls, 0, "a freed item is not freed a second time");
}

// Two conditions free the item instead of settling it onto the ground: a surface
// that does not face upwards, and a solid point at the resting position.
static void checkRunItemFreesWhenBlocked()
{
    g_group = "G_RunItem free";

    // A downward- or side-facing surface.
    resetItemScene();
    gentity_s &ent = g_runEnt;
    ent.s.groundEntityNum = ENTITYNUM_NONE;
    ent.s.lerp.pos.trType = TR_GRAVITY;
    ent.s.lerp.pos.trTime = level.time;
    ent.s.lerp.pos.trDelta[2] = -80.0f;
    ent.r.currentOrigin[2] = 10.0f;
    g_traceFraction = 0.005f;
    g_traceNormal[2] = -1.0f;
    G_RunItem(&ent);
    expectInt(g_freeEntityCalls, 1, "a surface facing away frees the item");
    expectInt(g_orientCalls, 0, "a freed item is not oriented");

    // A solid resting position.
    resetItemScene();
    gentity_s &embedded = g_runEnt;
    embedded.s.groundEntityNum = ENTITYNUM_NONE;
    embedded.s.lerp.pos.trType = TR_GRAVITY;
    embedded.s.lerp.pos.trTime = level.time;
    embedded.s.lerp.pos.trDelta[2] = -80.0f;
    embedded.r.currentOrigin[2] = 10.0f;
    g_traceFraction = 0.005f;
    g_traceNormal[2] = 0.9f;
    g_pointContents = 0x1;
    G_RunItem(&embedded);
    expectInt(g_pointContentsCalls, 1, "the resting position contents are sampled");
    expectInt(g_freeEntityCalls, 1, "a solid resting position frees the item");
    expectInt(g_orientCalls, 0, "an embedded item is not oriented");
}

// A stationary item on stationary ground only thinks: it is not re-simulated.
static void checkRunItemStationaryOnlyThinks()
{
    g_group = "G_RunItem stationary";
    resetItemScene();

    gentity_s &ent = g_runEnt;
    ent.s.groundEntityNum = 7;
    ent.s.lerp.pos.trType = TR_STATIONARY;
    level.gentities[7].s.lerp.pos.trType = TR_STATIONARY;
    ent.r.currentOrigin[2] = 33.0f;

    G_RunItem(&ent);

    expectInt(g_traceCalls, 0, "a resting item traces nothing");
    expectInt(g_runThinkCalls, 1, "a resting item still thinks");
    expectFloat(ent.r.currentOrigin[2], 33.0f, 0.0f, "a resting item does not move");
    expectInt(ent.s.lerp.pos.trType, TR_STATIONARY, "a resting item stays stationary");
    expectInt(g_linkEntityCalls, 0, "a resting item is not relinked");
}

// A step that would not move the item at all is nudged down a unit first, so a
// resting item keeps re-testing the ground instead of jittering in place.
static void checkRunItemNudgesAStillStep()
{
    g_group = "G_RunItem still step";
    resetItemScene();

    gentity_s &ent = g_runEnt;
    ent.s.groundEntityNum = ENTITYNUM_NONE;
    ent.s.lerp.pos.trType = TR_GRAVITY;
    ent.s.lerp.pos.trTime = level.time;
    // Falling at 20 units per second cancels the 1-unit gravity drop over the
    // frame, so the sample coincides with the origin and the nudge applies.
    ent.s.lerp.pos.trDelta[2] = 20.0f;
    ent.r.currentOrigin[2] = 20.0f;
    ent.s.lerp.pos.trBase[2] = 20.0f;

    g_traceFraction = 1.0f;

    G_RunItem(&ent);

    expectFloat(g_traceEnd[2], 19.0f, 1e-4f,
                "an empty step is nudged one unit down before tracing");
    expectFloat(ent.r.currentOrigin[2], 19.0f, 1e-4f, "the nudged step is taken");
}

// A steep, barely-penetrating step is slid along the surface and retraced before
// the item settles, rather than being clipped where it first touched.
static void checkRunItemSlidesAlongSteepSurface()
{
    g_group = "G_RunItem surface slide";
    resetItemScene();

    gentity_s &ent = g_runEnt;
    ent.s.groundEntityNum = ENTITYNUM_NONE;
    ent.s.lerp.pos.trType = TR_GRAVITY;
    ent.s.lerp.pos.trTime = level.time;
    ent.s.lerp.pos.trDelta[0] = 2000.0f;
    ent.r.currentOrigin[0] = 0.0f;

    g_traceFraction = 0.005f;
    g_traceNormal[0] = 0.99f;
    g_traceNormal[2] = 0.1f;

    G_RunItem(&ent);

    // The slide re-traces from the clipped end, so the item is traced twice.
    expectInt(g_traceCalls, 2, "a sliding step is retraced after the slide");

    // The slope still faces upwards and the rest position is clear, so the item
    // lies on it: G_SetOrigin places it and leaves it stationary.
    expectInt(g_orientCalls, 1, "an item on a shallow slope is oriented to it");
    expectInt(g_freeEntityCalls, 0, "a shallow slope does not free the item");
    expectInt(g_pointContentsCalls, 1, "the sloped rest position is content-checked");
    expectInt(ent.s.lerp.pos.trType, TR_STATIONARY,
              "a placed item is left stationary by G_SetOrigin");
    expectVec3(ent.s.lerp.pos.trBase, ent.r.currentOrigin, 0.0f,
               "a placed item's trajectory is based where it lies");
}

// The launched drop feeds the per-frame update: the velocity LaunchItem stored is
// consumed by the real trajectory integration, not merely observed.
static void checkLaunchThenRun()
{
    g_group = "launch then run";
    resetItemScene();

    float origin[3] = {0.0f, 0.0f, 64.0f};
    float angles[3] = {0.0f, 0.0f, 0.0f};
    float velocity[3] = {120.0f, 0.0f, -40.0f};

    gentity_s *dropped = LaunchItem(&bg_itemlist[3], origin, angles, velocity, 5);
    dropped->s.groundEntityNum = ENTITYNUM_NONE;

    g_traceFraction = 1.0f;

    G_RunItem(dropped);

    // x = 0 + 0.05*120 = 6; z = 64 + 0.05*(-40) - 0.05*400*0.05 = 64 - 2 - 1 = 61.
    // These are the shipped launch velocity's own consequences, so a wrong
    // trajectory object or a wrong frame length changes them.
    const float wantedOrigin[3] = {6.0f, 0.0f, 61.0f};
    expectVec3(dropped->r.currentOrigin, wantedOrigin, 1e-4f,
               "the launched drop follows its own trajectory over one 50 ms frame");
    expectInt(g_comErrorCalls, 0, "the shipped trajectory accepts the drop's type");
}

int main()
{
    checkHeldGrenadeUsesActiveWeapon();
    checkOffhandSelection();
    checkNoHeldGrenade();
    checkPerkGrenade();
    checkPerkGrenadeUnknownName();
    checkSuicideSuppressesPerk();
    checkHeldAndPerkAreIndependent();
    checkNormalDeathHasNoEffect();

    checkLaunchItemInitialization();
    checkLaunchItemWithoutWorldModel();
    checkRunItemStepsOneFrame();
    checkRunItemMovingGround();
    checkRunItemSettlesAgainstGeometry();
    checkRunItemLandsOnGround();
    checkRunItemFreedEntitySkipsGroundUpdate();
    checkRunItemFreesWhenBlocked();
    checkRunItemStationaryOnlyThinks();
    checkRunItemNudgesAStillStep();
    checkRunItemSlidesAlongSteepSurface();
    checkLaunchThenRun();

    std::printf("%d runtime checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

'''


def build_program(g_utils, bg_misc, g_combat, g_items) -> str:
    """Assemble the host program from harness, doubles and shipped bodies."""
    return "".join(
        [
            HARNESS,
            "",
            DOUBLES,
            "",
            # The three G_* random helpers the death-grenade path draws from, taken
            # from the shipped multiplayer definitions so the velocity arithmetic
            # under test is production code rather than a reproduction.
            function(g_utils, "G_rand", "int __cdecl G_rand()", draws=1),
            function(g_utils, "G_random", "float __cdecl G_random()", draws=0),
            function(g_utils, "G_crandom", "float __cdecl G_crandom()", draws=0),
            "",
            # The shipped placement and trajectory integrator. Extracting these is
            # what makes the item tests measure real behaviour: G_SetOrigin really
            # resets the trajectory (so LaunchItem's seeding must survive it), and
            # BG_EvaluateTrajectory really integrates the frame instead of being
            # handed a canned sample.
            extract(g_utils, "G_SetOrigin", "void __cdecl G_SetOrigin(gentity_s *ent, const float *origin)"),
            extract(bg_misc, "BG_EvaluateTrajectory",
                    "void __cdecl BG_EvaluateTrajectory(const trajectory_t *tr, int32_t atTime, "
                    "float *result)"),
            "",
            # The code under test.
            extract(g_combat, "DeathGrenadeDrop",
                    "void __cdecl DeathGrenadeDrop(gentity_s *self, int32_t meansOfDeath)"),
            extract(g_items, "LaunchItem",
                    "gentity_s *__cdecl LaunchItem(const gitem_s *item, float *origin, "
                    "float *angles, float *velocity, int32_t ownerNum)"),
            extract(g_items, "G_RunItem", "void __cdecl G_RunItem(gentity_s *ent)"),
            ASSERTIONS,
        ]
    )


def header_value(source: str, name: str, pattern: str) -> int | None:
    """Read one enum/#define value out of a shipping header.

    Returns None when the name is not declared in the expected shape, so a caller
    reports a missing check rather than silently passing.
    """
    match = re.search(pattern.format(name=re.escape(name)), source)
    if not match:
        return None
    text = match.group(1).replace("'", "").replace("0x", "").replace("0X", "")
    try:
        return int(text, 16) if re.search(r"0[xX]", match.group(1)) else int(text, 10)
    except ValueError:
        return None


# The harness constants are transcriptions; these are the shipped declarations
# they must equal. A mismatch here means the test is asserting against invented
# identifiers rather than the engine's own.
CONSTANT_SOURCES = (
    ("TR_GRAVITY", "q_shared", r"TR_GRAVITY = (0[xX][0-9A-Fa-f]+)"),
    ("TR_RAGDOLL_GRAVITY", "q_shared", r"TR_RAGDOLL_GRAVITY = (0[xX][0-9A-Fa-f]+)"),
    ("TR_LINEAR_STOP", "q_shared", r"TR_LINEAR_STOP = (0[xX][0-9A-Fa-f]+)"),
    ("CONTENTS_ITEM", "q_shared", r"#define CONTENTS_ITEM +(0[xX][0-9A-Fa-f]+)"),
    ("CONTENTS_USE", "q_shared", r"#define CONTENTS_USE +(0[xX][0-9A-Fa-f]+)"),
    ("FL_GROUND_ENT", "bg_public", r"FL_GROUND_ENT += (0[xX][0-9A-Fa-f']+)"),
    ("ANIM_COND_PERK", "bg_public", r"ANIM_COND_PERK = (0[xX][0-9A-Fa-f]+)"),
    ("ANIM_PERK_GRENADEDEATH", "bg_public", r"ANIM_PERK_GRENADEDEATH = (0[xX][0-9A-Fa-f]+)"),
    ("MOD_SUICIDE", "g_local", r"MOD_SUICIDE = (0[xX][0-9A-Fa-f]+)"),
    ("IT_WEAPON", "bg_local", r"IT_WEAPON = (0[xX][0-9A-Fa-f]+)"),
)


def source_checks(g_combat, g_items, g_utils, bg_misc, headers) -> int:
    """Shape assertions that the extracted code is still the code that ships."""
    # bg_public.h declares the handler and flag blocks twice, once per build. The
    # KISAK_MP block comes first, so the first match is the numbering this build
    # uses; the ANIM names have a single later declaration and match the same way.
    mp_block = headers["bg_public"]

    constants = dict(
        (name, header_value(headers[key] if key != "bg_public" else mp_block, name, pattern))
        for name, key, pattern in CONSTANT_SOURCES
    )
    # The two MP-only names need their own patterns against the MP slice.
    constants["ENT_HANDLER_DROPPED_ITEM"] = header_value(
        mp_block, "ENT_HANDLER_DROPPED_ITEM", r"ENT_HANDLER_DROPPED_ITEM = (0[xX][0-9A-Fa-f]+)")
    constants["FL_DROPPED_ITEM"] = header_value(
        mp_block, "FL_DROPPED_ITEM", r"FL_DROPPED_ITEM += (0[xX][0-9A-Fa-f]+)")

    # Values as the harness declares them, so a transcription slip is caught here
    # rather than being mistaken for a production regression.
    declared = dict(
        (match.group(1), int(match.group(2).replace("'", ""), 0))
        for match in re.finditer(r"constexpr int (\w+) = (0[xX][0-9A-Fa-f']+);", HARNESS)
    )
    declared.update(
        (match.group(1), int(match.group(2).replace("'", ""), 0))
        for match in re.finditer(r"constexpr int (\w+) = (\d+);", HARNESS)
    )

    expected = {
        "TR_GRAVITY": 0x5,
        "TR_RAGDOLL_GRAVITY": 0xA,
        "TR_LINEAR_STOP": 0x3,
        "CONTENTS_ITEM": 0x100,
        "CONTENTS_USE": 0x200000,
        "FL_GROUND_ENT": 0x100000,
        "FL_DROPPED_ITEM": 0x10,
        "ENT_HANDLER_DROPPED_ITEM": 0x10,
        "ANIM_COND_PERK": 0x9,
        "ANIM_PERK_GRENADEDEATH": 0x2,
        "MOD_SUICIDE": 0xC,
        "IT_WEAPON": 0x1,
    }

    checks = [
        ("DeathGrenadeDrop selects the offhand slot behind weapFlags bit 1",
         "(self->client->ps.weapFlags & 2) != 0" in g_combat),
        ("DeathGrenadeDrop gates the perk on meansOfDeath != MOD_SUICIDE",
         "meansOfDeath != MOD_SUICIDE && (self->client->ps.perks & 0x40) != 0" in g_combat),
        ("DeathGrenadeDrop raises the launch spot by 40 units",
         "launchspot[2] = launchspot[2] + 40.0;" in g_combat),
        # G_SetOrigin resets the trajectory to TR_STATIONARY and zeroes the
        # velocity, so LaunchItem must seed gravity after it places the drop.
        ("G_SetOrigin resets the trajectory it places",
         "ent->s.lerp.pos.trType = TR_STATIONARY;" in g_utils
         and "ent->s.lerp.pos.trDelta[2] = 0.0f;" in g_utils),
        ("LaunchItem places the drop before seeding its gravity trajectory",
         g_items.index("G_SetOrigin(dropped, origin);")
         < g_items.index("dropped->s.lerp.pos.trType = TR_GRAVITY;")),
        # The frame the item is stepped by, and the constant gravity term the
        # integration applies, are both load-bearing for the item assertions.
        ("G_RunItem samples the trajectory one 50 ms frame ahead",
         "BG_EvaluateTrajectory(&ent->s.lerp.pos, level.time + 50, origin);" in g_items),
        ("BG_EvaluateTrajectory integrates TR_GRAVITY with a 400-unit term",
         "result[2] = result[2] - deltaTimec * 400.0 * deltaTimec;" in bg_misc),
        ("BG_EvaluateTrajectory scales time by EQUAL_EPSILON",
         "deltaTimec = (double)(atTime - tr->trTime) * EQUAL_EPSILON;" in bg_misc),
    ]
    for name, value in expected.items():
        checks.append((f"harness constant {name} matches the shipped header",
                       constants.get(name) == value and declared.get(name) == value))

    passed = sum(1 for _, ok in checks if ok)
    for name, ok in checks:
        if not ok:
            print(f"FAIL [source] {name}")
    print(f"{passed}/{len(checks)} source checks passed")
    return 0 if passed == len(checks) else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("--g-utils", type=Path, default=root / "src/game_mp/g_utils_mp.cpp")
    parser.add_argument("--bg-misc", type=Path, default=root / "src/bgame/bg_misc.cpp")
    parser.add_argument("--g-combat", type=Path, default=root / "src/game_mp/g_combat_mp.cpp")
    parser.add_argument("--g-items", type=Path, default=root / "src/game/g_items.cpp")
    parser.add_argument("--arch", choices=("32", "64", "both"), default="both",
                        help="production word size is 32; 64 is checked for the arithmetic itself")
    parser.add_argument("--sanitize", action="store_true",
                        help="also run the 64-bit build under ASan/UBSan")
    args = parser.parse_args()

    g_utils = args.g_utils.read_text()
    bg_misc = args.bg_misc.read_text()
    g_combat = args.g_combat.read_text()
    g_items = args.g_items.read_text()

    # Shipping headers, read only to verify the constants transcribed into the
    # harness rather than to compile against.
    headers = {
        "q_shared": (root / "src/universal/q_shared.h").read_text(),
        "bg_public": (root / "src/bgame/bg_public.h").read_text(),
        "g_local": (root / "src/game/g_local.h").read_text(),
        "bg_local": (root / "src/bgame/bg_local.h").read_text(),
    }

    status = source_checks(g_combat, g_items, g_utils, bg_misc, headers)
    program = build_program(g_utils, bg_misc, g_combat, g_items)

    variants = []
    if args.arch in ("32", "both"):
        variants.append(("32", []))
    if args.arch in ("64", "both"):
        variants.append(("64", []))
        if args.sanitize:
            variants.append(("64-asan", ["-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all"]))

    with tempfile.TemporaryDirectory(prefix="kisakcod-gameplay-lifecycle-") as temp:
        cpp = Path(temp) / "gameplay_lifecycle.cpp"
        cpp.write_text(program)
        for arch, extra in variants:
            executable = Path(temp) / f"gameplay_lifecycle_{arch}"
            target = arch.split("-")[0]
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

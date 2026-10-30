// TEST-ONLY implementation of the engine services src/linux/net.cpp calls.
// Behaviour mirrors qcommon/common.cpp (logging), universal/dvar.cpp (dvar
// registration keeps the first current value) and qcommon/net_chan_mp.cpp
// (NET_AdrToString formatting).  Not part of any build target.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include "shim_engine.h"

// ---------------------------------------------------------------- logging ---

static void Shim_VLog(const char *prefix, const char *fmt, va_list args)
{
    fprintf(stderr, "%s", prefix);
    vfprintf(stderr, fmt, args);
}

static int shimComErrorCount;

void Com_Printf(int, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Shim_VLog("", fmt, args);
    va_end(args);
}

void Com_DPrintf(int, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Shim_VLog("DEBUG: ", fmt, args);
    va_end(args);
}

void Com_PrintError(int, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Shim_VLog("", fmt, args);
    va_end(args);
}

void Com_PrintWarning(int, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Shim_VLog("", fmt, args);
    va_end(args);
}

void Com_Error(errorParm_t code, const char *fmt, ...)
{
    va_list args;

    shimComErrorCount++;
    va_start(args, fmt);
    fprintf(stderr, "FATAL(%d): ", (int)code);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

// The real Com_Error(ERR_FATAL) never returns.  This one does, which is why the
// tests treat "an error was reported" and "no unsafe socket exists" as two
// separate assertions and never accept the diagnostic as proof of an abort.

int Shim_ComErrorCount()
{
    return shimComErrorCount;
}

void Shim_ComErrorReset()
{
    shimComErrorCount = 0;
}

// ------------------------------------------------------------------ dvars ---

static const int SHIM_MAX_DVARS = 64;
static dvar_s shimDvars[SHIM_MAX_DVARS];
static int shimDvarCount;

// The engine keeps this pointer for the "dedicated" dvar once Com_Init has
// registered it; src/linux/net.cpp reads com_dedicated->current.integer.
const dvar_t *com_dedicated;

const dvar_t *Shim_DvarFind(const char *name)
{
    int i;

    for (i = 0; i < shimDvarCount; ++i)
    {
        if (!strcmp(shimDvars[i].name, name))
            return &shimDvars[i];
    }
    return NULL;
}

const dvar_s *Dvar_FindVar(const char *dvarName)
{
    return Shim_DvarFind(dvarName);
}

// Mirrors registration of the "dedicated" dvar: the engine-side pointer follows
// whichever dvar carries that name.
static void Shim_LinkComDedicated(const char *name, dvar_s *dvar)
{
    if (!strcmp(name, "dedicated"))
        com_dedicated = dvar;
}

void Shim_ClearComDedicated()
{
    com_dedicated = NULL;
}

static dvar_s *Shim_DvarAlloc(const char *name, uint16_t flags, const char *description)
{
    dvar_s *dvar;

    if (shimDvarCount >= SHIM_MAX_DVARS)
        return NULL;

    dvar = &shimDvars[shimDvarCount++];
    memset(dvar, 0, sizeof(*dvar));
    dvar->name = name;
    dvar->description = description;
    dvar->flags = flags;
    dvar->modified = false;
    Shim_LinkComDedicated(name, dvar);
    return dvar;
}

void Shim_DvarSeedInt(const char *name, int value)
{
    dvar_s *dvar = (dvar_s *)Shim_DvarFind(name);

    if (!dvar)
        dvar = Shim_DvarAlloc(name, DVAR_LATCH, "seeded");
    if (dvar)
    {
        dvar->current.integer = value;
        Shim_LinkComDedicated(name, dvar);
    }
}

void Shim_DvarSeedString(const char *name, const char *value)
{
    dvar_s *dvar = (dvar_s *)Shim_DvarFind(name);

    if (!dvar)
        dvar = Shim_DvarAlloc(name, DVAR_LATCH, "seeded");
    if (dvar)
    {
        dvar->current.string = value;
        Shim_LinkComDedicated(name, dvar);
    }
}

// Seeds a boolean dvar the way "+set <name> 1" on the command line does: the
// value exists before the code under test registers the dvar, and registration
// keeps it (see Dvar_RegisterBool above).
void Shim_DvarSeedBool(const char *name, bool value)
{
    dvar_s *dvar = (dvar_s *)Shim_DvarFind(name);

    if (!dvar)
        dvar = Shim_DvarAlloc(name, DVAR_LATCH, "seeded");
    if (dvar)
    {
        dvar->current.enabled = value;
        dvar->current.integer = value ? 1 : 0;
        Shim_LinkComDedicated(name, dvar);
    }
}

const dvar_s *Dvar_RegisterBool(const char *dvarName, bool value, uint16_t flags, const char *description)
{
    dvar_s *dvar = (dvar_s *)Shim_DvarFind(dvarName);

    if (!dvar)
    {
        dvar = Shim_DvarAlloc(dvarName, flags, description);
        if (dvar)
            dvar->current.enabled = value;
    }
    return dvar;
}

const dvar_s *Dvar_RegisterInt(const char *dvarName, int value, DvarLimits min, uint16_t flags, const char *description)
{
    dvar_s *dvar = (dvar_s *)Shim_DvarFind(dvarName);

    if (!dvar)
    {
        dvar = Shim_DvarAlloc(dvarName, flags, description);
        if (dvar)
        {
            dvar->current.integer = value;
            dvar->domain = min;
        }
    }
    return dvar;
}

const dvar_s *Dvar_RegisterString(const char *dvarName, const char *value, uint16_t flags, const char *description)
{
    dvar_s *dvar = (dvar_s *)Shim_DvarFind(dvarName);

    if (!dvar)
    {
        dvar = Shim_DvarAlloc(dvarName, flags, description);
        if (dvar)
            dvar->current.string = value;
    }
    return dvar;
}

void Dvar_SetInt(dvar_s *dvar, int value)
{
    if (!dvar)
        return;
    dvar->current.integer = value;
}

// --------------------------------------------------------------- helpers ----

int I_stricmp(const char *s0, const char *s1)
{
    int c0, c1;

    if (s0 == s1)
        return 0;
    if (!s0)
        return -1;
    if (!s1)
        return 1;

    do
    {
        c0 = (unsigned char)*s0++;
        c1 = (unsigned char)*s1++;
        if (c0 >= 'A' && c0 <= 'Z')
            c0 += 'a' - 'A';
        if (c1 >= 'A' && c1 <= 'Z')
            c1 += 'a' - 'A';
    } while (c0 && c0 == c1);

    return c0 - c1;
}

char *I_strncpyz_impl(char *dest, const char *src, int destsize);

void I_strncpyz(char *dest, const char *src, int destsize)
{
    I_strncpyz_impl(dest, src, destsize);
}

char *I_strncpyz_impl(char *dest, const char *src, int destsize)
{
    if (destsize <= 0)
        return dest;
    if (!src)
        src = "";
    strncpy(dest, src, (size_t)destsize - 1);
    dest[destsize - 1] = '\0';
    return dest;
}

const char *NET_AdrToString(netadr_t a)
{
    static char s[64];

    if (a.type == NA_LOOPBACK)
        snprintf(s, sizeof(s), "loopback");
    else if (a.type == NA_BROADCAST)
        snprintf(s, sizeof(s), "broadcast");
    else if (a.type == NA_IP)
        snprintf(s, sizeof(s), "%i.%i.%i.%i:%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3], ntohs(a.port));
    else
        snprintf(s, sizeof(s), "bad");

    return s;
}

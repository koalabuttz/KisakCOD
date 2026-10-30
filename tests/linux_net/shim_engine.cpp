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
    va_start(args, fmt);
    fprintf(stderr, "FATAL(%d): ", (int)code);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

// ------------------------------------------------------------------ dvars ---

static const int SHIM_MAX_DVARS = 64;
static dvar_s shimDvars[SHIM_MAX_DVARS];
static int shimDvarCount;

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
    return dvar;
}

void Shim_DvarSeedInt(const char *name, int value)
{
    dvar_s *dvar = (dvar_s *)Shim_DvarFind(name);

    if (!dvar)
        dvar = Shim_DvarAlloc(name, DVAR_LATCH, "seeded");
    if (dvar)
        dvar->current.integer = value;
}

void Shim_DvarSeedString(const char *name, const char *value)
{
    dvar_s *dvar = (dvar_s *)Shim_DvarFind(name);

    if (!dvar)
        dvar = Shim_DvarAlloc(name, DVAR_LATCH, "seeded");
    if (dvar)
        dvar->current.string = value;
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

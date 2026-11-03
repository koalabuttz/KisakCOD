#pragma once

#include <qcommon/qcommon.h>
#include <qcommon/ent.h>

//#ifdef KISAK_MP
//#include <server_mp/server_mp.h>
//#elif KISAK_SP 
//#include "server.h"
//#endif

struct gentity_s;
struct IgnoreEntParams;
struct sightpointtrace_t;

uint32_t SV_ClipHandleForEntity(const gentity_s *ent);
void SV_UnlinkEntity(gentity_s *gEnt);
void SV_LinkEntity(gentity_s *gEnt);
void SnapAngles(float *vAngles);
void SV_ClipMoveToEntity(const moveclip_t *clip, svEntity_s *check, trace_t *trace);
void SV_PointTraceToEntity(const pointtrace_t *clip, svEntity_s *check, trace_t *trace);
DObj_s *SV_LocationalTraceDObj(const pointtrace_t *clip, const gentity_s *touch);
int SV_ClipSightToEntity(const sightclip_t *clip, svEntity_s *check);
int SV_PointSightTraceToEntity(const sightpointtrace_t *clip, svEntity_s *check);
DObj_s *SV_LocationalSightTraceDObj(const sightpointtrace_t *clip, const gentity_s *touch);
void SV_SetupIgnoreEntParams(IgnoreEntParams *ignoreEntParams, int baseEntity);
void SV_Trace(
    trace_t *results,
    const float *start,
    const float *mins,
    const float *maxs,
    const float *end,
    const IgnoreEntParams *ignoreEntParams,
    int contentmask,
    int locational,
    uint8_t *priorityMap,
    int staticmodels);
// Capsule trace honoring every IgnoreEntParams flag, including the parent.
void SV_TraceCapsuleWithIgnore(
    trace_t *results,
    const float *start,
    const float *mins,
    const float *maxs,
    const float *end,
    const IgnoreEntParams *ignoreEntParams,
    int contentmask);
int SV_TracePassed(
    const float *start,
    const float *mins,
    const float *maxs,
    const float *end,
    int passEntityNum0,
    int passEntityNum1,
    int contentmask,
    int locational,
    uint8_t *priorityMap,
    int staticmodels);
void SV_SightTrace(
    int *hitNum,
    const float *start,
    const float *mins,
    const float *maxs,
    const float *end,
    int passEntityNum0,
    int passEntityNum1,
    int contentmask);
int SV_SightTraceToEntity(float *start, float *mins, float *maxs, float *end, int entityNum, int contentmask);
int SV_PointContents(float *p, int passEntityNum, int contentmask);

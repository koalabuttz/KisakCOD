#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include <universal/q_shared.h>
#include "pathnode.h"
#include "actor_badplace.h"
#include "g_main.h"
#include <script/scr_vm.h>
#include "actor_corpse.h"
#include "actor_events.h"
#include "game_public.h"
#include "g_local.h"
#include <server/sv_game.h>
#include "actor_cover.h"
#include "actor_orientation.h"
#include "actor_team_move.h"
#include "actor_state.h"

#include <algorithm>

// Line 38954:  0006 : 005a27c8       struct badplace_t *g_badplaces 82c327c8     actor_badplace.obj

badplace_t g_badplaces[32];

void TRACK_actor_badplace()
{
    track_static_alloc_internal(g_badplaces, 1280, "g_badplaces", 5);
}

void Path_UpdateBadPlaceCount(badplace_t *place, int delta)
{
    int type; // r4
    const char *v5; // r3

    if (!place)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 49, 0, "%s", "place");
    if (delta != -1 && delta != 1)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp",
            50,
            0,
            "%s\n\t(delta) = %i",
            "(delta == -1 || delta == +1)",
            delta);
    type = place->type;
    if (type == 1)
    {
        Path_UpdateArcBadPlaceCount(&place->parms.arc, place->teamflags, delta);
    }
    else if (type == 2)
    {
        Path_UpdateBrushBadPlaceCount(place->parms.brush.volume, place->teamflags, delta);
    }
    else if (!alwaysfails)
    {
        v5 = va("unhandled bad place type %i", type);
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 63, 0, v5);
    }
}

void Path_FreeBadPlace(int index)
{
    int v1; // r30

    if (index >= 0)
    {
        v1 = index;
        Path_UpdateBadPlaceCount(&g_badplaces[index], -1);
        g_badplaces[v1].type = 0;
        Scr_SetString(&g_badplaces[v1].name, 0);
    }
}

int Path_FindBadPlace(unsigned int name)
{
    int v1; // r10

    // KISAKFIX: IDA SP used `&playerEyePos[1]` as end-of-array sentinel — a magic-
    // address artifact that only worked because g_badplaces+sizeof was at that VA on
    // PPC. On x86 playerEyePos and g_badplaces are at unrelated addresses. Use count.
    // Indexed rather than stepping a pointer by the 40-byte i386 badplace_t.
    for (v1 = 0; v1 < 32; ++v1)
    {
        if (g_badplaces[v1].name == name)
            return v1;
    }
    return -1;
}

badplace_t *Path_AllocBadPlace(unsigned int name, int duration)
{
    int v7; // r29
    badplace_t *result; // r3
    int v10; // r11
    const char *v12; // r3
    int v13; // r31

    if (name)
    {
        // KISAKFIX: magic-address sentinel — see Path_FindBadPlace.
        Path_FreeBadPlace(Path_FindBadPlace(name));
    }
    v7 = level.time + duration;
    if (duration > 0)
    {
        if (duration < 250)
        {
            Com_PrintWarning(CON_CHANNEL_AI,
                "WARNING: A badplace was created with duration [%.2f second], which is less than the ping time [%.2f second]\n",
                duration * 0.001f, 0.25);
        }
    }
    else
    {
        if (!name)
        {
            Scr_Error("anonymous bad places must have a duration");
            return 0;
        }
        v7 = 0x7FFFFFFF;
    }
    // KISAKFIX: magic-address sentinel — see Path_FindBadPlace. Indexed rather
    // than stepping by the 40-byte i386 badplace_t.
    for (v10 = 0; g_badplaces[v10].type; )
    {
        if (++v10 >= 32)
        {
            v12 = va("too many bad places (more than %i)", 32);
            Scr_Error(v12);
            return 0;
        }
    }
    v13 = v10;
    Scr_SetString(&g_badplaces[v10].name, name);
    result = &g_badplaces[v13];
    g_badplaces[v13].endtime = v7;
    return result;
}

void Path_MakeBadPlace(unsigned int name, int duration, int teamflags, int type, badplace_parms_t *parms)
{
    badplace_t *v10; // r3

    if (!parms)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 171, 0, "%s", "parms");
    if (teamflags <= 0 || teamflags >= 16)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp",
            172,
            0,
            "%s\n\t(teamflags) = %i",
            "(teamflags > 0 && teamflags < (1 << (sizeof( ((pathlink_t *) 0)->ubBadPlaceCount ) / (sizeof( ((pathlink_t *) 0)->"
            "ubBadPlaceCount[0] ) * (sizeof( ((pathlink_t *) 0)->ubBadPlaceCount ) != 4 || sizeof( ((pathlink_t *) 0)->ubBadPla"
            "ceCount[0] ) <= 4)))))",
            teamflags);
    if (teamflags != (unsigned __int8)teamflags)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp",
            173,
            0,
            "%s\n\t(teamflags) = %i",
            "(teamflags == (byte) teamflags)",
            teamflags);
    if (type != (unsigned __int8)type)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp",
            174,
            0,
            "%s\n\t(type) = %i",
            "(type == (byte) type)",
            type);
    v10 = Path_AllocBadPlace(name, duration);
    if (v10)
    {
        v10->teamflags = teamflags;
        v10->type = type;
        v10->parms = *parms;
        v10->pingTime = level.time;
        Path_UpdateBadPlaceCount(v10, 1);
        Actor_BadPlacesChanged();
    }
}

void Path_MakeArcBadPlace(unsigned int name, int duration, int teamflags, badplace_arc_t *arc)
{
    badplace_parms_t v11[2]; // [sp+50h] [-50h] BYREF

    if (!arc)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 199, 0, "%s", "arc");
    v11[0].arc = *arc;
    Path_MakeBadPlace(name, duration, teamflags, 1, v11);
}

void Path_MakeBrushBadPlace(unsigned int name, int duration, int teamflags, gentity_s *volume)
{
    badplace_parms_t v9; // [sp+50h] [-50h] BYREF

    if (!volume)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 215, 0, "%s", "volume");
    v9.brush.volume = volume;
    volume->flags |= FL_BADPLACE_VOLUME;
    v9.brush.radius = RadiusFromBounds2D(volume->r.mins, volume->r.maxs); // arc.origin[1] on i386 only
    Path_MakeBadPlace(name, duration, teamflags, 2, &v9);
}

void Path_RemoveBadPlaceEntity(gentity_s *entity)
{
    int v3; // r11
    int v5; // r31

    if (!entity)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 231, 0, "%s", "entity");
    // The brush bad place on this entity, by index (the decompiled loop used the
    // 40-byte i386 badplace_t stride).
    for (v3 = 0; g_badplaces[v3].type != 2 || g_badplaces[v3].parms.brush.volume != entity; ++v3)
    {
        if (v3 + 1 >= (int)ARRAY_COUNT(g_badplaces))
            return;
    }
    if (v3 >= 0)
    {
        v5 = v3;
        Path_UpdateBadPlaceCount(&g_badplaces[v3], -1);
        g_badplaces[v5].type = 0;
        Scr_SetString(&g_badplaces[v5].name, 0);
    }
}

void Path_DrawBadPlace(badplace_t *place)
{
    float radius; // fp31

    float v0[3];
    float v1[3];

    float dir0[3];
    float dir1[3];

    if (place->type == 1)
    {
        if ((float)(place->parms.arc.radius * place->parms.arc.radius) == 0.0)
            radius = sqrtf(Actor_EventDefaultRadiusSqrd(AI_EV_BADPLACE_ARC));
        else
            radius = place->parms.arc.radius;

        if (place->parms.arc.angle0 == 0.0 && place->parms.arc.angle1 == 360.0)
        {
            v0[0] = place->parms.arc.origin[0];
            v0[1] = place->parms.arc.origin[1];
            v0[2] = place->parms.arc.origin[2];
            G_DebugCircle(v0, radius, colorRed, 1, 1, 0);
            v0[2] -= place->parms.arc.halfheight;
            G_DebugCircle(v0, radius, colorLtOrange, 1, 1, 0);
            v0[2] += (place->parms.arc.halfheight * 2.0f);
            G_DebugCircle(v0, radius, colorLtOrange, 1, 1, 0);
            v0[2] += (place->parms.arc.halfheight * 2.0f);
            G_DebugCircle(v0, radius, colorLtOrange, 1, 1, 0);
        }
        else
        {
            YawVectors(place->parms.arc.angle0, dir0, 0);
            YawVectors(place->parms.arc.angle1, dir1, 0);
            //YawVectors(place->parms.arc.angle0, (float *)&place->endtime, &v28);
            //YawVectors(place->parms.arc.angle1, v9, &v31);

            v0[0] = place->parms.arc.origin[0];
            v0[1] = place->parms.arc.origin[1];
            v0[2] = place->parms.arc.origin[2] - place->parms.arc.halfheight;
            G_DebugArc(v0, radius, place->parms.arc.angle0, place->parms.arc.angle1, colorRed, 1, 0);

            v1[0] = (float)(radius * dir0[0]) + v0[0];
            v1[1] = (float)(radius * dir0[1]) + v0[1];
            v1[2] = (float)(radius * dir0[2]) + v0[2];
            G_DebugLine(v0, v1, colorRed, 1);

            v1[0] = (float)(radius * dir1[0]) + v0[0];
            v1[1] = (float)(radius * dir1[1]) + v0[1];
            v1[2] = (float)(radius * dir1[2]) + v0[2];
            G_DebugLine(v0, v1, colorRed, 1);

            v0[2] = (float)(place->parms.arc.halfheight * 2.0) + v0[2];
            v1[0] = (float)(radius * dir0[0]) + v0[0];
            v1[1] = (float)(radius * dir0[1]) + v0[1];
            v1[2] = (float)(radius * dir0[2]) + v0[2];
            G_DebugArc(v0, radius, place->parms.arc.angle0, place->parms.arc.angle1, colorRed, 1, 0);

            v1[0] = (float)(radius * dir0[0]) + v0[0];
            v1[1] = (float)(radius * dir0[1]) + v0[1];
            v1[2] = (float)(radius * dir0[2]) + v0[2];
            G_DebugLine(v0, v1, colorRed, 1);

            v1[0] = (float)(radius * dir1[0]) + v0[0];
            v1[1] = (float)(radius * dir1[1]) + v0[1];
            v1[2] = (float)(radius * dir1[2]) + v0[2];
            G_DebugLine(v0, v1, colorRed, 1);
        }
    }
    else if (place->type == 2)
    {
        G_DebugDrawBrushModel(place->parms.brush.volume, colorRed, 1, 0);
    }
    else if (!alwaysfails)
    {
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 304, 0, va("unhandled bad place type %i", place->type));
    }
}

void Path_InitBadPlaces()
{
    memset(g_badplaces, 0, sizeof(g_badplaces));
}

void Path_ShutdownBadPlaces()
{
    // Retail stepped &g_badplaces[i].name by 20 shorts (40-byte i386 badplace_t);
    // on x64 badplace_brush_t holds an 8-byte pointer, so index instead.
    static_assert(LAYOUT_32BIT(sizeof(badplace_t) == 40));
    for (unsigned int i = 0; i < ARRAY_COUNT(g_badplaces); ++i)
        Scr_SetString(&g_badplaces[i].name, 0);
    memset(g_badplaces, 0, sizeof(g_badplaces));
}

void Actor_Badplace_Ping(actor_s *self)
{
    if (!self)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 398, 0, "%s", "self");
    if (!self->flashBanged)
    {
        int randVal = G_rand();
        if ((float)randVal <= self->badPlaceAwareness * 32767.0f)
            self->isInBadPlace = 1;
    }
}

int Actor_IsInAnyBadPlace(actor_s *self)
{
    // Walks g_badplaces by index. (The decompiled loop stepped a float pointer
    // through the 40-byte i386 badplace_t and read fields at i386 offsets.)
    for (unsigned int index = 0; index < ARRAY_COUNT(g_badplaces); ++index)
    {
        const badplace_t *place = &g_badplaces[index];
        bool inside;
        if (!place->type)
            continue;
        if (place->type == 1)
        {
            inside = Actor_IsInsideArc(
                self, place->parms.arc.origin, place->parms.arc.radius, place->parms.arc.angle0,
                place->parms.arc.angle1, place->parms.arc.halfheight);
        }
        else if (place->type == 2)
        {
            inside = SV_EntityContact(self->ent->r.mins, self->ent->r.maxs, place->parms.brush.volume) != 0;
        }
        else
        {
            if (!alwaysfails)
                MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 439, 0, va("unhandled bad place type %i", place->type));
            continue;
        }
        if (inside)
            return 1;
    }
    return 0;
}

actor_s *Actor_BadPlace_UpdateFleeingActors()
{
    actor_s *result; // r3
    actor_s *i; // r31

    result = Actor_FirstActor(-1);
    for (i = result; result; i = result)
    {
        if (i->eState[i->stateLevel] == AIS_BADPLACE_FLEE && !(unsigned __int8)Actor_IsInAnyBadPlace(i))
            i->isInBadPlace = 0;
        result = Actor_NextActor(i, -1);
    }
    return result;
}

float Actor_BadPlace_GetMaximumFleeRadius()
{
    int v0; // r28
    double v2; // fp31
    int v3; // r4
    const char *v4; // r3
    double v5; // fp0
    double v6; // fp1

    // Retail stepped &g_badplaces[i].type by 40 bytes and read the arc radius at
    // type+14 and the brush radius at type+6 (i386 offsets); use the fields.
    static_assert(LAYOUT_32BIT(offsetof(badplace_t, parms.arc.radius) - offsetof(badplace_t, type) == 14));
    static_assert(LAYOUT_32BIT(offsetof(badplace_t, parms.brush.radius) - offsetof(badplace_t, type) == 6));
    v2 = -1.0;
    for (v0 = 0; v0 < (int)ARRAY_COUNT(g_badplaces); ++v0)
    {
        const badplace_t *place = &g_badplaces[v0];

        v3 = place->type;
        if (!v3)
            continue;
        if (v3 == 1)
        {
            v5 = place->parms.arc.radius;
        }
        else
        {
            if (v3 != 2)
            {
                if (!alwaysfails)
                {
                    v4 = va("unhandled bad place type %i", v3);
                    MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 500, 0, v4);
                }
                continue;
            }
            v5 = place->parms.brush.radius;
        }
        if (v5 > v2)
            v2 = v5;
    }
    v6 = v2;
    return (float)v6;
}

int Actor_BadPlace_HasPotentialNodeDuplicates(
    pathsort_t *potentialNodes,
    int potentialNodeCount,
    pathnode_t *checkNode)
{
    int v6; // r10
    pathsort_t *i; // r11

    if (!potentialNodes)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 517, 0, "%s", "potentialNodes");
    if (!checkNode)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 518, 0, "%s", "checkNode");
    v6 = 0;
    if (potentialNodeCount <= 0)
        return 0;
    for (i = potentialNodes; i->node != checkNode; ++i)
    {
        if (++v6 >= potentialNodeCount)
            return 0;
    }
    return 1;
}

int Actor_BadPlace_IsNodeInAnyBadPlace(pathnode_t *node)
{
    if (!node)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 542, 0, "%s", "node");
    // Walks g_badplaces by index (see Actor_IsInAnyBadPlace).
    for (unsigned int index = 0; index < ARRAY_COUNT(g_badplaces); ++index)
    {
        const badplace_t *place = &g_badplaces[index];
        int inside;
        if (!place->type)
            continue;
        if (place->type == 1)
        {
            inside = Path_IsNodeInArc(
                node, place->parms.arc.origin, place->parms.arc.radius, place->parms.arc.angle0,
                place->parms.arc.angle1, place->parms.arc.halfheight);
        }
        else if (place->type == 2)
        {
            inside = SV_EntityContact(node->constant.vOrigin, node->constant.vOrigin, place->parms.brush.volume);
        }
        else
        {
            if (!alwaysfails)
                MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 563, 0, va("unhandled bad place type %i", place->type));
            continue;
        }
        if (inside)
            return 1;
    }
    return 0;
}

pathnode_t *Actor_BadPlace_FindSafeNodeAlongPath(actor_s *self)
{
    int v2; // r30
    int *i; // r29
    pathnode_t *v4; // r3
    const pathnode_t *v5; // r31

    if (!self)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 641, 0, "%s", "self");
    if (!Actor_HasPath(self))
        return 0;
    v2 = self->Path.lookaheadNextNode + 1;
    if (v2 < 0)
        return 0;
    for (i = &self->Path.pts[v2].iNodeNum; ; i -= 7)
    {
        if (*i > 0)
        {
            v4 = Path_ConvertIndexToNode(*i);
            v5 = v4;
            if (v4)
            {
                if (!(unsigned __int8)Actor_BadPlace_IsNodeInAnyBadPlace(v4) && Path_CanClaimNode(v5, self->sentient))
                    break;
            }
        }
        if (--v2 < 0)
            return 0;
    }
    return (pathnode_t *)v5;
}

void Actor_BadPlace_Flee_Finish(actor_s *self, ai_state_t eNextState)
{
    ;
}

void Path_RemoveBadPlace(unsigned int name)
{
    int v1; // r10

    // Walks g_badplaces by index (retail stepped name by 20 shorts, the 40-byte
    // i386 badplace_t). Falls back to -1 as the KISAKFIX sentinel did.
    v1 = 0;
    while (g_badplaces[v1].name != name)
    {
        ++v1;
        // KISAKFIX: magic-address sentinel — see Path_FindBadPlace.
        if (v1 >= 32)
        {
            v1 = -1;
            break;
        }
    }
    Path_FreeBadPlace(v1);
    Actor_BadPlace_UpdateFleeingActors();
}

void Path_RunBadPlaces()
{
    bool removed = false;

    // Walks g_badplaces by index. (The decompiled loop stepped a float pointer
    // through the 40-byte i386 badplace_t and read fields at i386 offsets.)
    for (unsigned int index = 0; index < ARRAY_COUNT(g_badplaces); ++index)
    {
        badplace_t *place = &g_badplaces[index];
        if (!place->type)
            continue;
        if (level.time < place->endtime)
        {
            if (level.time - place->pingTime >= 250)
            {
                if (place->type == 1)
                {
                    Actor_BroadcastArcEvent(
                        0,
                        AI_EV_BADPLACE_ARC,
                        place->teamflags,
                        place->parms.arc.origin,
                        place->parms.arc.radius,
                        place->parms.arc.angle0,
                        place->parms.arc.angle1,
                        place->parms.arc.halfheight);
                }
                else if (place->type == 2)
                {
                    Actor_BroadcastVolumeEvent(
                        0,
                        AI_EV_BADPLACE_VOLUME,
                        place->teamflags,
                        place->parms.brush.volume,
                        place->parms.brush.radius);
                }
                else if (!alwaysfails)
                {
                    MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 374, 0, va("unhandled bad place type %i", place->type));
                }
                place->pingTime = level.time;
            }
            if (ai_showBadPlaces->current.enabled)
                Path_DrawBadPlace(place);
        }
        else
        {
            Path_UpdateBadPlaceCount(place, -1);
            place->type = 0;
            Scr_SetString(&place->name, 0);
            removed = true;
        }
    }
    if (removed)
        Actor_BadPlace_UpdateFleeingActors();
}

int Actor_BadPlace_FindSafeNodeOutsideBadPlace(
    actor_s *self,
    pathsort_t *potentialNodes,
    double maxFleeDist)
{
    int nodeCount;
    int nodesWritten; // r30
    pathsort_t *pOutNode; // r29
    pathsort_t *pNode; // r27
    pathnode_t *node; // r31
    double v14; // fp0
    double v15; // fp13
    double v16; // fp12
    double v17; // fp31
    pathsort_t nodes[256];

    //Profile_Begin(357);
    iassert(self);
    iassert(self->sentient);
    iassert(potentialNodes);
    iassert(maxFleeDist > 0.0f);

    nodeCount = Path_NodesInCylinder(self->ent->r.currentOrigin, maxFleeDist, 80.0, nodes, 256, -2);

    nodesWritten = 0;

    if (nodeCount > 0)
    {
        pOutNode = potentialNodes;
        pNode = &nodes[0];
        do
        {
            node = pNode->node;

            if (!Actor_BadPlace_IsNodeInAnyBadPlace(node) && Path_CanClaimNode(node, self->sentient))
            {
                iassert(node);

                if ((((1 << node->constant.type) & 0x41FFC) == 0 || Actor_Cover_IsValidCover(self, node))
                    && !Actor_BadPlace_HasPotentialNodeDuplicates(potentialNodes, nodesWritten, node))
                {
                    v14 = (float)(node->constant.vOrigin[0] - self->ent->r.currentOrigin[0]);
                    v15 = (float)(node->constant.vOrigin[2] - self->ent->r.currentOrigin[2]);
                    v16 = (float)(node->constant.vOrigin[1] - self->ent->r.currentOrigin[1]);
                    v17 = (float)((float)((float)v16 * (float)v16) + (float)((float)((float)v15 * (float)v15) + (float)((float)v14 * (float)v14)));

                    if (Path_IsCoverNode(node))
                        v17 = (float)((float)v17 * 0.89999998);

                    pOutNode->metric = v17;
                    pOutNode->node = node;
                    ++nodesWritten;
                    ++pOutNode;
                }
            }
            --nodeCount;
            ++pNode;
        } while (nodeCount);

        if (nodesWritten > 1)
        {
            //std::_Sort<pathsort_t *, int, bool(*)(pathsort_t const &, pathsort_t const &)>(
            //    potentialNodes,
            //    &potentialNodes[v9],
            //    12 * v9 / 12,
            //    Path_CompareNodesIncreasing);
            std::sort(potentialNodes, potentialNodes + nodesWritten, Path_CompareNodesIncreasing);
        }
    }

    //Profile_EndInternal(0);
    return nodesWritten;
}

int Actor_BadPlace_AttemptEscape(actor_s *self)
{
    double MaximumFleeRadius; // fp1
    double v3; // fp31
    pathnode_t *SafeNodeAlongPath; // r3
    pathnode_t *node; // r31
    int SafeNodeOutsideBadPlace; // r27
    int v8; // r29
    pathsort_t *i; // r30
    pathsort_t v10[256]; // [sp+50h] [-C50h] BYREF

    if (!self)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 684, 0, "%s", "self");
    if (!self->sentient)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 685, 0, "%s", "self->sentient");
    MaximumFleeRadius = Actor_BadPlace_GetMaximumFleeRadius();
    if (MaximumFleeRadius >= 0.0)
    {
        v3 = (float)((float)MaximumFleeRadius + (float)100.0);
        SafeNodeAlongPath = Actor_BadPlace_FindSafeNodeAlongPath(self);
        node = SafeNodeAlongPath;
        if (SafeNodeAlongPath && Actor_FindPathToNode(self, SafeNodeAlongPath, 1))
        {
        LABEL_8:
            Actor_ClearKeepClaimedNode(self);
            Sentient_ClaimNode(self->sentient, node);
            return 1;
        }
        SafeNodeOutsideBadPlace = Actor_BadPlace_FindSafeNodeOutsideBadPlace(self, v10, v3);
        v8 = 0;
        if (SafeNodeOutsideBadPlace > 0)
        {
            for (i = v10; ; ++i)
            {
                node = i->node;
                if (!i->node)
                    MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 711, 0, "%s", "safeNode");
                if (Actor_FindPathToNode(self, node, 1))
                    break;
                if (++v8 >= SafeNodeOutsideBadPlace)
                    return 0;
            }
            goto LABEL_8;
        }
    }
    return 0;
}

bool Actor_BadPlace_Flee_Start(actor_s *self, ai_state_t ePrevState)
{
    if (!self)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\game\\actor_badplace.cpp", 732, 0, "%s", "self");
    self->isInBadPlace = Actor_BadPlace_AttemptEscape(self);
    return 1;
}

actor_think_result_t Actor_BadPlace_Flee_Think(actor_s *self)
{
    actor_s *v2; // r3

    iassert(self);
    v2 = self;
    if (self->isInBadPlace)
    {
        Actor_PreThink(self);
        self->pszDebugInfo = "badplace_flee";
        if (Actor_HasPath(self))
        {
            Actor_SetOrientMode(self, AI_ORIENT_TO_ENEMY_OR_MOTION);
            Actor_TeamMoveBlockedClear(self);
            Actor_MoveAlongPathWithTeam(self, 1, 0, 0);
            if (self->pCloseEnt.isDefined() || self->pPileUpActor)
            {
                Actor_ClearPath(self);
                Actor_PostThink(self);
                return ACTOR_THINK_DONE;
            }
            goto LABEL_13;
        }
        if ((unsigned __int8)Actor_IsInAnyBadPlace(self))
        {
            if (Actor_NearClaimNode(self, 32.0))
                Path_MarkNodeInvalid(self->sentient->pClaimedNode, self->sentient->eTeam);
            if ((unsigned __int8)Actor_BadPlace_AttemptEscape(self))
            {
                Actor_AnimStop(self, &g_animScriptTable[self->species]->stop);
            LABEL_13:
                Actor_PostThink(self);
                return ACTOR_THINK_DONE;
            }
        }
        v2 = self;
        self->isInBadPlace = 0;
    }
    Actor_SetState(v2, AIS_EXPOSED);
    return ACTOR_THINK_REPEAT;
}


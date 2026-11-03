#pragma once
#include "teams.h"
#include "sentient.h"

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

enum PredictionTraceResult : __int32
{
    PTR_SUCCESS = 0x0,
    PTR_HIT_WORLD = 0x1,
    PTR_HIT_ENTITY = 0x2,
};

struct pathpoint_t
{
    float vOrigPoint[3];
    float fDir2D[2];
    float fOrigLength;
    int iNodeNum;
};

struct path_t
{
    pathpoint_t pts[32];
    __int16 wPathLen;
    __int16 wOrigPathLen;
    __int16 wDodgeCount;
    __int16 wNegotiationStartNode;
    __int16 lookaheadNextNode;
    __int16 wDodgeEntity;
    float vFinalGoal[3];
    float lookaheadDir[3];
    float forwardLookaheadDir2D[2];
    float fLookaheadDist;
    float fLookaheadAmount;
    float fLookaheadDistToNextNode;
    int minLookAheadNodes;
    int flags;
    int iPathTime;
    team_t eTeam;
    float fCurrLength;
    float vCurrPoint[3];
    int iPathEndTime;
    float pathEndAnimDistSq;
    int pathEndAnimNotified;
};

struct path_trim_t
{
    int iIndex;
    int iDelta;
};

struct actor_goal_s
{
    float pos[3];
    float radius;
    float height;
    pathnode_t *node;
    gentity_s *volume;
};

struct actor_s;
struct pathnode_t;
enum ai_stance_e : __int32;

bool Path_IsPathStanceNode(const pathnode_t *node);
float Path_GetPathDir(float *delta, const float *vFrom, const float *vTo);
pathnode_t *Path_GetNegotiationNode(const path_t *pPath);
void Path_IncrementNodeUserCount(path_t *pPath);
void Path_DecrementNodeUserCount(path_t *pPath);
void Path_Backup(const path_t *path);
float Path_GetDistToPathSegment(const float *vStartPos, const pathpoint_t *pt);
void Path_AddTrimmedAmount(path_t *pPath, const float *vStartPos);
void Path_SubtractTrimmedAmount(path_t *pPath, const float *vStartPos);
void Path_BeginTrim(path_t *pPath, path_trim_t *pTrim);
void Path_Begin(path_t *pPath);
void Path_Clear(path_t *pPath);
bool Path_Exists(const path_t *pPath);
int Path_CompleteLookahead(const path_t *pPath);
unsigned int Path_AttemptedCompleteLookahead(const path_t *pPath);
bool Path_UsesObstacleNegotiation(const path_t *pPath);
bool Path_HasNegotiationNode(const path_t *path);
bool Path_AtEndOrNegotiation(const path_t *pPath);
unsigned int Path_AllowsObstacleNegotiation(const path_t *pPath);
void Path_GetObstacleNegotiationScript(const path_t *pPath, struct scr_animscript_t *animscript);
int Path_NeedsReevaluation(const path_t *pPath);
int Path_EncroachesPoint2D(path_t *pPath, const float *vStart, const float *vPoint, float fMinDistSqrd);
int Path_DistanceGreaterThan(path_t *pPath, float fDist);
void Path_ReduceLookaheadAmount(path_t *pPath, double maxLookaheadAmountIfReduce);
bool Path_FailedLookahead(path_t *pPath);
void Path_IncreaseLookaheadAmount(path_t *pPath);
bool Path_PredictionTrace(
    float *vStartPos,
    float *vEndPos,
    int entityIgnore,
    int mask,
    float *vTraceEndPos,
    double stepheight,
    int allowStartSolid);
int Path_IsTrimmed(path_t *pPath);
void Path_RemoveCompletedPathPoints(path_t *pPath, __int16 pathPointIndex);
void Path_TrimCompletedPath(path_t *pPath, const float *vStartPos);
void Path_BacktrackCompletedPath(path_t *pPath, const float *vStartPos);
void PathCalcLookahead_CheckMinLookaheadNodes(path_t *pPath, const pathpoint_t *pt, int currentNode);
int Path_GetForwardStartPos(path_t *pPath, const float *vStartPos, float *vForwardStartPos);
void Path_UpdateForwardLookahead_IncompletePath(
    path_t *pPath,
    const pathpoint_t *pt,
    const float *vForwardStartPos,
    double area,
    double height);
void Path_UpdateForwardLookahead(path_t *pPath, const float *vStartPos);
void Path_DebugDraw(path_t *pPath, float *vStartPos, int bDrawLookahead);
bool Path_WithinApproxDist(path_t *pPath, double checkDist);
ai_stance_e Path_AllowedStancesForPath(path_t *pPath);
void Path_DodgeDrawRaisedLine(float *start, float *end, const float *color);
int Path_MayFaceEnemy(path_t *pPath, float *vEnemyDir, float *vOrg);
void Path_Restore(path_t *path);
bool Path_FindPathFromTo(
    path_t *pPath,
    team_t eTeam,
    pathnode_t *pNodeFrom,
    const float *vStartPos,
    pathnode_t *pNodeTo,
    const float *vGoalPos,
    bool bAllowNegotiationLinks);
void Path_TrimLastNodes(path_t *pPath, const int iNodeCount, bool bMaintainGoalPos);
int Path_ClipToGoal(path_t *pPath, const actor_goal_s *goal);
int Path_TrimToSeePoint(
    path_t *pPath,
    path_trim_t *pTrim,
    actor_s *pActor,
    float fMaxDistSqrd,
    int iIgnoreEntityNum,
    const float *vPoint);
PredictionTraceResult Path_PredictionTraceCheckForEntities(
    float *vStartPos,
    float *vEndPos,
    int *entities,
    int entityCount,
    int entityIgnore,
    int mask,
    float *vTraceEndPos);
bool Path_LookaheadPredictionTrace(path_t *pPath, float *vStartPos, float *vEndPos);
void Path_UpdateLookaheadAmount(
    path_t *pPath,
    float *vStartPos,
    float *vLookaheadPos,
    int bReduceLookaheadAmount,
    float dist,
    int lookaheadNextNode,
    float maxLookaheadAmountIfReduce,
    bool bAllowRestore); // USEBETTERLOOKAHEAD
void Path_CalcLookahead_Completed(
    path_t *pPath,
    float *vStartPos,
    int bReduceLookaheadAmount,
    float totalArea,
    bool bAllowRestore); // USEBETTERLOOKAHEAD
void Path_CalcLookahead(path_t *pPath, float *vStartPos, int bReduceLookaheadAmount, bool bAllowRestore);
void Path_CheckNodeCountForDodge(path_t *pPath, int numNeeded, pathpoint_t **pt, int *startIndex);
void Path_TrimToBadPlaceLink(path_t *pPath, team_t eTeam);
int Path_FindPath(
    path_t *pPath,
    team_t eTeam,
    const float *vStartPos,
    float *vGoalPos,
    bool bAllowNegotiationLinks);
pathnode_t *Path_FindPathFrom(
    path_t *pPath,
    team_t eTeam,
    pathnode_t *pNodeFrom,
    float *vStartPos,
    const float *vGoalPos,
    float fMaxNodeDist,
    bool bAllowNegotiationLinks);
void Path_UpdateLookahead(
    path_t *pPath,
    float *vStartPos,
    bool bReduceLookaheadAmount,
    bool bTrimAmount,
    bool bAllowBacktrack,
    bool bAllowRestore); // USEBETTERLOOKAHEAD
void Path_SetLookaheadToStart(path_t *pPath, float *vStartPos, int bTrimAmount);
void Path_TransferLookahead(path_t *pPath, float *vStartPos);
int Path_GeneratePath(
    path_t *pPath,
    team_t eTeam,
    float *vStartPos,
    float *vGoalPos,
    pathnode_t *pNodeFrom,
    pathnode_t *pNodeTo,
    bool bIncludeGoalPos,
    bool bAllowNegotiationLinks);
void Path_UpdateLookahead_NonCodeMove(path_t *pPath, const float *vPrevPos, float *vStartPos);
int Path_AttemptDodge(
    path_t *pPath,
    float *vOrg,
    float *vDodgeStart,
    float *vDodgeEnd,
    int startIndex,
    int *entities,
    int entityCount,
    int entityIgnore,
    int mask,
    int bCheckLookahead);
pathnode_t *Path_FindCloseNode(
    team_t eTeam,
    pathnode_t *pNodeFrom,
    const float *vGoalPos,
    bool bAllowNegotiationLinks);
bool Path_FindPathFromToWithWidth(
    path_t *pPath,
    team_t eTeam,
    pathnode_t *pNodeFrom,
    const float *vStartPos,
    pathnode_t *pNodeTo,
    const float *vGoalPos,
    bool bAllowNegotiationLinks,
    double width,
    float *perp);
bool Path_FindPathFromToNotCrossPlanes(
    path_t *pPath,
    team_t eTeam,
    pathnode_t *pNodeFrom,
    const float *vStartPos,
    pathnode_t *pNodeTo,
    const float *vGoalPos,
    float (*vNormal)[2],
    float *fDist,
    int iPlaneCount,
    bool bAllowNegotiationLinks);
bool Path_FindPathFromAway(
    path_t *pPath,
    team_t eTeam,
    pathnode_t *pNodeFrom,
    float *vStartPos,
    float *vAwayFromPos,
    float fDistAway,
    bool bAllowNegotiationLinks);
bool Path_FindPathFromAwayNotCrossPlanes(
    path_t *pPath,
    team_t eTeam,
    pathnode_t *pNodeFrom,
    float *vStartPos,
    float *vAwayFromPos,
    float fDistAway,
    float (*vNormal)[2],
    float *fDist,
    int iPlaneCount,
    bool bAllowNegotiationLinks);
bool Path_FindPathInCylinderWithLOS(
    path_t *pPath,
    team_t eTeam,
    const float *vStartPos,
    float *vGoalPos,
    const actor_goal_s *goal,
    float fWithinDistSqrd,
    bool bAllowNegotiationLinks);
bool Path_FindPathInCylinderWithLOSNotCrossPlanes(
    path_t *pPath,
    team_t eTeam,
    const float *vStartPos,
    float *vGoalPos,
    const actor_goal_s *goal,
    float fWithinDistSqrd,
    float (*vNormal)[2],
    float *fDist,
    int iPlaneCount,
    bool bAllowNegotiationLinks);
bool Path_FindPathFromInCylinder(
    path_t *pPath,
    team_t eTeam,
    pathnode_t *pNodeFrom,
    float *vStartPos,
    const float *vGoalPos,
    float *vOrigin,
    float fRadiusSqrd,
    float fHalfHeightSqrd,
    bool bAllowNegotiationLinks);
int Path_FindPathFromInCylinderNotCrossPlanes(
    path_t *pPath,
    team_t eTeam,
    pathnode_t *pNodeFrom,
    const float *vStartPos,
    const float *vGoalPos,
    float *vOrigin,
    float fRadiusSqrd,
    float fHalfHeightSqrd,
    float (*vNormal)[2],
    float *fDist,
    int iPlaneCount,
    bool bAllowNegotiationLinks);
const pathnode_t *Path_FindFacingNode(sentient_s *pSelf, sentient_s *pOther, struct sentient_info_t *pInfo);
bool Path_FindPathGetCloseAsPossible(
    path_t *pPath,
    team_t eTeam,
    pathnode_t *pNodeFrom,
    float *vStartPos,
    pathnode_t *pNodeTo,
    const float *vGoalPos,
    bool bAllowNegotiationLinks);
bool Path_FindPathWithWidth(
    path_t *pPath,
    team_t eTeam,
    const float *vStartPos,
    float *vGoalPos,
    bool bAllowNegotiationLinks,
    double width,
    float *perp);
bool Path_FindPathNotCrossPlanes(
    path_t *pPath,
    team_t eTeam,
    const float *vStartPos,
    float *vGoalPos,
    float (*vNormal)[2],
    float *fDist,
    int iPlaneCount,
    bool bAllowNegotiationLinks);
bool Path_FindPathFromNotCrossPlanes(
    path_t *pPath,
    team_t eTeam,
    pathnode_t *pNodeFrom,
    const float *vStartPos,
    const float *vGoalPos,
    float (*vNormal)[2],
    float *fDist,
    int iPlaneCount,
    bool bAllowNegotiationLinks);
pathnode_t *Path_FindPathAway(
    path_t *pPath,
    team_t eTeam,
    float *vStartPos,
    float *vAwayFromPos,
    float fDistAway,
    bool bAllowNegotiationLinks);
pathnode_t *Path_FindPathAwayNotCrossPlanes(
    path_t *pPath,
    team_t eTeam,
    float *vStartPos,
    float *vAwayFromPos,
    float fDistAway,
    float (*vNormal)[2],
    float *fDist,
    int iPlaneCount,
    bool bAllowNegotiationLinks);

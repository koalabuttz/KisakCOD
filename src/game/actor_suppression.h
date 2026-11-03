#pragma once

#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

struct actor_s;
struct sentient_s;

struct ai_suppression_t
{
    int iTime;
    sentient_s *pSuppressor;
    float clipPlane[3];
    int movementOnly;
};

void DebugDrawSuppression(actor_s *self);
int Actor_PickNewSuppressantEntry(actor_s *self, sentient_s *pSuppressor);
int Actor_NearCoverNode(actor_s *self);
void Actor_BulletWhizbyNotify(actor_s *self, sentient_s *pSuppressor);
void Actor_AddSuppressionLine(
    actor_s *self,
    sentient_s *pSuppressor,
    const float *vStart,
    const float *vEnd);
void Actor_ClearSuppressant(ai_suppression_t *suppressant);
void Actor_DecaySuppressionLines(actor_s *self);
void Actor_DissociateSuppressor(actor_s *self, sentient_s *pSuppressor);
int Actor_IsSuppressedInAnyway(actor_s *self);
bool Actor_IsSuppressed(actor_s *self);
int Actor_IsMoveSuppressed(actor_s *self);
int Actor_IsSuppressionWaiting(actor_s *self);
int Actor_GetSuppressionPlanes(actor_s *self, float (*vNormalOut)[2], float *fDistOut);
int Actor_GetMoveOnlySuppressionPlanes(actor_s *self, float (*vNormalOut)[2], float *fDistOut);
void Actor_ClearAllSuppressionFromEnemySentient(sentient_s *pSuppressor);

#pragma once
#include "r_gfx.h"


void TRACK_rb_showcollision();
void RB_ShowCollision(const GfxViewParms *viewParms);
void BuildFrustumPlanes(const GfxViewParms *viewParms, cplane_s *frustumPlanes);
void RB_DrawCollisionPoly(int numPoints, float (*points)[3], const float *colorFloat);
void RB_SetPolyVert(float *xyz, GfxColor color, int tessVertIndex);
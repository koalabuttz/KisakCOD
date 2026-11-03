#include <universal/q_shared.h>
#include "cg_local.h"
#include "cg_public.h"

#include <bgame/bg_local.h>

void CG_UsedDObjCalcPose(cpose_t *pose)
{
    iassert(pose);
    InterlockedCompareExchange((volatile uint32_t*)&pose->cullIn, 1, 0);
}

void CG_CullIn(cpose_t *pose)
{
    iassert(pose);
    pose->cullIn = 2;
}


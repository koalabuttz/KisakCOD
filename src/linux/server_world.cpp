#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/com_bsp.h>

const ComPrimaryLight* Com_GetPrimaryLight(uint32_t primaryLightIndex)
{
    iassert(primaryLightIndex < comWorld.primaryLightCount);
    return &comWorld.primaryLights[primaryLightIndex];
}

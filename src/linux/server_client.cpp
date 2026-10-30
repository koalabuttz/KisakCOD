#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <client_mp/client_mp.h>

// Command buffers retain a local-client/controller argument in the shared
// engine API. The dedicated server has one command-buffer context, index zero,
// and never allocates a local player or an input device.
int CL_ControllerIndexFromClientNum(int localClientNum)
{
    iassert(localClientNum == 0);
    return 0;
}

char CL_AnyLocalClientsRunning()
{
    return 0;
}

char CL_IsLocalClientActive(int)
{
    return 0;
}

int CL_GetLocalClientActiveCount()
{
    return 0;
}

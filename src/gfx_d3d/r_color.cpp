#include <universal/q_shared.h>
#include <qcommon/com_pack.h>
#include "r_utils.h"

void __cdecl R_ConvertColorToBytes(const float *colorFloat, uint32_t *colorBytes)
{
    if (colorFloat)
        Byte4PackVertexColor(colorFloat, (unsigned char*)colorBytes);
    else
        *colorBytes = -1;
}

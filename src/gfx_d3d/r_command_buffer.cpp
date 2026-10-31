#include "r_command_buffer.h"
#include <climits>

GfxCmdHeader *R_AppendCommand(GfxCmdArray &list,unsigned capacity,GfxRenderCommand command,int bytes)
{
    // Caller guarantees cmds names capacity bytes of aligned owned storage.
    if(!list.cmds || (reinterpret_cast<uintptr_t>(list.cmds)&3) || capacity>INT_MAX ||
       list.usedTotal<0 || unsigned(list.usedTotal)>capacity || (list.usedTotal&3) ||
       list.usedCritical<0 || list.usedCritical>list.usedTotal ||
       bytes<4 || bytes>65535 || (bytes&3) || unsigned(command)>=RC_COUNT) {
        list.lastCmd=nullptr;return nullptr;
    }
    int remaining = int(capacity) - list.usedTotal;
    if(command >= RC_FIRST_NONCRITICAL && list.usedCritical<0x2000)remaining -= 0x2000 - list.usedCritical;
    if(bytes > remaining) { list.lastCmd=nullptr;return nullptr; }
    auto *header=reinterpret_cast<GfxCmdHeader *>(list.cmds+list.usedTotal);
    list.usedTotal+=bytes;
    list.usedCritical+=command >= RC_FIRST_NONCRITICAL ? 0 : bytes;
    list.lastCmd=header;header->id=command;header->byteCount=bytes;
    return header;
}

void R_EmitClearScreenCommand(void *owner,GfxCommandAllocate allocate,int whichToClear,const float *color,float depth,uint8_t stencil)
{
    GfxCmdClearScreen *cmd; // [esp+Ch] [ebp-4h]

    iassert( whichToClear );
    if ((whichToClear & 0xFFFFFFF8) != 0)
        MyAssertHandler(
            ".\\r_rendercmds.cpp",
            2044,
            0,
            "%s\n\t(whichToClear) = %i",
            "((whichToClear & ~(0x00000001l | 0x00000002l | 0x00000004l)) == 0)",
            whichToClear);
    iassert( color );
    iassert( (depth >= 0.0f && depth <= 1.0f) );
    cmd = (GfxCmdClearScreen *)allocate(owner, RC_CLEAR_SCREEN, 28);
    iassert( cmd );
    cmd->whichToClear = whichToClear;
    iassert( cmd->whichToClear == whichToClear );
    cmd->stencil = stencil;
    cmd->depth = depth;
    cmd->color[0] = *color;
    cmd->color[1] = color[1];
    cmd->color[2] = color[2];
    cmd->color[3] = color[3];
}

void R_EmitEndCommand(void *owner,GfxCommandAllocate allocate)
{
    allocate(owner,RC_END_OF_LIST,4);
}

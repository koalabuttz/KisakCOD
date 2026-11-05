// Lock_Bink_textures and Unlock_Bink_textures (bink_textures.h) on the native
// D3D9 boundary, as RAD's binktextures.cpp does them on Windows with
// USE_SEPARATE_BLIT_TEXTURE: Bink decodes into system-memory textures, and the
// changed area of the new frame is copied into the draw textures with
// UpdateTexture.
//
// One difference: every frame buffer is locked writable. RAD locks all but the
// current one read-only, because Bink on Windows writes the next frame in place
// of the current one's neighbour; the shim decodes into the buffer after the
// current one too, and a read-only lock does not promise that writes land.
#include "bink_textures.h"

void Lock_Bink_textures(BINKTEXTURESET *set_textures)
{
    BINKFRAMETEXTURES *textures = set_textures->textures;
    BINKFRAMEPLANESET *planes = set_textures->bink_buffers.Frames;
    for (int i = 0; i < set_textures->bink_buffers.TotalFrames; ++i, ++textures, ++planes)
    {
        struct
        {
            LPDIRECT3DTEXTURE9 texture;
            BINKPLANE *plane;
        } const targets[] = {
            {textures->Ytexture, &planes->YPlane},
            {textures->cRtexture, &planes->cRPlane},
            {textures->cBtexture, &planes->cBPlane},
            {textures->Atexture, &planes->APlane},
        };
        for (const auto &target : targets)
        {
            target.plane->Buffer = nullptr;
            if (!target.texture)
                continue;
            D3DLOCKED_RECT locked;
            if (SUCCEEDED(target.texture->LockRect(0, &locked, nullptr, 0)))
            {
                target.plane->Buffer = locked.pBits;
                target.plane->BufferPitch = (U32)locked.Pitch;
            }
        }
    }
}

void Unlock_Bink_textures(LPDIRECT3DDEVICE9 d3d_device, BINKTEXTURESET *set_textures, HBINK Bink)
{
    BINKFRAMETEXTURES *textures = set_textures->textures;
    BINKFRAMEPLANESET *planes = set_textures->bink_buffers.Frames;
    for (int i = 0; i < set_textures->bink_buffers.TotalFrames; ++i, ++textures, ++planes)
    {
        LPDIRECT3DTEXTURE9 const all[] = {textures->Ytexture, textures->cRtexture, textures->cBtexture,
                                          textures->Atexture};
        BINKPLANE *const allPlanes[] = {&planes->YPlane, &planes->cRPlane, &planes->cBPlane, &planes->APlane};
        for (int p = 0; p < 4; ++p)
        {
            if (all[p] && allPlanes[p]->Buffer)
                all[p]->UnlockRect(0);
            allPlanes[p]->Buffer = nullptr;
        }
    }

    // Copy the frame just decoded into the textures that are drawn.
    BINKFRAMETEXTURES *src = &set_textures->textures[set_textures->bink_buffers.FrameNum];
    BINKFRAMETEXTURES *dst = &set_textures->tex_draw;
    const S32 rectCount = BinkGetRects(Bink, BINKSURFACEFAST);
    if (rectCount <= 0 || !d3d_device)
        return;
    for (S32 i = 0; i < rectCount; ++i)
    {
        const BINKRECT &brc = Bink->FrameRects[i];
        RECT rc;
        rc.left = brc.Left;
        rc.top = brc.Top;
        rc.right = brc.Left + brc.Width;
        rc.bottom = brc.Top + brc.Height;
        src->Ytexture->AddDirtyRect(&rc);
        if (src->Atexture)
            src->Atexture->AddDirtyRect(&rc);
        rc.left >>= 1;
        rc.top >>= 1;
        rc.right = (rc.right + 1) >> 1;
        rc.bottom = (rc.bottom + 1) >> 1;
        src->cRtexture->AddDirtyRect(&rc);
        src->cBtexture->AddDirtyRect(&rc);
    }
    d3d_device->UpdateTexture(src->Ytexture, dst->Ytexture);
    d3d_device->UpdateTexture(src->cRtexture, dst->cRtexture);
    d3d_device->UpdateTexture(src->cBtexture, dst->cBtexture);
    if (src->Atexture && dst->Atexture)
        d3d_device->UpdateTexture(src->Atexture, dst->Atexture);
}

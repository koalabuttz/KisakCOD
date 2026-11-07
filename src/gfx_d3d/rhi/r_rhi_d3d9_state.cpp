// The Direct3D 9 implementation of the renderer's graphics interface: state,
// shaders, constants, draws, viewport, scissor, render-target binding and clears.
//
// Every function here was moved out of a renderer unit verbatim, or is a call
// block cut out of one (renderer plan M1a, the mechanical half of the seam).
// Log strings and error messages are the retail ones, so r_logFile output does
// not change. See src/gfx_d3d/rhi/r_rhi.h.
#include <universal/q_shared.h>
#include "r_rhi.h"
#include "../r_state.h"
#include "../rb_logfile.h"
#include "../r_dvars.h"
#include "../rb_stats.h"
#include "../r_draw_bsp.h"
#include "../rb_state.h"
#include "../rb_pixelcost.h"
#include "../r_rendertarget.h"
#include "../r_utils.h"
#include "../r_reflection_probe.h"
#include "../r_shade.h"
#include "../rb_shade.h"

const uint32_t s_cullTable_30[4] = { 0u, 1u, 3u, 2u }; // idb
const uint32_t s_blendTable_30[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
const uint32_t s_blendOpTable_30[6] = { 0, 1, 2, 3, 4, 5 };
const uint32_t s_depthTestTable_30[4] = { 8, 2, 3, 4 };
const uint32_t s_stencilOpTable_30[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
const uint32_t s_stencilFuncTable_30[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

// ---- Draws and vertex input ---------------------------------------------------

void R_HW_DrawIndexedPrimitive(IDirect3DDevice9 *device, const GfxDrawPrimArgs *args, int triCount)
{
    const char *v2;
    int hr;

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, args->vertexCount, args->baseIndex, triCount )\n");
        hr = device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, args->vertexCount, args->baseIndex, triCount);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    ".\\r_state.cpp (%i) device->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, args->vertexCount, args->baseInd"
                    "ex, triCount ) failed: %s\n",
                    782,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetStreamSource(
    IDirect3DDevice9 *device,
    uint32_t streamIndex,
    IDirect3DVertexBuffer9 *vb,
    uint32_t vertexOffset,
    uint32_t vertexStride)
{
    int hr;

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetStreamSource( streamIndex, vb, vertexOffset, vertexStride )\n");
        hr = device->SetStreamSource(streamIndex, vb, vertexOffset, vertexStride);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_state.h (%i) device->SetStreamSource( streamIndex, vb, vertexOffset, vertexSt"
                    "ride ) failed: %s\n",
                    638,
                    R_ErrorDescription(hr));
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetIndices(IDirect3DDevice9 *device, IDirect3DIndexBuffer9 *ib)
{
    const char *v2;
    int hr;

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetIndices( ib )\n");
        //hr = ((int(__thiscall *)(IDirect3DDevice9 *, IDirect3DDevice9 *, IDirect3DIndexBuffer9 *))device->SetIndices)(
        //    device,
        //    device,
        //    ib);
        hr = device->SetIndices(ib);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_state.h (%i) device->SetIndices( ib ) failed: %s\n",
                    616,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetVertexDeclaration(IDirect3DDevice9 *device, IDirect3DVertexDeclaration9 *decl)
{
    int hr;

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetVertexDeclaration( decl )\n");

        hr = device->SetVertexDeclaration(decl);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_state.h (%i) device->SetVertexDeclaration( decl ) failed: %s\n",
                    674,
                    R_ErrorDescription(hr));
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

// ---- Scissor -------------------------------------------------------------------

void R_HW_EnableScissor(IDirect3DDevice9 *device, const tagRECT *rect)
{
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, 1);
    device->SetScissorRect(rect);
}

void R_HW_DisableScissor(IDirect3DDevice9 *device)
{
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, 0);
}

// ---- Fixed state, samplers, shaders, constants, targets and clears -------------

void R_SetInitialContextState(IDirect3DDevice9 *device)
{
    const char *v1; // eax
    const char *v2; // eax
    const char *v3; // eax
    int v4; // [esp+0h] [ebp-Ch]
    int v5; // [esp+4h] [ebp-8h]
    int hr; // [esp+8h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, 1 )\n");
        hr = device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, 1);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v1 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    ".\\r_state.cpp (%i) device->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, 1 ) failed: %s\n",
                    211,
                    v1);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_TWOSIDEDSTENCILMODE, 1 )\n");
        v5 = device->SetRenderState(
            D3DRS_TWOSIDEDSTENCILMODE,
            1);
        if (v5 < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(v5);
                Com_Error(
                    ERR_FATAL,
                    ".\\r_state.cpp (%i) device->SetRenderState( D3DRS_TWOSIDEDSTENCILMODE, 1 ) failed: %s\n",
                    212,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_ZENABLE, D3DZB_FALSE )\n");
        v4 = device->SetRenderState(D3DRS_ZENABLE, 0);
        if (v4 < 0)
        {
            do
            {
                ++g_disableRendering;
                v3 = R_ErrorDescription(v4);
                Com_Error(
                    ERR_FATAL,
                    ".\\r_state.cpp (%i) device->SetRenderState( D3DRS_ZENABLE, D3DZB_FALSE ) failed: %s\n",
                    217,
                    v3);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetViewport(IDirect3DDevice9 *device, const GfxViewport *viewport, float nearValue, float farValue)
{
    const char *v4; // eax
    int hr; // [esp+10h] [ebp-1Ch]
    _D3DVIEWPORT9 d3dViewport; // [esp+14h] [ebp-18h] BYREF

    d3dViewport.X = viewport->x;
    d3dViewport.Y = viewport->y;
    d3dViewport.Width = viewport->width;
    d3dViewport.Height = viewport->height;
    d3dViewport.MinZ = nearValue;
    d3dViewport.MaxZ = farValue;
    if (farValue <= (double)nearValue)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h",
            247,
            0,
            "d3dViewport.MinZ < d3dViewport.MaxZ\n\t%g, %g",
            d3dViewport.MinZ,
            d3dViewport.MaxZ);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetViewport( &d3dViewport )\n");
        hr = device->SetViewport(&d3dViewport);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v4 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetViewport( &d3dViewport ) failed: %s\n",
                    248,
                    v4);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetAlphaTestEnable(IDirect3DDevice9 *device, __int16 stateBits0)
{
    const char *v2; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_ALPHATESTENABLE, (stateBits0 & GFXS0_ATEST_DISABLE) ? 0 : 1 )\n");
        hr = device->SetRenderState(
            D3DRS_ALPHATESTENABLE,
            (stateBits0 & 0x800) == 0);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_ALPHATESTENABLE, (stateBits"
                    "0 & GFXS0_ATEST_DISABLE) ? 0 : 1 ) failed: %s\n",
                    297,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetColorMask(IDirect3DDevice9 *device, uint32_t stateBits0)
{
    const char *v2; // eax
    int hr; // [esp+0h] [ebp-8h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_COLORWRITEENABLE, mask )\n");
        hr = device->SetRenderState(
            D3DRS_COLORWRITEENABLE,
            ((stateBits0 & 0x8000000) != 0 ? 7 : 0) | ((stateBits0 & 0x10000000) != 0 ? 8 : 0));
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_COLORWRITEENABLE, mask ) failed: %s\n",
                    307,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetCullFace(IDirect3DDevice9 *device, __int16 stateBits0)
{
    const char *v2; // eax
    int hr; // [esp+0h] [ebp-4h]

    if ((stateBits0 & 0xC000) != 0x4000 && (stateBits0 & 0xC000) != 0xC000 && (stateBits0 & 0xC000) != 0x8000)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h",
            313,
            0,
            "%s",
            "(stateBits0 & GFXS0_CULL_MASK) == GFXS0_CULL_NONE || (stateBits0 & GFXS0_CULL_MASK) == GFXS0_CULL_FRONT || (stateB"
            "its0 & GFXS0_CULL_MASK) == GFXS0_CULL_BACK");
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_CULLMODE, s_cullTable[(stateBits0 & GFXS0_CULL_MASK) >> GFXS0_CULL_SHIFT] )\n");
        hr = device->SetRenderState(D3DRS_CULLMODE, s_cullTable_30[(uint16_t)(stateBits0 & 0xC000) >> 14]);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_CULLMODE, s_cullTable[(stat"
                    "eBits0 & GFXS0_CULL_MASK) >> GFXS0_CULL_SHIFT] ) failed: %s\n",
                    314,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetPolygonMode(IDirect3DDevice9 *device, signed int stateBits0)
{
    const char *v2; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint(
                "device->SetRenderState( D3DRS_FILLMODE, (stateBits0 & GFXS0_POLYMODE_LINE) ? D3DFILL_WIREFRAME : D3DFILL_SOLID )\n");
        hr = device->SetRenderState(
            D3DRS_FILLMODE,
            3 - (stateBits0 < 0));
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_FILLMODE, (stateBits0 & GFX"
                    "S0_POLYMODE_LINE) ? D3DFILL_WIREFRAME : D3DFILL_SOLID ) failed: %s\n",
                    320,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_DisableBlend(IDirect3DDevice9 *device)
{
    const char *v1; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_ALPHABLENDENABLE, 0 )\n");
        hr = device->SetRenderState(D3DRS_ALPHABLENDENABLE, 0);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v1 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_ALPHABLENDENABLE, 0 ) failed: %s\n",
                    327,
                    v1);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetBlend(
    IDirect3DDevice9 *device,
    bool blendWasEnabled,
    uint32_t changedBits,
    uint32_t stateBits0)
{
    const char *v4; // eax
    const char *v5; // eax
    const char *v6; // eax
    const char *v7; // eax
    const char *v8; // eax
    const char *v9; // eax
    const char *v10; // eax
    int v11; // [esp+0h] [ebp-1Ch]
    int v12; // [esp+4h] [ebp-18h]
    int v13; // [esp+8h] [ebp-14h]
    int v14; // [esp+Ch] [ebp-10h]
    int v15; // [esp+10h] [ebp-Ch]
    int v16; // [esp+14h] [ebp-8h]
    int hr; // [esp+18h] [ebp-4h]

    if (!blendWasEnabled)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("device->SetRenderState( D3DRS_ALPHABLENDENABLE, 1 )\n");
            hr = device->SetRenderState(
                D3DRS_ALPHABLENDENABLE,
                1);
            if (hr < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v4 = R_ErrorDescription(hr);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_ALPHABLENDENABLE, 1 ) failed: %s\n",
                        353,
                        v4);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
    if ((changedBits & 0x700) != 0)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint(
                    "device->SetRenderState( D3DRS_BLENDOP, s_blendOpTable[(stateBits0 >> GFXS0_BLENDOP_RGB_SHIFT) & GFXS_BLENDOP_MASK] )\n");
            v16 = device->SetRenderState(D3DRS_BLENDOP, s_blendOpTable_30[(stateBits0 >> 8) & 7]);
            if (v16 < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v5 = R_ErrorDescription(v16);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_BLENDOP, s_blendOpTable[("
                        "stateBits0 >> GFXS0_BLENDOP_RGB_SHIFT) & GFXS_BLENDOP_MASK] ) failed: %s\n",
                        356,
                        v5);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
    if ((changedBits & 0xF) != 0)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint(
                    "device->SetRenderState( D3DRS_SRCBLEND, s_blendTable[(stateBits0 >> GFXS0_SRCBLEND_RGB_SHIFT) & GFXS_BLEND_MASK] )\n");
            v15 = device->SetRenderState(D3DRS_SRCBLEND, s_blendTable_30[stateBits0 & 0xF]);
            if (v15 < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v6 = R_ErrorDescription(v15);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_SRCBLEND, s_blendTable[(s"
                        "tateBits0 >> GFXS0_SRCBLEND_RGB_SHIFT) & GFXS_BLEND_MASK] ) failed: %s\n",
                        359,
                        v6);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
    if ((changedBits & 0xF0) != 0)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint(
                    "device->SetRenderState( D3DRS_DESTBLEND, s_blendTable[(stateBits0 >> GFXS0_DSTBLEND_RGB_SHIFT) & GFXS_BLEND_MASK] )\n");
            v14 = device->SetRenderState(
                D3DRS_DESTBLEND,
                s_blendTable_30[(uint8_t)stateBits0 >> 4]);
            if (v14 < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v7 = R_ErrorDescription(v14);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_DESTBLEND, s_blendTable[("
                        "stateBits0 >> GFXS0_DSTBLEND_RGB_SHIFT) & GFXS_BLEND_MASK] ) failed: %s\n",
                        362,
                        v7);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
    if ((changedBits & 0x7000000) != 0)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint(
                    "device->SetRenderState( D3DRS_BLENDOPALPHA, s_blendOpTable[(stateBits0 >> GFXS0_BLENDOP_ALPHA_SHIFT) & GFXS_BLENDOP_MASK] )\n");
            v13 = device->SetRenderState(D3DRS_BLENDOPALPHA, s_blendOpTable_30[HIBYTE(stateBits0) & 7]);
            if (v13 < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v8 = R_ErrorDescription(v13);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_BLENDOPALPHA, s_blendOpTa"
                        "ble[(stateBits0 >> GFXS0_BLENDOP_ALPHA_SHIFT) & GFXS_BLENDOP_MASK] ) failed: %s\n",
                        365,
                        v8);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
    if ((changedBits & 0xF0000) != 0)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint(
                    "device->SetRenderState( D3DRS_SRCBLENDALPHA, s_blendTable[(stateBits0 >> GFXS0_SRCBLEND_ALPHA_SHIFT) & GFXS_BLEND_MASK] )\n");
            v12 = device->SetRenderState(D3DRS_SRCBLENDALPHA, s_blendTable_30[HIWORD(stateBits0) & 0xF]);
            if (v12 < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v9 = R_ErrorDescription(v12);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_SRCBLENDALPHA, s_blendTab"
                        "le[(stateBits0 >> GFXS0_SRCBLEND_ALPHA_SHIFT) & GFXS_BLEND_MASK] ) failed: %s\n",
                        368,
                        v9);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
    if ((changedBits & 0xF00000) != 0)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint(
                    "device->SetRenderState( D3DRS_DESTBLENDALPHA, s_blendTable[(stateBits0 >> GFXS0_DSTBLEND_ALPHA_SHIFT) & GFXS_BLEND_MASK] )\n");
            v11 = device->SetRenderState(
                D3DRS_DESTBLENDALPHA,
                s_blendTable_30[(stateBits0 >> 20) & 0xF]);
            if (v11 < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v10 = R_ErrorDescription(v11);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_DESTBLENDALPHA, s_blendTa"
                        "ble[(stateBits0 >> GFXS0_DSTBLEND_ALPHA_SHIFT) & GFXS_BLEND_MASK] ) failed: %s\n",
                        371,
                        v10);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
}

void R_SetAlphaTestFunction(GfxCmdBufState *state, __int16 stateBits0)
{
    const char *v2; // eax
    const char *v3; // eax
    int v4; // [esp+0h] [ebp-14h]
    int hr; // [esp+4h] [ebp-10h]
    uint8_t ref; // [esp+Bh] [ebp-9h]
    IDirect3DDevice9 *device; // [esp+Ch] [ebp-8h]
    uint32_t function; // [esp+10h] [ebp-4h]

    if ((stateBits0 & 0x3000) == 0x1000)
    {
        function = 5;
        ref = 0;
    }
    else if ((stateBits0 & 0x3000) == 0x2000)
    {
        function = 2;
        ref = 0x80;
    }
    else
    {
        if ((stateBits0 & 0x3000) != 0x3000)
            MyAssertHandler(
                "c:\\trees\\cod3\\src\\gfx_d3d\\r_state.h",
                705,
                1,
                "%s",
                "(stateBits0 & GFXS0_ATEST_MASK) == GFXS0_ATEST_GE_128");
        function = 7;
        ref = 0x80;
    }
    device = state->prim.device;
    iassert( device );
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_ALPHAFUNC, function )\n");
        hr = device->SetRenderState(D3DRS_ALPHAFUNC, function);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_state.h (%i) device->SetRenderState( D3DRS_ALPHAFUNC, function ) failed: %s\n",
                    713,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    if (state->alphaRef != ref)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("device->SetRenderState( D3DRS_ALPHAREF, ref )\n");
            v4 = device->SetRenderState(D3DRS_ALPHAREF, ref);
            if (v4 < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v3 = R_ErrorDescription(v4);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_state.h (%i) device->SetRenderState( D3DRS_ALPHAREF, ref ) failed: %s\n",
                        716,
                        v3);
                } while (alwaysfails);
            }
        } while (alwaysfails);
        state->alphaRef = ref;
    }
}

void R_HW_SetDepthWriteEnable(IDirect3DDevice9 *device, char stateBits1)
{
    const char *v2; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_ZWRITEENABLE, (stateBits1 & GFXS1_DEPTHWRITE) ? 1 : 0 )\n");
        hr = device->SetRenderState(
            D3DRS_ZWRITEENABLE,
            (stateBits1 & 1) != 0);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_ZWRITEENABLE, (stateBits1 &"
                    " GFXS1_DEPTHWRITE) ? 1 : 0 ) failed: %s\n",
                    395,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetDepthTestEnable(IDirect3DDevice9 *device, char stateBits1)
{
    const char *v2; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_ZENABLE, (stateBits1 & GFXS1_DEPTHTEST_DISABLE) ? D3DZB_FALSE : D3DZB_TRUE )\n");
        hr = device->SetRenderState(
            D3DRS_ZENABLE,
            (stateBits1 & 2) == 0);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_ZENABLE, (stateBits1 & GFXS"
                    "1_DEPTHTEST_DISABLE) ? D3DZB_FALSE : D3DZB_TRUE ) failed: %s\n",
                    401,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetDepthTestFunction(IDirect3DDevice9 *device, char stateBits1)
{
    const char *v2; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint(
                "device->SetRenderState( D3DRS_ZFUNC, s_depthTestTable[(stateBits1 & GFXS1_DEPTHTEST_MASK) >> GFXS1_DEPTHTEST_SHIFT] )\n");
        hr = device->SetRenderState(D3DRS_ZFUNC, s_depthTestTable_30[(uint8_t)(stateBits1 & 0xC) >> 2]);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_ZFUNC, s_depthTestTable[(st"
                    "ateBits1 & GFXS1_DEPTHTEST_MASK) >> GFXS1_DEPTHTEST_SHIFT] ) failed: %s\n",
                    407,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_EnableStencil(IDirect3DDevice9 *device)
{
    const char *v1; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_STENCILENABLE, 1 )\n");
        hr = device->SetRenderState(D3DRS_STENCILENABLE, 1u);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v1 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_STENCILENABLE, 1 ) failed: %s\n",
                    413,
                    v1);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_DisableStencil(IDirect3DDevice9 *device)
{
    const char *v1; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_STENCILENABLE, 0 )\n");
        hr = device->SetRenderState(D3DRS_STENCILENABLE, 0);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v1 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_STENCILENABLE, 0 ) failed: %s\n",
                    419,
                    v1);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetFrontStencilOp(
    IDirect3DDevice9 *device,
    uint32_t stencilOpPass,
    uint32_t stencilOpFail,
    uint32_t stencilOpZFail)
{
    const char *v4; // eax
    const char *v5; // eax
    const char *v6; // eax
    int v7; // [esp+0h] [ebp-Ch]
    int v8; // [esp+4h] [ebp-8h]
    int hr; // [esp+8h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_STENCILPASS, s_stencilOpTable[stencilOpPass] )\n");
        hr = device->SetRenderState(D3DRS_STENCILPASS, s_stencilOpTable_30[stencilOpPass]);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v4 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_STENCILPASS, s_stencilOpTab"
                    "le[stencilOpPass] ) failed: %s\n",
                    425,
                    v4);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_STENCILFAIL, s_stencilOpTable[stencilOpFail] )\n");
        v8 = device->SetRenderState(D3DRS_STENCILFAIL, s_stencilOpTable_30[stencilOpFail]);
        if (v8 < 0)
        {
            do
            {
                ++g_disableRendering;
                v5 = R_ErrorDescription(v8);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_STENCILFAIL, s_stencilOpTab"
                    "le[stencilOpFail] ) failed: %s\n",
                    426,
                    v5);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_STENCILZFAIL, s_stencilOpTable[stencilOpZFail] )\n");
        v7 = device->SetRenderState(D3DRS_STENCILZFAIL, s_stencilOpTable_30[stencilOpZFail]);
        if (v7 < 0)
        {
            do
            {
                ++g_disableRendering;
                v6 = R_ErrorDescription(v7);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_STENCILZFAIL, s_stencilOpTa"
                    "ble[stencilOpZFail] ) failed: %s\n",
                    427,
                    v6);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetBackStencilOp(
    IDirect3DDevice9 *device,
    uint32_t stencilOpPass,
    uint32_t stencilOpFail,
    uint32_t stencilOpZFail)
{
    const char *v4; // eax
    const char *v5; // eax
    const char *v6; // eax
    int v7; // [esp+0h] [ebp-Ch]
    int v8; // [esp+4h] [ebp-8h]
    int hr; // [esp+8h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_CCW_STENCILPASS, s_stencilOpTable[stencilOpPass] )\n");
        hr = device->SetRenderState(D3DRS_CCW_STENCILPASS, s_stencilOpTable_30[stencilOpPass]);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v4 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_CCW_STENCILPASS, s_stencilO"
                    "pTable[stencilOpPass] ) failed: %s\n",
                    433,
                    v4);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_CCW_STENCILFAIL, s_stencilOpTable[stencilOpFail] )\n");
        v8 = device->SetRenderState(D3DRS_CCW_STENCILFAIL, s_stencilOpTable_30[stencilOpFail]);
        if (v8 < 0)
        {
            do
            {
                ++g_disableRendering;
                v5 = R_ErrorDescription(v8);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_CCW_STENCILFAIL, s_stencilO"
                    "pTable[stencilOpFail] ) failed: %s\n",
                    434,
                    v5);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_CCW_STENCILZFAIL, s_stencilOpTable[stencilOpZFail] )\n");
        v7 = device->SetRenderState(D3DRS_CCW_STENCILZFAIL, s_stencilOpTable_30[stencilOpZFail]);
        if (v7 < 0)
        {
            do
            {
                ++g_disableRendering;
                v6 = R_ErrorDescription(v7);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_CCW_STENCILZFAIL, s_stencil"
                    "OpTable[stencilOpZFail] ) failed: %s\n",
                    435,
                    v6);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetFrontStencilFunc(IDirect3DDevice9 *device, uint32_t stencilFunc)
{
    const char *v2; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_STENCILFUNC, s_stencilFuncTable[stencilFunc] )\n");
        hr = device->SetRenderState(D3DRS_STENCILFUNC, s_stencilFuncTable_30[stencilFunc]);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_STENCILFUNC, s_stencilFuncT"
                    "able[stencilFunc] ) failed: %s\n",
                    441,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetBackStencilFunc(IDirect3DDevice9 *device, uint32_t stencilFunc)
{
    const char *v2; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_CCW_STENCILFUNC, s_stencilFuncTable[stencilFunc] )\n");
        hr = device->SetRenderState(D3DRS_CCW_STENCILFUNC, s_stencilFuncTable_30[stencilFunc]);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_CCW_STENCILFUNC, s_stencilF"
                    "uncTable[stencilFunc] ) failed: %s\n",
                    447,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

uint32_t R_HW_SetSamplerState(
    IDirect3DDevice9 *device,
    uint32_t samplerIndex,
    uint32_t samplerState,
    uint32_t oldSamplerState)
{
    const char *v4; // eax
    const char *v5; // eax
    const char *v6; // eax
    const char *v7; // eax
    const char *v8; // eax
    const char *v9; // eax
    const char *v10; // eax
    int v12; // [esp+0h] [ebp-38h]
    int v13; // [esp+4h] [ebp-34h]
    int v14; // [esp+8h] [ebp-30h]
    int v15; // [esp+Ch] [ebp-2Ch]
    int v16; // [esp+10h] [ebp-28h]
    int v17; // [esp+14h] [ebp-24h]
    int hr; // [esp+18h] [ebp-20h]
    uint32_t finalSamplerState; // [esp+1Ch] [ebp-1Ch]
    uint32_t diffSamplerState; // [esp+2Ch] [ebp-Ch]

    finalSamplerState = samplerState;
    diffSamplerState = oldSamplerState ^ samplerState;
    iassert( diffSamplerState );
    if ((diffSamplerState & 0xF00) != 0)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_MINFILTER, minFilter )\n");
            hr = device->SetSamplerState(
                samplerIndex,
                D3DSAMP_MINFILTER,
                (uint16_t)(samplerState & 0xF00) >> 8);
            if (hr < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v4 = R_ErrorDescription(hr);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_MINFILTE"
                        "R, minFilter ) failed: %s\n",
                        148,
                        v4);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
    if ((diffSamplerState & 0xF000) != 0)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_MAGFILTER, magFilter )\n");
            v17 = device->SetSamplerState(samplerIndex, D3DSAMP_MAGFILTER, (uint16_t)(samplerState & 0xF000) >> 12);
            if (v17 < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v5 = R_ErrorDescription(v17);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_MAGFILTE"
                        "R, magFilter ) failed: %s\n",
                        154,
                        v5);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
    if ((_BYTE)diffSamplerState)
    {
        if ((uint8_t)samplerState <= 1u)
        {
            finalSamplerState = (uint8_t)oldSamplerState | samplerState & 0xFFFFFF00;
        }
        else
        {
            do
            {
                if (r_logFile && r_logFile->current.integer)
                    RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_MAXANISOTROPY, anisotropy )\n");
                v16 = device->SetSamplerState(
                    samplerIndex,
                    D3DSAMP_MAXANISOTROPY,
                    (uint8_t)samplerState);
                if (v16 < 0)
                {
                    do
                    {
                        ++g_disableRendering;
                        v6 = R_ErrorDescription(v16);
                        Com_Error(
                            ERR_FATAL,
                            "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_MAXANI"
                            "SOTROPY, anisotropy ) failed: %s\n",
                            161,
                            v6);
                    } while (alwaysfails);
                }
            } while (alwaysfails);
        }
    }
    if ((diffSamplerState & 0xF0000) != 0)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_MIPFILTER, mipFilter )\n");
            v15 = device->SetSamplerState(samplerIndex, D3DSAMP_MIPFILTER, (samplerState & 0xF0000) >> 16);
            if (v15 < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v7 = R_ErrorDescription(v15);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_MIPFILTE"
                        "R, mipFilter ) failed: %s\n",
                        169,
                        v7);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
    if ((diffSamplerState & 0x3F00000) != 0)
    {
        if ((diffSamplerState & 0x300000) != 0)
        {
            do
            {
                if (r_logFile && r_logFile->current.integer)
                    RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_ADDRESSU, address )\n");
                v14 = device->SetSamplerState(samplerIndex, D3DSAMP_ADDRESSU, (samplerState & 0x300000) >> 20);
                if (v14 < 0)
                {
                    do
                    {
                        ++g_disableRendering;
                        v8 = R_ErrorDescription(v14);
                        Com_Error(
                            ERR_FATAL,
                            "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_ADDRES"
                            "SU, address ) failed: %s\n",
                            177,
                            v8);
                    } while (alwaysfails);
                }
            } while (alwaysfails);
        }
        if ((diffSamplerState & 0xC00000) != 0)
        {
            do
            {
                if (r_logFile && r_logFile->current.integer)
                    RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_ADDRESSV, address )\n");
                v13 = device->SetSamplerState(samplerIndex, D3DSAMP_ADDRESSV, (samplerState & 0xC00000) >> 22);
                if (v13 < 0)
                {
                    do
                    {
                        ++g_disableRendering;
                        v9 = R_ErrorDescription(v13);
                        Com_Error(
                            ERR_FATAL,
                            "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_ADDRES"
                            "SV, address ) failed: %s\n",
                            182,
                            v9);
                    } while (alwaysfails);
                }
            } while (alwaysfails);
        }
        if ((diffSamplerState & 0x3000000) != 0)
        {
            do
            {
                if (r_logFile && r_logFile->current.integer)
                    RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_ADDRESSW, address )\n");
                v12 = device->SetSamplerState(samplerIndex, D3DSAMP_ADDRESSW, (samplerState & 0x3000000) >> 24);
                if (v12 < 0)
                {
                    do
                    {
                        ++g_disableRendering;
                        v10 = R_ErrorDescription(v12);
                        Com_Error(
                            ERR_FATAL,
                            "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_ADDRES"
                            "SW, address ) failed: %s\n",
                            187,
                            v10);
                    } while (alwaysfails);
                }
            } while (alwaysfails);
        }
    }
    return finalSamplerState;
}

void R_HW_DisableSampler(IDirect3DDevice9 *device, uint32_t samplerIndex)
{
    const char *v2; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetTexture( samplerIndex, 0 )\n");

        hr = device->SetTexture(samplerIndex,0);

        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetTexture( samplerIndex, 0 ) failed: %s\n",
                    127,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetRenderTarget(GfxCmdBufState *state, GfxRenderTargetId newTargetId)
{
    int hr; // [esp+4h] [ebp-8h]
    IDirect3DDevice9 *device; // [esp+8h] [ebp-4h]

    device = state->prim.device;
    iassert(device);

    if (gfxRenderTargets[state->renderTargetId].surface.color != gfxRenderTargets[newTargetId].surface.color)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("device->SetRenderTarget( 0, gfxRenderTargets[newTargetId].surface.color )\n");
            hr = device->SetRenderTarget(0, gfxRenderTargets[newTargetId].surface.color);
            if (hr < 0)
            {
                do
                {
                    ++g_disableRendering;
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_state.h (%i) device->SetRenderTarget( 0, gfxRenderTargets[newTargetId].surf"
                        "ace.color ) failed: %s\n",
                        892,
                        R_ErrorDescription(hr));
                } while (alwaysfails);
            }
        } while (alwaysfails);

        state->viewport.x = 0;
        state->viewport.y = 0;
        state->viewport.width = gfxRenderTargets[newTargetId].width;
        state->viewport.height = gfxRenderTargets[newTargetId].height;
        state->depthRangeType = GFX_DEPTH_RANGE_FULL;
        state->depthRangeNear = 0.0;
        state->depthRangeFar = 1.0;
    }

    if (gfxRenderTargets[state->renderTargetId].surface.depthStencil != gfxRenderTargets[newTargetId].surface.depthStencil)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("device->SetDepthStencilSurface( gfxRenderTargets[newTargetId].surface.depthStencil )\n");

            hr = device->SetDepthStencilSurface(gfxRenderTargets[newTargetId].surface.depthStencil);
            if (hr < 0)
            {
                do
                {
                    ++g_disableRendering;
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_state.h (%i) device->SetDepthStencilSurface( gfxRenderTargets[newTargetId]."
                        "surface.depthStencil ) failed: %s\n",
                        905,
                        R_ErrorDescription(hr));
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
}

void R_ClearScreenInternal(
    IDirect3DDevice9 *device,
    uint8_t whichToClear,
    const float *color,
    float depth,
    uint8_t stencil,
    const GfxViewport *viewport)
{
    const char *v6; // eax
    const char *v7; // eax
    const char *v8; // eax
    const char *v9; // eax
    int hr; // [esp+9Ch] [ebp-8h]
    GfxColor nativeColor; // [esp+A0h] [ebp-4h] BYREF

    iassert( device );
    if (r_logFile->current.integer)
    {
        RB_LogPrint("---------- R_ClearScreenInternal\n");
        v6 = va(
            "---------- (%c) color %g %g %g %g\n",
            (whichToClear & 1) != 0 ? 42 : 32,
            *color,
            color[1],
            color[2],
            color[3]);
        RB_LogPrint(v6);
        v7 = va("---------- (%c) depth %g\n", (whichToClear & 2) != 0 ? 42 : 32, depth);
        RB_LogPrint(v7);
        v8 = va("---------- (%c) stencil %i\n", (whichToClear & 4) != 0 ? 42 : 32, stencil);
        RB_LogPrint(v8);
    }
    iassert( whichToClear );
    iassert( color );
    //iassert( depth not in [0.0f, 1.0f]\n\t%g not in [%g, %g] );
    Byte4PackVertexColor(color, (uint8_t *)&nativeColor);
    iassert( !viewport );
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->Clear( 0, 0, whichToClear, nativeColor.packed, depth, stencil )\n");
        //hr = ((int(__stdcall *)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))device->Clear)(
        //    device,
        //    0,
        //    0,
        //    whichToClear,
        //    (GfxColor)nativeColor.packed,
        //    LODWORD(depth),
        //    stencil);
        hr = device->Clear(0, 0, whichToClear, nativeColor.packed, depth, stencil);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v9 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    ".\\r_state.cpp (%i) device->Clear( 0, 0, whichToClear, nativeColor.packed, depth, stencil ) failed: %s\n",
                    1458,
                    v9);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetPolygonOffset(IDirect3DDevice9 *device, float scale, float bias)
{
    const char *v3; // eax
    const char *v4; // eax
    int v5; // [esp+8h] [ebp-8h]
    int hr; // [esp+Ch] [ebp-4h]

    if (gfxMetrics.slopeScaleDepthBias)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("device->SetRenderState( D3DRS_SLOPESCALEDEPTHBIAS, FloatAsInt( &scale ) )\n");
            hr = device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, FloatAsInt(scale));
            if (hr < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v3 = R_ErrorDescription(hr);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_SLOPESCALEDEPTHBIAS, Floa"
                        "tAsInt( &scale ) ) failed: %s\n",
                        468,
                        v3);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
    else
    {
        bias = bias + bias;
    }
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_DEPTHBIAS, FloatAsInt( &bias ) )\n");
        v5 = device->SetRenderState(D3DRS_DEPTHBIAS, FloatAsInt(bias));
        if (v5 < 0)
        {
            do
            {
                ++g_disableRendering;
                v4 = R_ErrorDescription(v5);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetRenderState( D3DRS_DEPTHBIAS, FloatAsInt( &bia"
                    "s ) ) failed: %s\n",
                    472,
                    v4);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_SetAlphaAntiAliasingState(IDirect3DDevice9 *device, __int16 stateBits0)
{
    const char *v2; // eax
    int hr; // [esp+0h] [ebp-Ch]
    _D3DFORMAT aaAlphaFormat; // [esp+4h] [ebp-8h]

    if ((stateBits0 & 0xF00) != 0)
    {
        aaAlphaFormat = D3DFMT_UNKNOWN;
    }
    else if (r_aaAlpha->current.integer == 2)
    {
        aaAlphaFormat = (_D3DFORMAT)1094800211;
    }
    else
    {
        aaAlphaFormat = (_D3DFORMAT)1129272385;
    }
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_ADAPTIVETESS_Y, aaAlphaFormat )\n");
        hr = device->SetRenderState(D3DRS_ADAPTIVETESS_Y, aaAlphaFormat);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v2 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    ".\\r_state.cpp (%i) device->SetRenderState( D3DRS_ADAPTIVETESS_Y, aaAlphaFormat ) failed: %s\n",
                    826,
                    v2);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_SetDefaultAlphaTestFunction(GfxCmdBufState *state)
{
    const char *v1; // eax
    int hr; // [esp+0h] [ebp-8h]
    IDirect3DDevice9 *device; // [esp+4h] [ebp-4h]

    device = state->prim.device;
    iassert( device );
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetRenderState( D3DRS_ALPHAREF, 0 )\n");
        hr = device->SetRenderState(D3DRS_ALPHAREF, 0);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v1 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_state.h (%i) device->SetRenderState( D3DRS_ALPHAREF, 0 ) failed: %s\n",
                    729,
                    v1);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    state->alphaRef = 0;
}

void R_HW_ForceSamplerState(IDirect3DDevice9 *device, uint32_t samplerIndex, uint32_t samplerState)
{
    const char *v3; // eax
    const char *v4; // eax
    const char *v5; // eax
    const char *v6; // eax
    const char *v7; // eax
    const char *v8; // eax
    const char *v9; // eax
    const char *v10; // eax
    int v11; // [esp+0h] [ebp-34h]
    int v12; // [esp+4h] [ebp-30h]
    int v13; // [esp+8h] [ebp-2Ch]
    int v14; // [esp+Ch] [ebp-28h]
    int v15; // [esp+10h] [ebp-24h]
    int v16; // [esp+14h] [ebp-20h]
    int v17; // [esp+18h] [ebp-1Ch]
    int hr; // [esp+1Ch] [ebp-18h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_MINFILTER, minFilter )\n");
        hr = device->SetSamplerState(samplerIndex, D3DSAMP_MINFILTER, (uint16_t)(samplerState & 0xF00) >> 8);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v3 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_MINFILTER,"
                    " minFilter ) failed: %s\n",
                    204,
                    v3);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_MAGFILTER, magFilter )\n");
        v17 = device->SetSamplerState(
            samplerIndex,
            D3DSAMP_MAGFILTER,
            (uint16_t)(samplerState & 0xF000) >> 12);
        if (v17 < 0)
        {
            do
            {
                ++g_disableRendering;
                v4 = R_ErrorDescription(v17);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_MAGFILTER,"
                    " magFilter ) failed: %s\n",
                    207,
                    v4);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    if ((uint8_t)samplerState > 1u)
    {
        do
        {
            if (r_logFile && r_logFile->current.integer)
                RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_MAXANISOTROPY, anisotropy )\n");
            v16 = device->SetSamplerState(samplerIndex, D3DSAMP_MAXANISOTROPY, (uint8_t)samplerState);
            if (v16 < 0)
            {
                do
                {
                    ++g_disableRendering;
                    v5 = R_ErrorDescription(v16);
                    Com_Error(
                        ERR_FATAL,
                        "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_MAXANISO"
                        "TROPY, anisotropy ) failed: %s\n",
                        211,
                        v5);
                } while (alwaysfails);
            }
        } while (alwaysfails);
    }
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_MIPFILTER, mipFilter )\n");
        v15 = device->SetSamplerState(samplerIndex, D3DSAMP_MIPFILTER, (samplerState & 0xF0000) >> 16);
        if (v15 < 0)
        {
            do
            {
                ++g_disableRendering;
                v6 = R_ErrorDescription(v15);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_MIPFILTER,"
                    " mipFilter ) failed: %s\n",
                    214,
                    v6);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_MIPMAPLODBIAS, dx.mipBias )\n");
        v14 = device->SetSamplerState(samplerIndex, D3DSAMP_MIPMAPLODBIAS, dx.mipBias);
        if (v14 < 0)
        {
            do
            {
                ++g_disableRendering;
                v7 = R_ErrorDescription(v14);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_MIPMAPLODB"
                    "IAS, dx.mipBias ) failed: %s\n",
                    215,
                    v7);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_ADDRESSU, address )\n");
        v13 = device->SetSamplerState(
            samplerIndex,
            D3DSAMP_ADDRESSU,
            (samplerState & 0x300000) >> 20);
        if (v13 < 0)
        {
            do
            {
                ++g_disableRendering;
                v8 = R_ErrorDescription(v13);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_ADDRESSU, "
                    "address ) failed: %s\n",
                    218,
                    v8);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_ADDRESSV, address )\n");
        v12 = device->SetSamplerState(samplerIndex, D3DSAMP_ADDRESSV, (samplerState & 0xC00000) >> 22);
        if (v12 < 0)
        {
            do
            {
                ++g_disableRendering;
                v9 = R_ErrorDescription(v12);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_ADDRESSV, "
                    "address ) failed: %s\n",
                    221,
                    v9);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetSamplerState( samplerIndex, D3DSAMP_ADDRESSW, address )\n");
        v11 = device->SetSamplerState(samplerIndex, D3DSAMP_ADDRESSW, (samplerState & 0x3000000) >> 24);
        if (v11 < 0)
        {
            do
            {
                ++g_disableRendering;
                v10 = R_ErrorDescription(v11);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetSamplerState( samplerIndex, D3DSAMP_ADDRESSW, "
                    "address ) failed: %s\n",
                    224,
                    v10);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetVertexShaderConstant(
    IDirect3DDevice9 *device,
    uint32_t dest,
    const float *data,
    uint32_t rowCount)
{
    const char *v4; // eax
    const char *v5; // eax
    int hr; // [esp+30h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetVertexShaderConstantF( dest, data, rowCount )\n");
        hr = device->SetVertexShaderConstantF(dest, data, rowCount);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v4 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetVertexShaderConstantF( dest, data, rowCount ) failed: %s\n",
                    94,
                    v4);
            } while (alwaysfails);
        }
    } while (alwaysfails);
    if (r_logFile->current.integer)
    {
        while (rowCount)
        {
            v5 = va("vertex const %i: %g %g %g %g\n", dest, *data, data[1], data[2], data[3]);
            RB_LogPrint(v5);
            ++dest;
            data += 4;
            --rowCount;
        }
    }
}

void R_HW_SetPixelShaderConstant(
    IDirect3DDevice9 *device,
    uint32_t dest,
    const float *data,
    uint32_t rowCount)
{
    const char *v4; // eax
    int hr; // [esp+0h] [ebp-4h]

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetPixelShaderConstantF( dest, data, rowCount )\n");
        hr = device->SetPixelShaderConstantF(dest, data, rowCount);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                v4 = R_ErrorDescription(hr);
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetPixelShaderConstantF( dest, data, rowCount ) failed: %s\n",
                    113,
                    v4);
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetPixelShader(IDirect3DDevice9 *device, const MaterialPixelShader *mtlShader)
{
    int v2; // eax
    HRESULT hr; // [esp+4h] [ebp-4h]

    iassert(device);

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetPixelShader( mtlShader ? mtlShader->prog.ps : 0 )\n");
        if (mtlShader)
        {
            v2 = device->SetPixelShader(mtlShader->prog.ps);
        }
        else
        {
            v2 = device->SetPixelShader(0);
        }
        hr = v2;
        if (v2 < 0)
        {
            do
            {
                ++g_disableRendering;
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetPixelShader( mtlShader ? mtlShader->prog.ps : "
                    "0 ) failed: %s\n",
                    454,
                    R_ErrorDescription(hr));
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetVertexShader(IDirect3DDevice9 *device, const MaterialVertexShader *mtlShader)
{
    int v2; // eax
    HRESULT hr; // [esp+4h] [ebp-4h]

    iassert(device);
    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetVertexShader( mtlShader ? mtlShader->prog.vs : 0 )\n");
        if (mtlShader)
        {
            v2 = device->SetVertexShader(mtlShader->prog.vs);
        }
        else
        {
            v2 = device->SetVertexShader(0);
        }
        hr = v2;
        if (v2 < 0)
        {
            do
            {
                ++g_disableRendering;
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetVertexShader( mtlShader ? mtlShader->prog.vs :"
                    " 0 ) failed: %s\n",
                    461,
                    R_ErrorDescription(hr));
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

void R_HW_SetSamplerTexture(IDirect3DDevice9 *device, uint32_t samplerIndex, const GfxTexture *texture)
{
    int hr; // [esp+0h] [ebp-4h]

    iassert(texture);
    iassert(texture->basemap);

    do
    {
        if (r_logFile && r_logFile->current.integer)
            RB_LogPrint("device->SetTexture( samplerIndex, texture->basemap )\n");

        hr = device->SetTexture(samplerIndex, texture->basemap);
        if (hr < 0)
        {
            do
            {
                ++g_disableRendering;
                Com_Error(
                    ERR_FATAL,
                    "c:\\trees\\cod3\\src\\gfx_d3d\\r_setstate_d3d.h (%i) device->SetTexture( samplerIndex, texture->basemap ) failed: %s\n",
                    121,
                    R_ErrorDescription(hr));
            } while (alwaysfails);
        }
    } while (alwaysfails);
}

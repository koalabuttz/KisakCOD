// The Direct3D 9 implementation of the renderer's graphics interface: the
// D3D9-shaped calls (renderer plan M1a).
//
// One typed forwarder per Direct3D 9 interface method the renderer still calls
// directly: `object->Method(args)` in a renderer unit became
// `R_HW_Method(object, args)`, and the call itself happens here. Each forwarder
// passes its arguments through unchanged and returns the method's own result;
// none adds a check, a log line or a retry, so callers behave exactly as before.
// The overloads follow d3d9.h: one per interface that declares the method.
//
// M1b folds these into the backend operations of the renderer plan. Until then
// a new direct call in src/gfx_d3d gets a forwarder here instead (see
// scripts/tests/check_rhi_boundary.py and docs/renderer-interface.md).
#include <universal/q_shared.h>
#include "r_rhi.h"

// ---- Reference counts -------------------------------------------------------

ULONG R_HW_Release(IDirect3D9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DBaseTexture9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DCubeTexture9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DDevice9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DIndexBuffer9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DPixelShader9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DQuery9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DResource9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DStateBlock9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DSurface9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DSwapChain9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DTexture9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DVertexBuffer9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DVertexDeclaration9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DVertexShader9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DVolume9 *object)
{
    return object->Release();
}

ULONG R_HW_Release(IDirect3DVolumeTexture9 *object)
{
    return object->Release();
}

ULONG R_HW_AddRef(IDirect3D9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DBaseTexture9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DCubeTexture9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DDevice9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DIndexBuffer9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DPixelShader9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DQuery9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DResource9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DStateBlock9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DSurface9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DSwapChain9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DTexture9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DVertexBuffer9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DVertexDeclaration9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DVertexShader9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DVolume9 *object)
{
    return object->AddRef();
}

ULONG R_HW_AddRef(IDirect3DVolumeTexture9 *object)
{
    return object->AddRef();
}

// ---- The Direct3D 9 object: adapters, formats and the device ----------------

IDirect3D9 *R_HW_Direct3DCreate9(UINT SDKVersion)
{
    return Direct3DCreate9(SDKVersion);
}

HRESULT R_HW_CheckDeviceFormat(IDirect3D9 *object, UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
{
    return object->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
}

HRESULT R_HW_CheckDepthStencilMatch(IDirect3D9 *object, UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
{
    return object->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
}

HRESULT R_HW_CheckDeviceMultiSampleType(IDirect3D9 *object, UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
{
    return object->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
}

UINT R_HW_GetAdapterCount(IDirect3D9 *object)
{
    return object->GetAdapterCount();
}

HMONITOR R_HW_GetAdapterMonitor(IDirect3D9 *object, UINT Adapter)
{
    return object->GetAdapterMonitor(Adapter);
}

HRESULT R_HW_GetAdapterIdentifier(IDirect3D9 *object, UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier)
{
    return object->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
}

UINT R_HW_GetAdapterModeCount(IDirect3D9 *object, UINT Adapter, D3DFORMAT Format)
{
    return object->GetAdapterModeCount(Adapter, Format);
}

HRESULT R_HW_EnumAdapterModes(IDirect3D9 *object, UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
{
    return object->EnumAdapterModes(Adapter, Format, Mode, pMode);
}

HRESULT R_HW_GetAdapterDisplayMode(IDirect3D9 *object, UINT Adapter, D3DDISPLAYMODE* pMode)
{
    return object->GetAdapterDisplayMode(Adapter, pMode);
}

HRESULT R_HW_GetDeviceCaps(IDirect3D9 *object, UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps)
{
    return object->GetDeviceCaps(Adapter, DeviceType, pCaps);
}

HRESULT R_HW_GetDeviceCaps(IDirect3DDevice9 *object, D3DCAPS9* pCaps)
{
    return object->GetDeviceCaps(pCaps);
}

HRESULT R_HW_CreateDevice(IDirect3D9 *object, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, struct IDirect3DDevice9** ppReturnedDeviceInterface)
{
    return object->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
}

// ---- Device lifetime and frames ---------------------------------------------

HRESULT R_HW_TestCooperativeLevel(IDirect3DDevice9 *object)
{
    return object->TestCooperativeLevel();
}

HRESULT R_HW_Reset(IDirect3DDevice9 *object, D3DPRESENT_PARAMETERS* pPresentationParameters)
{
    return object->Reset(pPresentationParameters);
}

HRESULT R_HW_BeginScene(IDirect3DDevice9 *object)
{
    return object->BeginScene();
}

HRESULT R_HW_EndScene(IDirect3DDevice9 *object)
{
    return object->EndScene();
}

HRESULT R_HW_Present(IDirect3DDevice9 *object, const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region)
{
    return object->Present(src_rect, dst_rect, dst_window_override, dirty_region);
}

HRESULT R_HW_Present(IDirect3DSwapChain9 *object, const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region, DWORD flags)
{
    return object->Present(src_rect, dst_rect, dst_window_override, dirty_region, flags);
}

HRESULT R_HW_CreateAdditionalSwapChain(IDirect3DDevice9 *object, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain)
{
    return object->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain);
}

HRESULT R_HW_GetSwapChain(IDirect3DDevice9 *object, UINT iSwapChain, IDirect3DSwapChain9** pSwapChain)
{
    return object->GetSwapChain(iSwapChain, pSwapChain);
}

HRESULT R_HW_GetBackBuffer(IDirect3DDevice9 *object, UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer)
{
    return object->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer);
}

HRESULT R_HW_GetBackBuffer(IDirect3DSwapChain9 *object, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, struct IDirect3DSurface9 **ppBackBuffer)
{
    return object->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
}

void R_HW_SetGammaRamp(IDirect3DDevice9 *object, UINT swapchain_idx, DWORD flags, const D3DGAMMARAMP *ramp)
{
    return object->SetGammaRamp(swapchain_idx, flags, ramp);
}

UINT R_HW_GetAvailableTextureMem(IDirect3DDevice9 *object)
{
    return object->GetAvailableTextureMem();
}

// ---- Resource creation ------------------------------------------------------

HRESULT R_HW_CreateTexture(IDirect3DDevice9 *object, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
{
    return object->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
}

HRESULT R_HW_CreateCubeTexture(IDirect3DDevice9 *object, UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle)
{
    return object->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
}

HRESULT R_HW_CreateVolumeTexture(IDirect3DDevice9 *object, UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle)
{
    return object->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
}

HRESULT R_HW_CreateVertexBuffer(IDirect3DDevice9 *object, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle)
{
    return object->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
}

HRESULT R_HW_CreateIndexBuffer(IDirect3DDevice9 *object, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle)
{
    return object->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
}

HRESULT R_HW_CreateRenderTarget(IDirect3DDevice9 *object, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
{
    return object->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
}

HRESULT R_HW_CreateDepthStencilSurface(IDirect3DDevice9 *object, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
{
    return object->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
}

HRESULT R_HW_CreateOffscreenPlainSurface(IDirect3DDevice9 *object, UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
{
    return object->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
}

HRESULT R_HW_CreateVertexShader(IDirect3DDevice9 *object, const DWORD *byte_code, IDirect3DVertexShader9 **shader)
{
    return object->CreateVertexShader(byte_code, shader);
}

HRESULT R_HW_CreatePixelShader(IDirect3DDevice9 *object, const DWORD *byte_code, IDirect3DPixelShader9 **shader)
{
    return object->CreatePixelShader(byte_code, shader);
}

HRESULT R_HW_CreateVertexDeclaration(IDirect3DDevice9 *object, const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **declaration)
{
    return object->CreateVertexDeclaration(elements, declaration);
}

HRESULT R_HW_CreateQuery(IDirect3DDevice9 *object, D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
{
    return object->CreateQuery(Type, ppQuery);
}

// ---- Resource access: lock, unlock and describe -----------------------------

HRESULT R_HW_Lock(IDirect3DIndexBuffer9 *object, UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags)
{
    return object->Lock(OffsetToLock, SizeToLock, ppbData, Flags);
}

HRESULT R_HW_Lock(IDirect3DVertexBuffer9 *object, UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags)
{
    return object->Lock(OffsetToLock, SizeToLock, ppbData, Flags);
}

HRESULT R_HW_Unlock(IDirect3DIndexBuffer9 *object)
{
    return object->Unlock();
}

HRESULT R_HW_Unlock(IDirect3DVertexBuffer9 *object)
{
    return object->Unlock();
}

HRESULT R_HW_LockRect(IDirect3DCubeTexture9 *object, D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags)
{
    return object->LockRect(face, level, locked_rect, rect, flags);
}

HRESULT R_HW_LockRect(IDirect3DSurface9 *object, D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags)
{
    return object->LockRect(locked_rect, rect, flags);
}

HRESULT R_HW_LockRect(IDirect3DTexture9 *object, UINT level, D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags)
{
    return object->LockRect(level, locked_rect, rect, flags);
}

HRESULT R_HW_UnlockRect(IDirect3DCubeTexture9 *object, D3DCUBEMAP_FACES FaceType, UINT Level)
{
    return object->UnlockRect(FaceType, Level);
}

HRESULT R_HW_UnlockRect(IDirect3DSurface9 *object)
{
    return object->UnlockRect();
}

HRESULT R_HW_UnlockRect(IDirect3DTexture9 *object, UINT Level)
{
    return object->UnlockRect(Level);
}

HRESULT R_HW_LockBox(IDirect3DVolume9 *object, D3DLOCKED_BOX *locked_box, const D3DBOX *box, DWORD flags)
{
    return object->LockBox(locked_box, box, flags);
}

HRESULT R_HW_LockBox(IDirect3DVolumeTexture9 *object, UINT level, D3DLOCKED_BOX *locked_box, const D3DBOX *box, DWORD flags)
{
    return object->LockBox(level, locked_box, box, flags);
}

HRESULT R_HW_UnlockBox(IDirect3DVolume9 *object)
{
    return object->UnlockBox();
}

HRESULT R_HW_UnlockBox(IDirect3DVolumeTexture9 *object, UINT Level)
{
    return object->UnlockBox(Level);
}

HRESULT R_HW_AddDirtyBox(IDirect3DVolumeTexture9 *object, const D3DBOX *dirty_box)
{
    return object->AddDirtyBox(dirty_box);
}

HRESULT R_HW_GetLevelDesc(IDirect3DCubeTexture9 *object, UINT Level, D3DSURFACE_DESC* pDesc)
{
    return object->GetLevelDesc(Level, pDesc);
}

HRESULT R_HW_GetLevelDesc(IDirect3DTexture9 *object, UINT Level, D3DSURFACE_DESC* pDesc)
{
    return object->GetLevelDesc(Level, pDesc);
}

HRESULT R_HW_GetLevelDesc(IDirect3DVolumeTexture9 *object, UINT Level, D3DVOLUME_DESC *pDesc)
{
    return object->GetLevelDesc(Level, pDesc);
}

HRESULT R_HW_GetSurfaceLevel(IDirect3DTexture9 *object, UINT Level, IDirect3DSurface9** ppSurfaceLevel)
{
    return object->GetSurfaceLevel(Level, ppSurfaceLevel);
}

HRESULT R_HW_GetDesc(IDirect3DIndexBuffer9 *object, D3DINDEXBUFFER_DESC* pDesc)
{
    return object->GetDesc(pDesc);
}

HRESULT R_HW_GetDesc(IDirect3DSurface9 *object, D3DSURFACE_DESC* pDesc)
{
    return object->GetDesc(pDesc);
}

HRESULT R_HW_GetDesc(IDirect3DVertexBuffer9 *object, D3DVERTEXBUFFER_DESC* pDesc)
{
    return object->GetDesc(pDesc);
}

HRESULT R_HW_GetDesc(IDirect3DVolume9 *object, D3DVOLUME_DESC* pDesc)
{
    return object->GetDesc(pDesc);
}

// ---- Copies, clears and readback --------------------------------------------

HRESULT R_HW_Clear(IDirect3DDevice9 *object, DWORD rect_count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    return object->Clear(rect_count, rects, flags, color, z, stencil);
}

HRESULT R_HW_StretchRect(IDirect3DDevice9 *object, IDirect3DSurface9 *src_surface, const RECT *src_rect, IDirect3DSurface9 *dst_surface, const RECT *dst_rect, D3DTEXTUREFILTERTYPE filter)
{
    return object->StretchRect(src_surface, src_rect, dst_surface, dst_rect, filter);
}

HRESULT R_HW_UpdateTexture(IDirect3DDevice9 *object, IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture)
{
    return object->UpdateTexture(pSourceTexture, pDestinationTexture);
}

HRESULT R_HW_GetRenderTargetData(IDirect3DDevice9 *object, IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface)
{
    return object->GetRenderTargetData(pRenderTarget, pDestSurface);
}

HRESULT R_HW_GetFrontBufferData(IDirect3DDevice9 *object, UINT iSwapChain, IDirect3DSurface9* pDestSurface)
{
    return object->GetFrontBufferData(iSwapChain, pDestSurface);
}

HRESULT R_HW_GetFrontBufferData(IDirect3DSwapChain9 *object, struct IDirect3DSurface9 *pDestSurface)
{
    return object->GetFrontBufferData(pDestSurface);
}

// ---- Draws and queries ------------------------------------------------------

HRESULT R_HW_DrawPrimitiveUP(IDirect3DDevice9 *object, D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void *data, UINT stride)
{
    return object->DrawPrimitiveUP(primitive_type, primitive_count, data, stride);
}

HRESULT R_HW_GetRenderState(IDirect3DDevice9 *object, D3DRENDERSTATETYPE State, DWORD* pValue)
{
    return object->GetRenderState(State, pValue);
}

HRESULT R_HW_Issue(IDirect3DQuery9 *object, DWORD dwIssueFlags)
{
    return object->Issue(dwIssueFlags);
}

HRESULT R_HW_GetData(IDirect3DQuery9 *object, void* pData, DWORD dwSize, DWORD dwGetDataFlags)
{
    return object->GetData(pData, dwSize, dwGetDataFlags);
}

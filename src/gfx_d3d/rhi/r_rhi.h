#pragma once

// The renderer's one interface to the graphics API.
//
// Every Direct3D 9 call the renderer (src/gfx_d3d) makes goes through a function
// declared here; the implementation is r_rhi_d3d9_*.cpp in this directory, and
// no other renderer unit calls a D3D9 method. scripts/tests/check_rhi_boundary.py
// enforces that in CI.
//
// This is step M1a of the renderer plan (the mechanical half of the seam): the
// calls moved out of the renderer verbatim, so the D3D9 build renders exactly as
// before and r_logFile prints the same lines. The signatures still use D3D9
// types; M1b replaces them with backend handles once the shader spike (S0) has
// settled what the Vulkan Legacy adapter needs. Names follow the engine's own
// R_HW_* convention, which most of these functions already had.

#include "../rb_backend.h"

struct MaterialPixelShader;
struct tagRECT; // RECT: Windows, and the native D3D9 boundary on Linux
struct MaterialVertexShader;

// ---- Draws and vertex input (r_rhi_d3d9_state.cpp) -----------------------------
void R_HW_DrawIndexedPrimitive(IDirect3DDevice9 *device, const GfxDrawPrimArgs *args, int triCount);
void R_HW_SetIndices(IDirect3DDevice9 *device, IDirect3DIndexBuffer9 *ib);
void R_HW_SetStreamSource(
    IDirect3DDevice9 *device,
    uint32_t streamIndex,
    IDirect3DVertexBuffer9 *vb,
    uint32_t vertexOffset,
    uint32_t vertexStride);
void R_HW_SetVertexDeclaration(IDirect3DDevice9 *device, IDirect3DVertexDeclaration9 *decl);

// ---- Scissor -------------------------------------------------------------------
void R_HW_EnableScissor(IDirect3DDevice9 *device, const tagRECT *rect);
void R_HW_DisableScissor(IDirect3DDevice9 *device);

// ---- Fixed-function state ------------------------------------------------------
void R_SetInitialContextState(IDirect3DDevice9 *device);
void R_HW_SetViewport(IDirect3DDevice9 *device, const GfxViewport *viewport, float nearValue, float farValue);
void R_HW_SetAlphaTestEnable(IDirect3DDevice9 *device, __int16 stateBits0);
void R_HW_SetColorMask(IDirect3DDevice9 *device, uint32_t stateBits0);
void R_HW_SetCullFace(IDirect3DDevice9 *device, __int16 stateBits0);
void R_HW_SetPolygonMode(IDirect3DDevice9 *device, signed int stateBits0);
void R_HW_DisableBlend(IDirect3DDevice9 *device);
void R_HW_SetBlend(
    IDirect3DDevice9 *device,
    bool blendWasEnabled,
    uint32_t changedBits,
    uint32_t stateBits0);
void R_SetAlphaTestFunction(GfxCmdBufState *state, __int16 stateBits0);
void R_HW_SetDepthWriteEnable(IDirect3DDevice9 *device, char stateBits1);
void R_HW_SetDepthTestEnable(IDirect3DDevice9 *device, char stateBits1);
void R_HW_SetDepthTestFunction(IDirect3DDevice9 *device, char stateBits1);
void R_HW_EnableStencil(IDirect3DDevice9 *device);
void R_HW_DisableStencil(IDirect3DDevice9 *device);
void R_HW_SetFrontStencilOp(
    IDirect3DDevice9 *device,
    uint32_t stencilOpPass,
    uint32_t stencilOpFail,
    uint32_t stencilOpZFail);
void R_HW_SetBackStencilOp(
    IDirect3DDevice9 *device,
    uint32_t stencilOpPass,
    uint32_t stencilOpFail,
    uint32_t stencilOpZFail);
void R_HW_SetFrontStencilFunc(IDirect3DDevice9 *device, uint32_t stencilFunc);
void R_HW_SetBackStencilFunc(IDirect3DDevice9 *device, uint32_t stencilFunc);
uint32_t R_HW_SetSamplerState(
    IDirect3DDevice9 *device,
    uint32_t samplerIndex,
    uint32_t samplerState,
    uint32_t oldSamplerState);
void R_HW_DisableSampler(IDirect3DDevice9 *device, uint32_t samplerIndex);
void R_HW_SetRenderTarget(GfxCmdBufState *state, GfxRenderTargetId newTargetId);
void R_ClearScreenInternal(
    IDirect3DDevice9 *device,
    uint8_t whichToClear,
    const float *color,
    float depth,
    uint8_t stencil,
    const GfxViewport *viewport);
void R_HW_SetPolygonOffset(IDirect3DDevice9 *device, float scale, float bias);
void R_SetAlphaAntiAliasingState(IDirect3DDevice9 *device, __int16 stateBits0);
void R_SetDefaultAlphaTestFunction(GfxCmdBufState *state);
void R_HW_ForceSamplerState(IDirect3DDevice9 *device, uint32_t samplerIndex, uint32_t samplerState);

// ---- Shaders, constants and textures -------------------------------------------
void R_HW_SetVertexShaderConstant(
    IDirect3DDevice9 *device,
    uint32_t dest,
    const float *data,
    uint32_t rowCount);
void R_HW_SetPixelShaderConstant(
    IDirect3DDevice9 *device,
    uint32_t dest,
    const float *data,
    uint32_t rowCount);
void R_HW_SetPixelShader(IDirect3DDevice9 *device, const MaterialPixelShader *mtlShader);
void R_HW_SetVertexShader(IDirect3DDevice9 *device, const MaterialVertexShader *mtlShader);
void R_HW_SetSamplerTexture(IDirect3DDevice9 *device, uint32_t samplerIndex, const GfxTexture *texture);

// ---- D3D9-shaped calls (r_rhi_d3d9_calls.cpp) ------------------------------------
//
// One typed forwarder per Direct3D 9 interface method the renderer still calls
// directly: `object->Method(args)` became `R_HW_Method(object, args)`. They pass
// their arguments through and return the method's result. There is one overload
// per d3d9.h interface the renderer calls the method on.
//
// Only units that see the full D3D9 headers get these: Windows, and Linux units
// compiled through the native D3D9 boundary. Units built with the stand-in D3D9
// types (src/linux/d3d9_types.h) cannot call D3D9 methods and do not need them.
#if !defined(__linux__) || defined(KISAK_LINUX_NATIVE_D3D9)

// ---- Reference counts -------------------------------------------------------
ULONG R_HW_Release(IDirect3D9 *object);
ULONG R_HW_Release(IDirect3DBaseTexture9 *object);
ULONG R_HW_Release(IDirect3DCubeTexture9 *object);
ULONG R_HW_Release(IDirect3DDevice9 *object);
ULONG R_HW_Release(IDirect3DIndexBuffer9 *object);
ULONG R_HW_Release(IDirect3DPixelShader9 *object);
ULONG R_HW_Release(IDirect3DQuery9 *object);
ULONG R_HW_Release(IDirect3DResource9 *object);
ULONG R_HW_Release(IDirect3DStateBlock9 *object);
ULONG R_HW_Release(IDirect3DSurface9 *object);
ULONG R_HW_Release(IDirect3DSwapChain9 *object);
ULONG R_HW_Release(IDirect3DTexture9 *object);
ULONG R_HW_Release(IDirect3DVertexBuffer9 *object);
ULONG R_HW_Release(IDirect3DVertexDeclaration9 *object);
ULONG R_HW_Release(IDirect3DVertexShader9 *object);
ULONG R_HW_Release(IDirect3DVolume9 *object);
ULONG R_HW_Release(IDirect3DVolumeTexture9 *object);
ULONG R_HW_AddRef(IDirect3D9 *object);
ULONG R_HW_AddRef(IDirect3DBaseTexture9 *object);
ULONG R_HW_AddRef(IDirect3DCubeTexture9 *object);
ULONG R_HW_AddRef(IDirect3DDevice9 *object);
ULONG R_HW_AddRef(IDirect3DIndexBuffer9 *object);
ULONG R_HW_AddRef(IDirect3DPixelShader9 *object);
ULONG R_HW_AddRef(IDirect3DQuery9 *object);
ULONG R_HW_AddRef(IDirect3DResource9 *object);
ULONG R_HW_AddRef(IDirect3DStateBlock9 *object);
ULONG R_HW_AddRef(IDirect3DSurface9 *object);
ULONG R_HW_AddRef(IDirect3DSwapChain9 *object);
ULONG R_HW_AddRef(IDirect3DTexture9 *object);
ULONG R_HW_AddRef(IDirect3DVertexBuffer9 *object);
ULONG R_HW_AddRef(IDirect3DVertexDeclaration9 *object);
ULONG R_HW_AddRef(IDirect3DVertexShader9 *object);
ULONG R_HW_AddRef(IDirect3DVolume9 *object);
ULONG R_HW_AddRef(IDirect3DVolumeTexture9 *object);

// ---- The Direct3D 9 object: adapters, formats and the device ----------------
IDirect3D9 *R_HW_Direct3DCreate9(UINT SDKVersion);
HRESULT R_HW_CheckDeviceFormat(IDirect3D9 *object, UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat);
HRESULT R_HW_CheckDepthStencilMatch(IDirect3D9 *object, UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat);
HRESULT R_HW_CheckDeviceMultiSampleType(IDirect3D9 *object, UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels);
UINT R_HW_GetAdapterCount(IDirect3D9 *object);
HMONITOR R_HW_GetAdapterMonitor(IDirect3D9 *object, UINT Adapter);
HRESULT R_HW_GetAdapterIdentifier(IDirect3D9 *object, UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier);
UINT R_HW_GetAdapterModeCount(IDirect3D9 *object, UINT Adapter, D3DFORMAT Format);
HRESULT R_HW_EnumAdapterModes(IDirect3D9 *object, UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode);
HRESULT R_HW_GetAdapterDisplayMode(IDirect3D9 *object, UINT Adapter, D3DDISPLAYMODE* pMode);
HRESULT R_HW_GetDeviceCaps(IDirect3D9 *object, UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps);
HRESULT R_HW_GetDeviceCaps(IDirect3DDevice9 *object, D3DCAPS9* pCaps);
HRESULT R_HW_CreateDevice(IDirect3D9 *object, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, struct IDirect3DDevice9** ppReturnedDeviceInterface);

// ---- Device lifetime and frames ---------------------------------------------
HRESULT R_HW_TestCooperativeLevel(IDirect3DDevice9 *object);
HRESULT R_HW_Reset(IDirect3DDevice9 *object, D3DPRESENT_PARAMETERS* pPresentationParameters);
HRESULT R_HW_BeginScene(IDirect3DDevice9 *object);
HRESULT R_HW_EndScene(IDirect3DDevice9 *object);
HRESULT R_HW_Present(IDirect3DDevice9 *object, const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region);
HRESULT R_HW_Present(IDirect3DSwapChain9 *object, const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region, DWORD flags);
HRESULT R_HW_CreateAdditionalSwapChain(IDirect3DDevice9 *object, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain);
HRESULT R_HW_GetSwapChain(IDirect3DDevice9 *object, UINT iSwapChain, IDirect3DSwapChain9** pSwapChain);
HRESULT R_HW_GetBackBuffer(IDirect3DDevice9 *object, UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer);
HRESULT R_HW_GetBackBuffer(IDirect3DSwapChain9 *object, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, struct IDirect3DSurface9 **ppBackBuffer);
void R_HW_SetGammaRamp(IDirect3DDevice9 *object, UINT swapchain_idx, DWORD flags, const D3DGAMMARAMP *ramp);
UINT R_HW_GetAvailableTextureMem(IDirect3DDevice9 *object);

// ---- Resource creation ------------------------------------------------------
HRESULT R_HW_CreateTexture(IDirect3DDevice9 *object, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle);
HRESULT R_HW_CreateCubeTexture(IDirect3DDevice9 *object, UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle);
HRESULT R_HW_CreateVolumeTexture(IDirect3DDevice9 *object, UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle);
HRESULT R_HW_CreateVertexBuffer(IDirect3DDevice9 *object, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle);
HRESULT R_HW_CreateIndexBuffer(IDirect3DDevice9 *object, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle);
HRESULT R_HW_CreateRenderTarget(IDirect3DDevice9 *object, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle);
HRESULT R_HW_CreateDepthStencilSurface(IDirect3DDevice9 *object, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle);
HRESULT R_HW_CreateOffscreenPlainSurface(IDirect3DDevice9 *object, UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle);
HRESULT R_HW_CreateVertexShader(IDirect3DDevice9 *object, const DWORD *byte_code, IDirect3DVertexShader9 **shader);
HRESULT R_HW_CreatePixelShader(IDirect3DDevice9 *object, const DWORD *byte_code, IDirect3DPixelShader9 **shader);
HRESULT R_HW_CreateVertexDeclaration(IDirect3DDevice9 *object, const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **declaration);
HRESULT R_HW_CreateQuery(IDirect3DDevice9 *object, D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery);

// ---- Resource access: lock, unlock and describe -----------------------------
HRESULT R_HW_Lock(IDirect3DIndexBuffer9 *object, UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags);
HRESULT R_HW_Lock(IDirect3DVertexBuffer9 *object, UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags);
HRESULT R_HW_Unlock(IDirect3DIndexBuffer9 *object);
HRESULT R_HW_Unlock(IDirect3DVertexBuffer9 *object);
HRESULT R_HW_LockRect(IDirect3DCubeTexture9 *object, D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags);
HRESULT R_HW_LockRect(IDirect3DSurface9 *object, D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags);
HRESULT R_HW_LockRect(IDirect3DTexture9 *object, UINT level, D3DLOCKED_RECT *locked_rect, const RECT *rect, DWORD flags);
HRESULT R_HW_UnlockRect(IDirect3DCubeTexture9 *object, D3DCUBEMAP_FACES FaceType, UINT Level);
HRESULT R_HW_UnlockRect(IDirect3DSurface9 *object);
HRESULT R_HW_UnlockRect(IDirect3DTexture9 *object, UINT Level);
HRESULT R_HW_LockBox(IDirect3DVolume9 *object, D3DLOCKED_BOX *locked_box, const D3DBOX *box, DWORD flags);
HRESULT R_HW_LockBox(IDirect3DVolumeTexture9 *object, UINT level, D3DLOCKED_BOX *locked_box, const D3DBOX *box, DWORD flags);
HRESULT R_HW_UnlockBox(IDirect3DVolume9 *object);
HRESULT R_HW_UnlockBox(IDirect3DVolumeTexture9 *object, UINT Level);
HRESULT R_HW_AddDirtyBox(IDirect3DVolumeTexture9 *object, const D3DBOX *dirty_box);
HRESULT R_HW_GetLevelDesc(IDirect3DCubeTexture9 *object, UINT Level, D3DSURFACE_DESC* pDesc);
HRESULT R_HW_GetLevelDesc(IDirect3DTexture9 *object, UINT Level, D3DSURFACE_DESC* pDesc);
HRESULT R_HW_GetLevelDesc(IDirect3DVolumeTexture9 *object, UINT Level, D3DVOLUME_DESC *pDesc);
HRESULT R_HW_GetSurfaceLevel(IDirect3DTexture9 *object, UINT Level, IDirect3DSurface9** ppSurfaceLevel);
HRESULT R_HW_GetDesc(IDirect3DIndexBuffer9 *object, D3DINDEXBUFFER_DESC* pDesc);
HRESULT R_HW_GetDesc(IDirect3DSurface9 *object, D3DSURFACE_DESC* pDesc);
HRESULT R_HW_GetDesc(IDirect3DVertexBuffer9 *object, D3DVERTEXBUFFER_DESC* pDesc);
HRESULT R_HW_GetDesc(IDirect3DVolume9 *object, D3DVOLUME_DESC* pDesc);

// ---- Copies, clears and readback --------------------------------------------
HRESULT R_HW_Clear(IDirect3DDevice9 *object, DWORD rect_count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil);
HRESULT R_HW_StretchRect(IDirect3DDevice9 *object, IDirect3DSurface9 *src_surface, const RECT *src_rect, IDirect3DSurface9 *dst_surface, const RECT *dst_rect, D3DTEXTUREFILTERTYPE filter);
HRESULT R_HW_UpdateTexture(IDirect3DDevice9 *object, IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture);
HRESULT R_HW_GetRenderTargetData(IDirect3DDevice9 *object, IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface);
HRESULT R_HW_GetFrontBufferData(IDirect3DDevice9 *object, UINT iSwapChain, IDirect3DSurface9* pDestSurface);
HRESULT R_HW_GetFrontBufferData(IDirect3DSwapChain9 *object, struct IDirect3DSurface9 *pDestSurface);

// ---- Draws and queries ------------------------------------------------------
HRESULT R_HW_DrawPrimitiveUP(IDirect3DDevice9 *object, D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void *data, UINT stride);
HRESULT R_HW_GetRenderState(IDirect3DDevice9 *object, D3DRENDERSTATETYPE State, DWORD* pValue);
HRESULT R_HW_Issue(IDirect3DQuery9 *object, DWORD dwIssueFlags);
HRESULT R_HW_GetData(IDirect3DQuery9 *object, void* pData, DWORD dwSize, DWORD dwGetDataFlags);
#endif // !defined(__linux__) || defined(KISAK_LINUX_NATIVE_D3D9)

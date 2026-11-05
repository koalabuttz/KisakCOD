// Windows ARM64 builds only (scripts/common_files.cmake).
//
// D3DX9 from the June 2010 DirectX SDK ships x86 and x64 libraries only, so
// ARM64 cannot link d3dx9.lib. The engine calls D3DX in one place:
// r_material_load_obj.cpp, which compiles loose HLSL shaders when maps are
// loaded from raw assets (useFastFile 0). Fastfile loading, the normal path,
// never reaches it. These definitions take the D3DX entry points that file
// uses and fail them, so raw-asset shader compiles report an error instead of
// the build failing to link. The Linux client leaves that path out the same way.
#include <universal/q_shared.h>
#include <d3dx9shader.h>

#include <qcommon/qcommon.h>

namespace
{
void ReportD3DXUnavailable(const char *function)
{
    static bool reported;
    if (reported)
        return;
    reported = true;
    Com_PrintError(CON_CHANNEL_GFX,
                   "%s: D3DX9 is not available on Windows ARM64, so shaders cannot be compiled from raw assets. "
                   "Load the game from fastfiles.\n",
                   function);
}
} // namespace

HRESULT WINAPI D3DXCreateBuffer(DWORD NumBytes, LPD3DXBUFFER *ppBuffer)
{
    (void)NumBytes;
    ReportD3DXUnavailable("D3DXCreateBuffer");
    if (ppBuffer)
        *ppBuffer = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DXCompileShader(LPCSTR pSrcData, UINT SrcDataLen, CONST D3DXMACRO *pDefines,
                                 LPD3DXINCLUDE pInclude, LPCSTR pFunctionName, LPCSTR pProfile, DWORD Flags,
                                 LPD3DXBUFFER *ppShader, LPD3DXBUFFER *ppErrorMsgs,
                                 LPD3DXCONSTANTTABLE *ppConstantTable)
{
    (void)pSrcData;
    (void)SrcDataLen;
    (void)pDefines;
    (void)pInclude;
    (void)pFunctionName;
    (void)pProfile;
    (void)Flags;
    ReportD3DXUnavailable("D3DXCompileShader");
    if (ppShader)
        *ppShader = nullptr;
    if (ppErrorMsgs)
        *ppErrorMsgs = nullptr;
    if (ppConstantTable)
        *ppConstantTable = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DXGetShaderConstantTable(CONST DWORD *pFunction, LPD3DXCONSTANTTABLE *ppConstantTable)
{
    (void)pFunction;
    ReportD3DXUnavailable("D3DXGetShaderConstantTable");
    if (ppConstantTable)
        *ppConstantTable = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DXGetShaderInputSemantics(CONST DWORD *pFunction, D3DXSEMANTIC *pSemantics, UINT *pCount)
{
    (void)pFunction;
    (void)pSemantics;
    ReportD3DXUnavailable("D3DXGetShaderInputSemantics");
    if (pCount)
        *pCount = 0;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DXGetShaderOutputSemantics(CONST DWORD *pFunction, D3DXSEMANTIC *pSemantics, UINT *pCount)
{
    (void)pFunction;
    (void)pSemantics;
    ReportD3DXUnavailable("D3DXGetShaderOutputSemantics");
    if (pCount)
        *pCount = 0;
    return E_NOTIMPL;
}

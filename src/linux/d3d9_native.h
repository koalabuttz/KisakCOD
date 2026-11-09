// d3d9_native.h -- the engine-facing boundary to DXVK Native D3D9 (i386, and x86_64
// for the opt-in 64-bit track).
//
// Reached only through <linux/d3d9_types.h> in a translation unit compiled with
// KISAK_LINUX_NATIVE_D3D9 (the opt-in kisak_linux_native_d3d9 CMake target sets
// it together with the DXVK include directory). It gives that unit the complete
// D3D9 interface declarations from DXVK Native 2.7.1, so unmodified engine code
// such as `dx.device->SetRenderState(...)` compiles against the real vtables and
// dispatches into libdxvk_d3d9.so.
//
// Why a firewall instead of a plain #include <d3d9.h>:
//
// DXVK's native windows_base.h is written for programs that have no Win32 layer
// of their own. The engine has one (src/linux/compat.h), and the two disagree:
//
//   1. Calling conventions. windows_base.h does `#define __stdcall` and
//      `#define WINAPI` (both empty). Left alone, every engine declaration after
//      the include would silently become cdecl in this translation unit while
//      the same function stays stdcall everywhere else: an ABI split that the
//      linker cannot see. GCC at least warns about the redefinition; Clang, where
//      __stdcall is a keyword, accepts the empty macro without a word. The
//      engine's macros are saved before the include and restored after it.
//
//      DXVK's own declarations, on the other hand, must keep DXVK's convention.
//      libdxvk_d3d9.so is built with STDMETHODCALLTYPE and WINAPI empty, so its
//      COM methods and Direct3DCreate9 are plain cdecl on i386. They are
//      declared while DXVK's empty definitions are in effect, which is exactly
//      what the library implements. Wrapping them in the engine's stdcall would
//      be the opposite mistake: a callee-pops/caller-pops mismatch on every call.
//
//   2. Base types. DXVK spells HWND/HINSTANCE/HMODULE as void *, WCHAR as the
//      host wchar_t (4 bytes on Linux) and POINT/RECT as their own structs; compat.h
//      spells them as the opaque handle structs, a 2-byte WCHAR and tagPOINT
//      with the Windows x86-32 layouts the engine persists. Those DXVK
//      typedefs are renamed to KisakDxvk* while the headers are read, so
//      DXVK's declarations keep the exact types libdxvk_d3d9.so was built with
//      and the engine's names keep the engine's meaning. Handles convert
//      implicitly to void * when the engine passes them to DXVK; nothing is
//      reinterpreted by the firewall itself. RECT, which compat.h does not
//      declare, keeps DXVK's layout under the Windows tag name tagRECT, because
//      that is the spelling engine code uses.
//
//      HMONITOR and HDC are handled in two phases. DXVK's windows_base.h
//      declares them as void * under the renamed names, and then d3d9.h is read
//      with the engine's names visible, so IDirect3D9::GetAdapterMonitor returns
//      the engine's HMONITOR and IDirect3DSurface9::GetDC/ReleaseDC take its HDC.
//      r_init.cpp stores the monitor in an HMONITOR__ * and compares it with
//      the one it chose itself; both are pointer-sized handles returned and
//      passed in registers or stack slots identically, so the declarations
//      differ only in the pointee name the engine sees.
//
//   3. Shared macros. compat.h's sys_fileio.h defines INVALID_HANDLE_VALUE and
//      the WAIT_* results with the same values but different spellings; the
//      engine's definitions are kept.
//
// What this is NOT:
//
//   * It does not change any serialized asset or script VM structure. Those are
//     defined in engine headers whose layouts are asserted (see
//     tests/linux_d3d9_boundary/engine_layout_check.cpp, which is compiled in
//     both modes against the same constants). Only translation units that opt in
//     see DXVK types at all; everything that only stores D3D9 pointers keeps the
//     stand-in declarations.
//   * It is not a renderer. Including it creates nothing; a device exists only
//     once engine code calls Direct3DCreate9 and IDirect3D9::CreateDevice.
//   * It does not include a window-system header. The DXVK WSI headers
//     (wsi/native_sdl2.h, wsi/native_sdl3.h) belong to the window host, which
//     hands DXVK an SDL_Window * cast to HWND. What it does add, last, is
//     <linux/win32_window.h>: declarations of the Win32 window, monitor and
//     process calls the renderer units make, so that they compile unmodified.
//     Only a target that links src/linux/client/window/win32_window_sdl3.cpp
//     can call them.

#pragma once

#if !defined(__linux__) && !defined(__APPLE__) && !defined(__vita__)
#error "src/linux/d3d9_native.h is the native Linux and macOS DXVK boundary only"
#endif
#if !defined(KISAK_LINUX_NATIVE_D3D9)
#error "d3d9_native.h requires KISAK_LINUX_NATIVE_D3D9: link the kisak_linux_native_d3d9 target"
#endif
#if !defined(KISAK_LINUX_D3D9_TYPES_DISPATCH)
#error "include <linux/d3d9_types.h> (or a gfx_d3d header), the single D3D9 entry point, not d3d9_native.h"
#endif
#if !defined(__i386__) && !defined(__x86_64__) && !defined(__aarch64__) && !defined(__vita__)
#error "the DXVK Native D3D9 boundary is validated for i386, x86_64, aarch64 and the PS Vita (armv7) only"
#endif

#include <linux/compat.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#if defined(_D3D9_H_) || defined(__WINE_D3D9TYPES_H) || defined(__WINE_D3D9CAPS_H)
#error "a D3D9 header was included before the boundary; include <linux/d3d9_types.h> first"
#endif

// --- 1. Save the engine's definitions that DXVK would overwrite ------------
#pragma push_macro("__stdcall")
#pragma push_macro("WINAPI")
#pragma push_macro("INVALID_HANDLE_VALUE")
#pragma push_macro("WAIT_OBJECT_0")
#pragma push_macro("WAIT_TIMEOUT")
#pragma push_macro("WAIT_FAILED")
#pragma push_macro("WAIT_ABANDONED")
#undef __stdcall
#undef WINAPI
#undef INVALID_HANDLE_VALUE
#undef WAIT_OBJECT_0
#undef WAIT_TIMEOUT
#undef WAIT_FAILED
#undef WAIT_ABANDONED

// --- 2. Rename DXVK's base typedefs that differ from compat.h ---------------
#define WCHAR KisakDxvkWCHAR
#define NWPSTR KisakDxvkNWPSTR
#define LPWSTR KisakDxvkLPWSTR
#define PWSTR KisakDxvkPWSTR
#define LPCWSTR KisakDxvkLPCWSTR
#define HWND KisakDxvkHWND
#define HINSTANCE KisakDxvkHINSTANCE
#define HMODULE KisakDxvkHMODULE
#define POINT KisakDxvkPOINT
#define LPPOINT KisakDxvkLPPOINT
// RECT is not in compat.h. DXVK declares it as `struct RECT`; the engine spells
// the Windows tag (`tagRECT rect;` in rb_draw3d.cpp and friends), so DXVK's
// struct is given the Windows tag name and RECT becomes the Windows typedef of it.
#define RECT tagRECT

// HMONITOR and HDC, phase one: windows_base.h declares DXVK's under these names.
#define HMONITOR KisakDxvkHMONITOR
#define HDC KisakDxvkHDC
#include <windows.h>
#undef HMONITOR
#undef HDC

// DXVK's pshpack4.h/poppack.h bracket d3d9types.h and d3d9caps.h with
// pack(push, 4)/pack(pop); Clang reports every such bracket in an included file.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpragma-pack"
#endif

// HMONITOR and HDC, phase two: windows.h is already read, so d3d9.h's are the
// engine's compat.h handle types.
#include <d3d9.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// --- 3. Restore the engine's names and conventions --------------------------
#undef WCHAR
#undef NWPSTR
#undef LPWSTR
#undef PWSTR
#undef LPCWSTR
#undef HWND
#undef HINSTANCE
#undef HMODULE
#undef POINT
#undef LPPOINT
#undef RECT
typedef struct tagRECT RECT;

#undef __stdcall
#undef WINAPI
#undef INVALID_HANDLE_VALUE
#undef WAIT_OBJECT_0
#undef WAIT_TIMEOUT
#undef WAIT_FAILED
#undef WAIT_ABANDONED
#pragma pop_macro("WAIT_ABANDONED")
#pragma pop_macro("WAIT_FAILED")
#pragma pop_macro("WAIT_TIMEOUT")
#pragma pop_macro("WAIT_OBJECT_0")
#pragma pop_macro("INVALID_HANDLE_VALUE")
#pragma pop_macro("WINAPI")
#pragma pop_macro("__stdcall")

// --- 4. Prove the firewall held, in every translation unit that uses it -----
namespace kisak_d3d9_boundary
{
// The engine's stdcall survived DXVK's `#define __stdcall`.
#if defined(__i386__)
using EngineStdcall = void(__attribute__((stdcall)) *)(int);
using EngineCdecl = void(__attribute__((cdecl)) *)(int);
using SpelledStdcall = void(__stdcall *)(int);
using SpelledWinapi = void(WINAPI *)(int);
static_assert(std::is_same_v<SpelledStdcall, EngineStdcall>,
              "__stdcall must still mean stdcall after the DXVK headers");
static_assert(std::is_same_v<SpelledWinapi, EngineStdcall>,
              "WINAPI must still mean stdcall for engine declarations after the DXVK headers");
static_assert(!std::is_same_v<EngineStdcall, EngineCdecl>, "the compiler distinguishes stdcall from cdecl");
#else
// x86_64 and aarch64 have one C calling convention, so stdcall and cdecl are
// the same type and the engine's spellings only have to survive as macros.
static_assert(std::is_same_v<void(__stdcall *)(int), void (*)(int)>, "__stdcall is the 64-bit C convention");
static_assert(std::is_same_v<void(WINAPI *)(int), void (*)(int)>, "WINAPI is the 64-bit C convention");
#endif

// DXVK's exports keep DXVK's (cdecl) convention, which is how libdxvk_d3d9.so
// implements them on i386.
static_assert(std::is_same_v<decltype(&Direct3DCreate9), IDirect3D9 *(__attribute__((cdecl)) *)(UINT)>,
              "Direct3DCreate9 is declared with DXVK Native's own (cdecl) convention");

// The engine's base types kept their compat.h meaning.
static_assert(std::is_same_v<HWND, struct HWND__ *>, "HWND keeps the engine's opaque handle type");
static_assert(std::is_same_v<HINSTANCE, struct HINSTANCE__ *>, "HINSTANCE keeps the engine's type");
static_assert(std::is_same_v<HMODULE, HINSTANCE>, "HMODULE keeps the engine's type");
static_assert(std::is_same_v<WCHAR, std::uint16_t> && sizeof(WCHAR) == 2, "WCHAR keeps the Windows 2-byte width");
static_assert(std::is_same_v<POINT, struct tagPOINT>, "POINT keeps the engine's tagPOINT");
static_assert(sizeof(POINT) == 8 && offsetof(POINT, y) == 4, "POINT keeps the Windows layout");
static_assert(std::is_same_v<RECT, struct tagRECT>, "RECT is the Windows typedef of tagRECT");
static_assert(sizeof(RECT) == 16 && offsetof(RECT, bottom) == 12, "RECT keeps the Windows layout");

// Types both sides declare must be the same type, not merely the same size.
static_assert(std::is_same_v<DWORD, std::uint32_t>, "DWORD agrees with DXVK");
static_assert(std::is_same_v<UINT, std::uint32_t>, "UINT agrees with DXVK");
static_assert(std::is_same_v<LONG, std::int32_t>, "LONG agrees with DXVK");
static_assert(std::is_same_v<BOOL, std::int32_t>, "BOOL agrees with DXVK");
static_assert(std::is_same_v<HRESULT, std::int32_t>, "HRESULT agrees with DXVK");
static_assert(std::is_same_v<HANDLE, void *>, "HANDLE agrees with DXVK");

// DXVK's renamed handle types are the ones its API takes; engine handles pass
// to them through the ordinary pointer conversion and nothing else.
static_assert(std::is_same_v<KisakDxvkHWND, void *>, "DXVK's HWND is void *");
static_assert(std::is_convertible_v<HWND, KisakDxvkHWND>, "engine HWND converts to DXVK's HWND");
static_assert(std::is_same_v<decltype(D3DPRESENT_PARAMETERS::hDeviceWindow), KisakDxvkHWND>,
              "D3DPRESENT_PARAMETERS keeps DXVK's own HWND type");

// HMONITOR and HDC: DXVK's own spellings are void *, and the D3D9 methods that
// name them use the engine's handle types (see the two phases above).
static_assert(std::is_same_v<HMONITOR, struct HMONITOR__ *>, "HMONITOR keeps the engine's opaque handle type");
static_assert(std::is_same_v<HDC, struct HDC__ *>, "HDC keeps the engine's opaque handle type");
static_assert(std::is_same_v<KisakDxvkHMONITOR, void *> && std::is_same_v<KisakDxvkHDC, void *>,
              "DXVK's HMONITOR and HDC are void *");
static_assert(sizeof(HMONITOR) == sizeof(KisakDxvkHMONITOR) && sizeof(HDC) == sizeof(KisakDxvkHDC),
              "the engine and DXVK monitor and DC handles are the same size");
static_assert(std::is_same_v<decltype(&IDirect3D9::GetAdapterMonitor), HMONITOR (IDirect3D9::*)(UINT)>,
              "IDirect3D9::GetAdapterMonitor returns the engine's HMONITOR");
static_assert(std::is_same_v<decltype(&IDirect3DSurface9::GetDC), HRESULT (IDirect3DSurface9::*)(HDC *)>,
              "IDirect3DSurface9::GetDC takes the engine's HDC");

// The macros the engine owns kept the engine's spelling and value.
static_assert(WAIT_OBJECT_0 == 0 && WAIT_TIMEOUT == 258, "engine WAIT_* values");

// Every enumerator the stand-in declares has the same value in DXVK, so a
// translation unit that only sees the stand-in agrees with one that sees DXVK.
#define KISAK_D3D9_CHECK_ENUMERATOR(name, value)                                              \
    static_assert(static_cast<long long>(name) == static_cast<long long>(value),            \
                  #name " differs between the Linux stand-in and DXVK Native");
KISAK_D3D9_FORMAT_VALUES(KISAK_D3D9_CHECK_ENUMERATOR)
KISAK_D3D9_MULTISAMPLE_VALUES(KISAK_D3D9_CHECK_ENUMERATOR)
KISAK_D3D9_TEXTUREFILTER_VALUES(KISAK_D3D9_CHECK_ENUMERATOR)
KISAK_D3D9_CUBEMAP_FACE_VALUES(KISAK_D3D9_CHECK_ENUMERATOR)
#undef KISAK_D3D9_CHECK_ENUMERATOR

// The structs and interface pointers the engine embeds by value have the same
// layout here as in the stand-in.
KISAK_D3D9_ASSERT_EMBEDDED_LAYOUTS();
} // namespace kisak_d3d9_boundary

// --- 5. The Win32 window surface the renderer units call ---------------------
#include <linux/win32_window.h>

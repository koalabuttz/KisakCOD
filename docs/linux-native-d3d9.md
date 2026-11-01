# Native D3D9 engine boundary (DXVK Native, i386)

This is the one build boundary through which engine renderer code reaches
DXVK Native 2.7.1 D3D9 on 32-bit Linux. It is opt-in and off by default. It
compiles engine code against the real D3D9 interfaces. It does **not** create a
device, draw anything, or link a renderer into any executable. The next milestone
(a static genuine map view through the engine renderer) builds on top of it.

## The problem it solves

DXVK Native ships its own `windows_base.h`, written for programs with no Win32
layer of their own. The engine has one (`src/linux/compat.h`), and the two
disagree in ways that break the build or, worse, break the ABI silently:

| DXVK `windows_base.h` | Engine `compat.h` | What a plain `#include <d3d9.h>` would do |
| --- | --- | --- |
| `#define __stdcall` (empty) | `__stdcall` = `__attribute__((stdcall))` (GCC) / keyword (Clang) | every engine declaration after the include becomes cdecl in that unit only. GCC warns; **Clang accepts it silently** |
| `#define WINAPI` (empty) | `WINAPI` = `__stdcall` | same split for `WINAPI`/thread entry points |
| `HWND`, `HINSTANCE`, `HMODULE` = `void *` | `HWND__ *`, `HINSTANCE__ *` | typedef conflict (`DxGlobals` stores `HWND__ *`/`HINSTANCE__ *`) |
| `WCHAR` = `wchar_t` (4 bytes) | `WCHAR` = `uint16_t` (Windows width) | typedef conflict; a layout change if it ever won |
| `struct POINT` | `struct tagPOINT` + `typedef POINT` | conflict |
| `INVALID_HANDLE_VALUE`, `WAIT_*` | same values, different spelling (`sys_fileio.h`) | macro redefinition |

The opposite mistake is just as bad: `libdxvk_d3d9.so` is built with
`STDMETHODCALLTYPE` and `WINAPI` empty, so on i386 its COM methods and
`Direct3DCreate9` are **cdecl**. Declaring them with the engine's stdcall would
make callee-cleanup and caller-cleanup disagree on every call.

## Design

```
gfx_d3d/*.h (unchanged)
   └── #include <linux/d3d9_types.h>          single entry point, per translation unit
          ├── default:  type-only stand-in       opaque interface tags, D3DFORMAT-family enums,
          │                                      D3DDISPLAYMODE (what the dedicated server sees)
          └── KISAK_LINUX_NATIVE_D3D9:
                 └── <linux/d3d9_native.h>       firewall around DXVK's <d3d9.h>
```

`src/linux/d3d9_native.h`:

1. saves the engine's `__stdcall`, `WINAPI`, `INVALID_HANDLE_VALUE` and `WAIT_*`
   (`#pragma push_macro`) and undefines them;
2. renames DXVK's `WCHAR`, `LPWSTR`, `PWSTR`, `NWPSTR`, `LPCWSTR`, `HWND`,
   `HINSTANCE`, `HMODULE`, `POINT`, `LPPOINT` to `KisakDxvk*` while DXVK's headers
   are read, so DXVK's declarations keep exactly the types `libdxvk_d3d9.so` was
   built with. DXVK's `struct RECT` is given the Windows tag `tagRECT`, and `RECT`
   becomes the Windows typedef of it, because engine code spells `tagRECT`;
3. includes DXVK's `<d3d9.h>` with DXVK's own (empty) conventions in effect;
4. undoes the renames and restores the engine's macros (`#pragma pop_macro`);
5. static_asserts, in every unit that uses it: `__stdcall` and `WINAPI` still
   mean stdcall; `Direct3DCreate9` is cdecl; `HWND`/`HINSTANCE`/`WCHAR`/`POINT`
   keep the engine's types and Windows layouts; `DWORD`/`UINT`/`LONG`/`BOOL`/
   `HRESULT`/`HANDLE` are the same types on both sides; `D3DPRESENT_PARAMETERS`
   keeps DXVK's `HWND`; **every enumerator value the stand-in declares equals
   DXVK's**; and the embedded layouts (`D3DDISPLAYMODE`, enum widths, interface
   pointers) match the stand-in.

DXVK's COM helper macros (`interface`, `STDMETHOD`, `PURE`, `THIS`, `FAILED`,
`SUCCEEDED`, `S_OK`, `D3DERR_*`, `__uuidof`, ...) stay defined in opt-in units, as
they would after `<windows.h>` and `<d3d9.h>` on Windows; only the names listed
above are saved, renamed or restored.

Engine handles reach DXVK through ordinary pointer conversion
(`HWND__ *` → `void *`); the firewall reinterprets nothing. The window host will
hand DXVK an `SDL_Window *` cast to `HWND`, which is what DXVK's SDL WSI expects;
the WSI headers (`wsi/native_sdl3.h`) are deliberately not included by the boundary.

Both views share one list of enumerator values (`KISAK_D3D9_*_VALUES` in
`d3d9_types.h`). The stand-in defines its enums from them; the native header checks
them against DXVK. The two views cannot drift apart without a compile error.

### What stays out of DXVK's reach

Only translation units compiled with `KISAK_LINUX_NATIVE_D3D9` see DXVK at all.
Everything that loads or lays out serialized assets (`database/`, the image and
material loaders), the script VM and the dedicated server keep the stand-in.
The D3D9-bearing engine structures (`GfxImage`, `GfxImageLoadDef`,
`MaterialVertexDeclaration`, `MaterialVertexStreamRouting`, shader programs,
`GfxVertexBufferState`, `GfxBuffers`, `GfxCmdBufPrimState`, `GfxCmdBufState`,
`GfxRenderTarget`, `DxGlobals`, ...) are checked against their documented x86-32
sizes and offsets **in both views**, and the two views' printed layouts must be
byte-identical (`tests/linux_d3d9_boundary/engine_layout_check.cpp`).

Units that see the stand-in and units that see DXVK disagree about one thing only:
whether the interfaces and the `_D3DFORMAT` family are complete/declared by DXVK.
Their values, sizes and pointer layouts are proven equal, which is what code that
only stores them needs.

### Not in scope

- D3DX. `r_material_load_obj.cpp` includes D3DX headers; DXVK Native ships no
  D3DX library. Shader compilation/loading needs its own decision.
- Win32 APIs that are not D3D9: `ddraw.h` (`r_texturemem.cpp`), `MONITORINFO`
  (`r_screenshot.cpp`), `Sleep`, `_itoa`, `_time64`, Bink headers. These are
  ordinary Linux porting work in the renderer TUs, not boundary work.
- Linking. No renderer unit is linked into an executable here, and
  `KISAK_DXVK_NATIVE_LIBRARY` is only passed through for future device-creating
  targets.

## Build

```sh
cmake -S . -B build-linux/native-d3d9-Debug -G Ninja \
  -DKISAK_PLATFORM=linux -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DKISAK_BUILD_LINUX_NATIVE_D3D9=ON \
  -DKISAK_DXVK_NATIVE_INCLUDE_DIR=/tmp/kisak-dxvk-prefix/include/dxvk
cmake --build build-linux/native-d3d9-Debug --target KisakCOD-native-d3d9-proofs
```

| CMake | Meaning |
| --- | --- |
| `KISAK_BUILD_LINUX_NATIVE_D3D9` (OFF) | creates the targets below; nothing else changes |
| `KISAK_DXVK_NATIVE_INCLUDE_DIR` | DXVK Native `<prefix>/include/dxvk` (must hold `d3d9.h`, `windows.h`, ...) |
| `KISAK_DXVK_NATIVE_LIBRARY` | optional i386 `libdxvk_d3d9.so`, linked only by targets that link the interface target |
| `kisak_linux_native_d3d9` | INTERFACE target: `KISAK_LINUX_NATIVE_D3D9` plus the DXVK include directory (as SYSTEM). **Renderer targets link this; nothing else should.** |
| `KisakCOD-native-d3d9-boundary-check` | the boundary header alone, native mode, `-Werror` |
| `KisakCOD-native-d3d9-layout-standin` / `-native` | engine layout prints for both views |
| `KisakCOD-native-d3d9-renderer-proof` | OBJECT library of the production units in `scripts/platform/linux/native_d3d9_renderer_sources.txt` |
| `KisakCOD-native-d3d9-proofs` | builds all four |

The DXVK install must be the i386 build of DXVK Native 2.7.1 (source revision
`c3dd74be6baec53786d4e064a572185b70347a17`); see
[tests/linux_renderer/README.md](../tests/linux_renderer/README.md) for building it.
For compile-only work the headers of a source checkout are enough (see below).

## Asset-free checks

```sh
python3 scripts/tests/test_native_d3d9_boundary_runner.py
python3 scripts/tests/run_native_d3d9_boundary_checks.py --config Debug --dxvk-source <dxvk-2.7.1-checkout>
python3 scripts/tests/run_native_d3d9_boundary_checks.py --config RelWithDebInfo --dxvk-include-dir <prefix>/include/dxvk
```

The runner needs DXVK's headers only (a source checkout with the
`include/native/directx` submodule is staged the way `meson install` lays it out,
and its version must be 2.7.1). It runs, per configuration:

| Check | What it proves |
| --- | --- |
| `boundary_standin-{gcc,clang}` | stand-in mode: interfaces incomplete, layouts hold; stdcall probes end in `ret $0x8`, the cdecl probe in `ret` |
| `boundary_native-{gcc,clang}` | native mode under `-Werror`: a local `IDirect3DQuery9` implementation is called through DXVK's vtable; D3D9 struct sizes (`D3DPRESENT_PARAMETERS` 0x38, `D3DCAPS9` 0x130, `D3DADAPTER_IDENTIFIER9` 0x44C, ...); engine `__stdcall`/`WINAPI`/`CALLBACK` probes still end in `ret $0x8` **after** DXVK's headers, and the DXVK COM method ends in `ret` |
| `reject_*-{gcc,clang}` | must not compile, for the stated reason: direct `d3d9_native.h` include, `<d3d9.h>` before the boundary, DXVK beside `compat.h` without the firewall, a D3D9 call without the opt-in |
| `engine_layout_{standin,native,compare}` | documented x86-32 sizes/offsets of the D3D9-bearing engine structs in both views, and identical prints |
| `renderer_proof-<unit>` | each listed production unit compiles unmodified (`-c`) to an ELF32/i386 object through the boundary |
| `renderer_standin_control` | every listed unit fails without the boundary, so the list stays a list of units that need the D3D9 API |

The `native_d3d9_boundary` job in `.github/workflows/build-kisakcod-linux.yaml`
fetches DXVK 2.7.1 and its `mingw-directx-headers` revision
(`9df86f2341616ef1888ae59919feaa6d4fad693d`) by pinned commit, runs the runner's
contract tests, the runner in Debug and RelWithDebInfo, then configures with the
option ON and builds `KisakCOD-native-d3d9-proofs`. It builds no DXVK, needs no GPU,
Vulkan, SDL or game data, and never builds or changes `KisakCOD-dedi`.

## Adding a renderer unit

1. Compile it with the boundary (`run_native_d3d9_boundary_checks.py`, or the
   CMake proof target) and fix any non-D3D9 Linux gaps in their own change.
2. Add it to `scripts/platform/linux/native_d3d9_renderer_sources.txt` only with
   a passing run of the runner in both configurations.
3. A unit that compiles with the stand-in does not need the boundary; keep it out
   of the list (the control check enforces this).

## Verification boundary

What the checks above establish: the boundary compiles with GCC 13 and Clang 18
for i386; it keeps the engine's calling conventions and layouts; 19 production
renderer units that call D3D9 compile unmodified through it. What they do not
establish: device creation from engine code, any draw, linking of renderer units,
behaviour on other DXVK versions, or anything on hardware. Those belong to the
static map view milestone and must be verified there.

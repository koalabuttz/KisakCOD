#!/usr/bin/env python3
"""Check that only the renderer's graphics interface calls Direct3D 9.

The renderer reaches the graphics API through one interface,
``src/gfx_d3d/rhi/r_rhi.h``; its Direct3D 9 implementation is the
``src/gfx_d3d/rhi/`` directory. This check scans every other renderer source
(``src/gfx_d3d/**/*.cpp`` and ``*.h`` outside ``rhi/``) for Direct3D 9 calls:

  * a method of a Direct3D 9 interface called through a pointer
    (``device->SetRenderState(``, ``(*fence)->Issue(``, ``texture->Release(``),
    matched by name against the methods ``d3d9.h`` declares;
  * the API's free functions (``Direct3DCreate9``, ``Direct3DCreate9Ex`` and the
    ``D3DPERF_*`` markers).

Comments and string literals are skipped, so the decompiled log strings
(``"device->SetIndices( ib )"``) and commented-out leftovers do not count.

The result is compared with ``scripts/tests/rhi_boundary_allowlist.txt``, the
reviewed list of calls that may stay outside the interface, each with a count
and a reason. The check is a two-way ratchet, like the client compile census:

  * a file with more calls than its entry allows (or calls and no entry) is a
    **regression**: a renderer file gained a direct D3D9 call;
  * a file with fewer calls than its entry says is **stale**: lower the count
    or delete the line in the same change.

Either one fails the run. This is a source scan by method name; it does not
compile anything. A name it matches on a non-D3D9 object (the D3DX shader
compiler's buffers are COM objects too) needs an allowlist entry that says so.

Usage:
    python3 scripts/tests/check_rhi_boundary.py            # check
    python3 scripts/tests/check_rhi_boundary.py --list     # print every call found
    python3 scripts/tests/check_rhi_boundary.py --root <repo>

Exit status: 0 when the scan matches the allowlist, 1 on a regression or a
stale entry, 2 on a usage error or an unreadable allowlist.
"""

import argparse
from collections import Counter
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
RENDERER = Path("src/gfx_d3d")
INTERFACE = RENDERER / "rhi"
ALLOWLIST = Path("scripts/tests/rhi_boundary_allowlist.txt")

# Every method the Direct3D 9 interfaces declare (d3d9.h: IDirect3D9(Ex),
# IDirect3DDevice9(Ex), the resources, surfaces, swap chains, queries, state
# blocks, shaders and vertex declarations, with IUnknown's three).
D3D9_METHODS = frozenset("""
AddDirtyBox AddDirtyRect AddRef Apply BeginScene BeginStateBlock Capture
CheckDepthStencilMatch CheckDeviceFormat CheckDeviceFormatConversion
CheckDeviceMultiSampleType CheckDeviceState CheckDeviceType
CheckResourceResidency Clear ColorFill ComposeRects CreateAdditionalSwapChain
CreateCubeTexture CreateDepthStencilSurface CreateDepthStencilSurfaceEx
CreateDevice CreateDeviceEx CreateIndexBuffer CreateOffscreenPlainSurface
CreateOffscreenPlainSurfaceEx CreatePixelShader CreateQuery CreateRenderTarget
CreateRenderTargetEx CreateStateBlock CreateTexture CreateVertexBuffer
CreateVertexDeclaration CreateVertexShader CreateVolumeTexture DeletePatch
DrawIndexedPrimitive DrawIndexedPrimitiveUP DrawPrimitive DrawPrimitiveUP
DrawRectPatch DrawTriPatch EndScene EndStateBlock EnumAdapterModes
EnumAdapterModesEx EvictManagedResources FreePrivateData GenerateMipSubLevels
GetAdapterCount GetAdapterDisplayMode GetAdapterDisplayModeEx
GetAdapterIdentifier GetAdapterLUID GetAdapterModeCount GetAdapterModeCountEx
GetAdapterMonitor GetAvailableTextureMem GetBackBuffer GetClipPlane
GetClipStatus GetContainer GetCreationParameters GetCubeMapSurface
GetCurrentTexturePalette GetDC GetData GetDataSize GetDeclaration
GetDepthStencilSurface GetDesc GetDevice GetDeviceCaps GetDirect3D
GetDisplayMode GetDisplayModeEx GetFVF GetFrontBufferData GetFunction
GetGPUThreadPriority GetGammaRamp GetIndices GetLOD GetLastPresentCount
GetLevelCount GetLevelDesc GetLight GetLightEnable GetMaterial
GetMaximumFrameLatency GetNPatchMode GetNumberOfSwapChains GetPaletteEntries
GetPixelShader GetPixelShaderConstantB GetPixelShaderConstantF
GetPixelShaderConstantI GetPresentParameters GetPresentStats GetPriority
GetPrivateData GetRasterStatus GetRenderState GetRenderTarget
GetRenderTargetData GetSamplerState GetScissorRect GetSoftwareVertexProcessing
GetStreamSource GetStreamSourceFreq GetSurfaceLevel GetSwapChain GetTexture
GetTextureStageState GetTransform GetVertexDeclaration GetVertexShader
GetVertexShaderConstantB GetVertexShaderConstantF GetVertexShaderConstantI
GetViewport GetVolumeLevel Issue LightEnable Lock LockBox LockRect
MultiplyTransform PreLoad Present PresentEx ProcessVertices QueryInterface
RegisterSoftwareDevice Release ReleaseDC Reset ResetEx SetAutoGenFilterType
SetClipPlane SetClipStatus SetConvolutionMonoKernel SetCurrentTexturePalette
SetCursorPosition SetCursorProperties SetDepthStencilSurface SetDialogBoxMode
SetFVF SetGPUThreadPriority SetGammaRamp SetIndices SetLOD SetLight
SetMaterial SetMaximumFrameLatency SetNPatchMode SetPaletteEntries
SetPixelShader SetPixelShaderConstantB SetPixelShaderConstantF
SetPixelShaderConstantI SetPriority SetPrivateData SetRenderState
SetRenderTarget SetSamplerState SetScissorRect SetSoftwareVertexProcessing
SetStreamSource SetStreamSourceFreq SetTexture SetTextureStageState
SetTransform SetVertexDeclaration SetVertexShader SetVertexShaderConstantB
SetVertexShaderConstantF SetVertexShaderConstantI SetViewport ShowCursor
StretchRect TestCooperativeLevel Unlock UnlockBox UnlockRect UpdateSurface
UpdateTexture ValidateDevice WaitForVBlank
""".split())

FREE_FUNCTIONS = re.compile(r"\b(Direct3DCreate9(?:Ex)?|D3DPERF_[A-Za-z]+)\s*\(")
METHOD_CALL = re.compile(r"->\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(")
# Comments, string and character literals: replaced by blanks of the same
# shape so line numbers stay right.
NOISE = re.compile(r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'""", re.DOTALL)


def blank(match):
    return re.sub(r"[^\n]", " ", match.group(0))


def scan_text(text):
    """Return [(line, name)] for each D3D9 call in one source text."""
    code = NOISE.sub(blank, text)
    calls = []
    for match in METHOD_CALL.finditer(code):
        if match.group(1) in D3D9_METHODS:
            calls.append((code.count("\n", 0, match.start()) + 1, match.group(1)))
    for match in FREE_FUNCTIONS.finditer(code):
        # Every use counts: the renderer calls these and never declares them.
        calls.append((code.count("\n", 0, match.start()) + 1, match.group(1)))
    return sorted(calls)


def renderer_sources(root):
    base = root / RENDERER
    interface = root / INTERFACE
    for path in sorted(base.rglob("*")):
        if path.suffix not in (".cpp", ".h") or not path.is_file():
            continue
        if interface in path.parents:
            continue
        yield path


def scan(root):
    """Return {relative path: [(line, name)]} for files with at least one call."""
    found = {}
    for path in renderer_sources(root):
        calls = scan_text(path.read_text(encoding="utf-8", errors="replace"))
        if calls:
            found[path.relative_to(root).as_posix()] = calls
    return found


def read_allowlist(path):
    """Parse `path: count: reason` lines. Returns ({path: count}, [problems])."""
    allowed, problems = {}, []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [part.strip() for part in line.split(":", 2)]
        if len(parts) != 3 or not parts[2]:
            problems.append(f"{path}:{number}: expected `path: count: reason`")
            continue
        source, count, _reason = parts
        if not count.isdigit() or int(count) == 0:
            problems.append(f"{path}:{number}: count must be a positive integer")
            continue
        if source in allowed:
            problems.append(f"{path}:{number}: {source} is listed twice")
            continue
        allowed[source] = int(count)
    return allowed, problems


def compare(found, allowed):
    """Return (regressions, stale) as lists of messages."""
    regressions, stale = [], []
    for source, calls in sorted(found.items()):
        limit = allowed.get(source, 0)
        if len(calls) > limit:
            names = Counter(name for _, name in calls)
            where = ", ".join(f"{line}:{name}" for line, name in calls[:8])
            more = "" if len(calls) <= 8 else f", ... ({len(calls) - 8} more)"
            regressions.append(
                f"{source}: {len(calls)} direct D3D9 call(s), allowlist allows {limit} "
                f"({', '.join(f'{n} x{c}' for n, c in names.most_common())}) at {where}{more}. "
                f"Call a function from {INTERFACE.as_posix()}/r_rhi.h instead.")
    for source, limit in sorted(allowed.items()):
        count = len(found.get(source, []))
        if count < limit:
            stale.append(f"{source}: allowlist allows {limit} call(s), the file has {count}; "
                         f"lower the count or delete the line in {ALLOWLIST.as_posix()}")
    return regressions, stale


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this checkout)")
    parser.add_argument("--allowlist", type=Path, help=f"allowlist file (default: <root>/{ALLOWLIST.as_posix()})")
    parser.add_argument("--list", action="store_true", help="print every call found and exit 0")
    options = parser.parse_args(argv)
    root = options.root.resolve()
    if not (root / RENDERER).is_dir():
        print(f"error: {root / RENDERER} is not a directory", file=sys.stderr)
        return 2

    found = scan(root)
    if options.list:
        for source, calls in sorted(found.items()):
            for line, name in calls:
                print(f"{source}:{line}: {name}")
        print(f"{sum(len(c) for c in found.values())} call(s) in {len(found)} file(s) outside {INTERFACE.as_posix()}")
        return 0

    allowlist = options.allowlist or root / ALLOWLIST
    try:
        allowed, problems = read_allowlist(allowlist)
    except OSError as error:
        print(f"error: cannot read the allowlist: {error}", file=sys.stderr)
        return 2
    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        return 2

    regressions, stale = compare(found, allowed)
    for message in regressions:
        print(f"REGRESSION {message}")
    for message in stale:
        print(f"STALE {message}")
    total = sum(len(c) for c in found.values())
    if regressions or stale:
        print(f"FAIL rhi boundary: {len(regressions)} regression(s), {len(stale)} stale entr(y/ies); "
              f"{total} allowed call(s) remain outside {INTERFACE.as_posix()}")
        return 1
    print(f"PASS rhi boundary: {total} allowlisted call(s) in {len(found)} file(s) outside "
          f"{INTERFACE.as_posix()}; no other renderer file calls Direct3D 9")
    return 0


if __name__ == "__main__":
    sys.exit(main())

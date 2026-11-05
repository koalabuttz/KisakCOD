# Native D3D9 device experiment — 2026-11-01

> Device-only checkpoint. The shader/surface gate described as next below has
> since passed in bounded standalone tests; see
> [renderer validation](linux-renderer-validation.md) and the
> [current status](status.md). This page preserves the device-run evidence.

## Result

The standalone `tests/linux_renderer` probe successfully created a native D3D9
HAL device through DXVK Native 2.7.1 in an ELF32 process, cleared/presented 180
frames, resized 640x360 to 800x480, reset immediately after resize, performed a
separate same-size reset, checked back-buffer dimensions, and shut down cleanly.
Parent runs passed **23 checks, two resets, four back-buffer checks** on native
Wayland and the current desktop X11 path. These are API/presentation lifecycle
results, not independent screenshot comparisons or shader/world-rendering proof.

No Wine, game executable, licensed assets, game shaders, geometry or engine
compatibility headers are used. The root build and game renderer are unchanged.

## Pinned dependencies and hardware

- DXVK Native v2.7.1, source `c3dd74be6baec53786d4e064a572185b70347a17`,
  recursive submodules; D3D9 and SDL3 WSI only. Private GCC 14 multilib build.
- SDL3 Debian i386 `3.2.10+ds-1`, private SDK. Native DXVK headers intentionally
  remain separate from the engine's Win32 calling-convention compatibility layer.
- An actual ELF32 `vulkaninfo` enumerated Intel Graphics (RPL-U), integrated GPU,
  Mesa ANV `25.0.7-2+deb13u1`, device Vulkan API 1.4.305. The loader package
  version alone was not used as hardware evidence.
- The probe selects a non-CPU Vulkan device, optionally matches `--expect-gpu`,
  and binds DXVK's device-name filter to that selected name before device creation.
  Parent runs used `--expect-gpu Intel`; DXVK's Vulkan log confirms Intel selection.
  Its D3D9 adapter identifier spoofs AMD Radeon RX 6700 XT by default and is not
  authoritative evidence of physical hardware.

The hosted Linux run [41267177207](https://github.com/koalabuttz/KisakCOD/actions/runs/41267177207)
passed all six existing server/core and SDL2/SDL3 Debug/RelWithDebInfo jobs before
this experiment. That workflow does not build or execute this hardware probe.

## Verification and failed attempts

Parent manual GCC build and standalone CMake build both linked as 32-bit without
suppressing unresolved shared-library errors. See the probe README for commands.
The private SDK needs `-rpath-link` for transitive dependencies at link time and
its runtime directory in `LD_LIBRARY_PATH`. No system libraries were replaced.

Negative controls:

- Resize at frame 1 or unchanged resize target: usage exit 2.
- Expected GPU not present: failure exit 1, no D3D9 success.
- Missing ICD: unsupported exit 3, no PASS.
- Software-only ICD without expected-device constraint: unsupported exit 3.
- Explicitly requesting the CPU device: expected-device mismatch exit 1; also
  not a PASS. A combined negative-control assertion initially expected exit 3
  for this case; the output was inspected and the unfiltered software-only case
  was verified separately.

Initial DXVK configure failed on missing private PipeWire development metadata;
unpacked matching development packages. Compilation then failed because the
relocated SDL pkg-config file pointed at system headers; added the explicit SDK
include path. Both failures were resolved before the successful library build.
An initial probe compile used a nonexistent device GetPresentParameters method;
corrected to acquire/release the swap chain and query it there.

A private Xvfb hardware attempt lost its X connection after the first present and
failed. It is not counted as a pass; the owned Xvfb service was stopped. The
subsequent desktop X11 and Wayland runs passed. Scratch libraries, tools, logs,
binaries and DXVK source stay outside the repository.

## Next gate

This experiment does not integrate the engine renderer. Next is a separately
reviewed experiment with a representative packaged shader pair, real material
constant/sampler bindings, vertex declarations and one indexed textured world
surface. Preserve native/Win32 ABI separation. Full shader corpus compatibility,
D3DX/source compilation needs, world passes, reset/resource ownership, minimize/
restore, visual correctness and performance remain open. Do not call a clear/
present test a playable Linux client or a validated world renderer.

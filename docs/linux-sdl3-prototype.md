# Optional SDL3 platform prototype

The native Linux window host can be built against either SDL2 or SDL3. SDL2
remains the default. This is an experimental disconnected input/menu host, not a
playable client or a replacement for the D3D9 world renderer.

## Build selection

Keep separate build directories for the two SDL versions. Both require matching
32-bit development/runtime dependencies and the project's multilib toolchain.
SDL3 requires at least 3.2.10; the initial local SDK was Debian i386
`3.2.10+ds-1`, unpacked outside the repository. Nothing is downloaded by the
backend selector or vendored into the source tree.

```sh
python3 scripts/tests/test_linux_window.py --build-dir build-linux/window-sdl2 \
    --sdl-major 2 --sdl2-dir /usr/lib/i386-linux-gnu/cmake/SDL2 \
    --cc clang-19 --cxx clang++-19

# Set the SDK runtime library search path if it is not installed system-wide.
python3 scripts/tests/test_linux_window.py --build-dir build-linux/window-sdl3 \
    --sdl-major 3 --sdl3-dir /absolute/path/to/i386/cmake/SDL3 \
    --cc clang-19 --cxx clang++-19
```

The underlying CMake selector is `KISAK_LINUX_SDL_MAJOR=2|3`, alongside
`KISAK_BUILD_LINUX_WINDOW_HOST=ON`. Dedicated and client-core builds do not require
SDL3. Each executable links only the selected SDL major. The hosted window job
has an SDL2/SDL3 × Debug/RelWithDebInfo matrix. SDL3 is built from pinned upstream
revision `877399b2b2cf21e67554ed9046410f268ce1d1b2` (release 3.2.10) with `-m32`,
shared-library linkage and an ELF32 check. Its SDK deliberately disables native
display/audio backends for dummy/disk tests; this is not X11, Wayland or physical
audio coverage. Configure and checker logs are retained even on failure. Hosted run
[41267177207](https://github.com/koalabuttz/KisakCOD/actions/runs/41267177207)
passed all four window jobs of this matrix — SDL2 and SDL3, Debug and
RelWithDebInfo — alongside its two native dedicated/client-core jobs.

## Preserved behavior and deliberate differences

The adapter keeps engine-facing integer keys/mouse coordinates, 32-bit modular
millisecond timestamps, and synchronous RGBA pixel-buffer presentation. SDL3
nanosecond timestamps are divided before narrowing. Relative motion and wheel
fractions are accumulated within uninterrupted input, with checked conversion
and reset at input interruptions. SDL3 text is borrowed only during dispatch;
the engine consumer processes it immediately. Existing Latin-1 restrictions,
missing composition/IME integration, and clipboard limitations are not solved by
changing SDL versions.

Keys, motion and pointer boundaries retain ordered delivery. Focus loss clears
held state. Closed windows cannot reacquire relative capture. Text-input startup
failure rolls back construction. The shared tests cover both releases rather
than replacing engine consumers with successful stubs.

SDL3 menu audio uses an audio stream with bounded application mixing storage.
Stream submission refusal permanently fails that provider and is reported by
subsequent owner-thread operations, rather than silently treating lost PCM as a
completed sound. Callback failures do not throw across SDL. Mixer behavior and
the SDL2 device path remain unchanged. Dummy/disk audio tests do not establish
physical audibility or full game-audio integration.

SDL chooses the window surface backing. The old unconditional framebuffer-
acceleration disable prevented native Wayland presentation and has been removed.
The engine still produces CPU pixels; SDL may upload them for presentation. This
is not an implementation of world rendering or a commitment to SDL's GPU API.

## Initial native verification

Focused Debug checks were executed directly as ELF32, not under QEMU:

- SDL2 window suite: 74 checks; SDL3 window suite: 83 checks, including SDL3
  timestamp, fractional-input, borrowed-text and startup-failure cases.
- Production input suite: 64 checks; shared presentation suite: 27 checks.
- SDL3 audio refusal fixture: 28 checks, including first and later queue-write
  failures. A worker's mutation removed the failure reporting and failed seven
  assertions. Parent execution independently passed the fixed failure fixture
  and unavailable-device check.
- Parent SDL3 X11/Xvfb integration passed input, commands, relative capture,
  resize/focus, pixels and cleanup. This is XTest input, not physical hardware.
- Parent native Wayland input-host smoke passed creation, five presentations,
  observed focus/motion delivery and clean close using a four-frame run. The log
  explicitly reported `driver=wayland`, `game=false`, `frame=unavailable`.

Parent final Debug aggregate runs passed **182 SDL2 checker checks** and
**204 SDL3 checker checks**, zero failures. These per-fixture counts overlap
aggregate checks. Final read-only review found no remaining actionable blockers
in its window/runner scope after the capture, text-startup and push-result fixes.

An earlier SDL2 aggregate failed because the refactored summary parser omitted
punctuation; corrected and rerun. Parent audio verification also exposed driver
environment precedence in the fixtures: SDL3's canonical variable overrode the
legacy SDL2 variable used by the test. Fixtures now select the matching variable;
the failure and unavailable-device tests and both final aggregates passed.
Neither failed attempt is counted as successful verification.

Native Wayland interactive capture/reacquisition, non-US/IME text, resize and
fractional-scale behavior, physical audio, game audio, full client frames and
connected gameplay remain unverified. RelWithDebInfo and Windows builds were not
covered by these Debug results. The hosted window matrix now runs Debug and
RelWithDebInfo, but its runners set the dummy video and audio drivers, so the
Wayland and X11 evidence above remains local.

The same SDL3 release also supplies the window system for the separate DXVK
Native D3D9 experiments in `tests/linux_renderer`. Those probes do not use this
host and are not engine rendering; see
[the native D3D9 device experiment](linux-renderer-probe.md).

## Research basis

The implementation follows the official [SDL 3.2.10 migration guide](https://github.com/libsdl-org/SDL/blob/release-3.2.10/docs/README-migration.md),
[stream callback contract](https://wiki.libsdl.org/SDL3/SDL_SetAudioStreamGetCallback),
and [stream locking contract](https://wiki.libsdl.org/SDL3/SDL_LockAudioStream).
SDL3's different return conventions, text ownership, timestamp units and audio
lifetime are behavioral changes, not just renamed APIs.

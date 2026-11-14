# Speed builds on Linux (opt-in, for measuring)

Three build options trade something for speed. None is on by default, and the
default build is unchanged. They exist so a speed-up can be measured on a real
machine before anything becomes a default.

They have been measured once, on a PC, and showed no gain (2026-11-14; see
"What was measured" below). They stay opt-in.

## The options

| Option | What it does | Cost |
| --- | --- | --- |
| `-DKISAK_LINUX_CPU=<level>` | Lets the compiler assume a newer x86 CPU (`-march=<level>`), for example `x86-64-v3` (AVX2, any Intel or AMD CPU from about 2015 on), `haswell` or `native` (the building machine's own CPU). | The games stop with an illegal instruction on an older CPU. |
| `-DKISAK_LINUX_LTO=ON` | Links with ThinLTO: the compiler sees the whole program at link time and can inline across source files. | Needs `lld`. The link takes minutes instead of seconds, and the code is larger. |
| `-DKISAK_LINUX_CLIENT_ASSERTS=OFF` | Builds `KisakCOD-mp` and `KisakCOD-sp` without the engine's assertions (several thousand checks, many in per-frame code). The Windows release builds and the Linux dedicated server are already built this way. | A broken invariant is no longer reported where it happens; the game runs on. |

The first two apply to every target of the build directory. Use a build
directory of its own, so the normal build stays next to it for comparison.

Keep assertions on for bug hunting and for any run whose log is evidence that
the port works. The option is for timing only.

`KISAK_LINUX_CPU` is for x86 builds (i386 and x86_64). A 32-bit build takes a
CPU name such as `haswell` or `native` the same way.

## Floats stay the same

A newer CPU level brings fused multiply-add, one instruction for `a * b + c`
that rounds once where the baseline rounds twice. Clang uses it on its own when
the CPU level allows it. That would change the game's floats: a client would
predict movement slightly differently from the server it plays on.

`KISAK_LINUX_CPU` therefore always adds `-ffp-contract=off`, the same rule the
Arm builds follow. No fast-math flag is used.

`scripts/tests/test_linux_speed_build.py` checks both halves: that Clang does
fuse at `x86-64-v3` without the flag and does not with it, and that every
compile command of a build configured with the options carries the flags. It
runs as the `speed_build` check of `scripts/tests/run_linux_checks.py`.

## Building

The multiplayer client, as in [linux-mp-client.md](linux-mp-client.md), with
the three options added:

```sh
cmake -S . -B build-speed -G Ninja \
    -DKISAK_PLATFORM=linux -DKISAK_LINUX_ARCH=x86_64 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DKISAK_LINUX_CPU=x86-64-v3 -DKISAK_LINUX_LTO=ON -DKISAK_LINUX_CLIENT_ASSERTS=OFF \
    ...the client options of linux-mp-client.md...
cmake --build build-speed --target KisakCOD-mp
```

On Debian and Ubuntu `lld` is the package of that name.

## Comparing two builds

Two builds cannot share one launch, and a laptop's speed drifts between
launches by more than most changes are worth. Frame rate is therefore a poor
number to compare two builds by.

Compare the CPU time a frame takes instead, at a fixed frame rate both builds
reach: the same map, the same view, `com_maxfps 60`, and `perf_sample`
([perf-ab.md](perf-ab.md)) for its `cpu ms/frame` numbers, `main` for the main
thread and `all` for the whole game. Set `r_gpuSyncSleep 100` in both, so a
thread that waits for the GPU does not count as work. Take each build twice,
in the order A B B A.

## What was measured

On the PC (Ryzen 5 2600, RTX 2080 Super; local results, not CI), 2026-11-14,
alone on the machine, in the project files' `perf/pc-speed-build-run-*.md`.
The question was whether the idea (a newer CPU level and whole-program
optimisation) saves CPU time, so these are not the Linux builds:

- **Direct3D 9 client, Win32 Release** (run 3): MSVC `/arch:AVX2` with whole-program
  optimisation against the plain build, mp_crash with 12 bots, 1024x768,
  windowed, 90 s a row, order P S S P. The PC was CPU-limited (the GPU about
  25 to 30% busy). Plain 461.3 frames a second, the speed build 457.8: -0.8%,
  inside the speed build's own spread of 4.9%. No measurable gain. The
  speed build's executable is 5.4% larger.
- **Dedicated server in WSL, no renderer** (run 2): `-march=x86-64-v3`
  against the plain build, 12 bots, rows P S S P P S. A server tick took about
  1.26 ms plain and 1.33 ms with the option (1.23 ms without the first
  speed row, which was 20% slower throughout and is not explained). Out of a 50 ms budget this scene
  does not load the CPU enough to separate the builds.
- A first attempt in WSL under software rendering (run 1) ran at 2.8 frames
  a second and says nothing about CPU time.

The options stay opt-in and nothing changes by default. The server run's
report suggests a heavier server scene (more bots or a faster `sv_fps`) as
the next try.

## What is verified

In a cloud container, without game data and without a GPU:

- With no option given, the compile commands and link flags of the x86_64
  client and server are identical to those before the options existed.
- `KisakCOD-mp` and `KisakCOD-dedi` (x86_64) build with `x86-64-v3` and
  ThinLTO, and `KisakCOD-mp` also without assertions; `KisakCOD-dedi` (i386)
  builds with `haswell` and ThinLTO. Clang 18, lld 18.
- None of those binaries contains a fused multiply-add instruction
  (`objdump -d`, counting `vfmadd`, `vfmsub`, `vfnmadd`, `vfnmsub`). The
  x86_64 client contains about 31,000 other AVX-encoded float instructions, so
  the CPU level did take effect.
- The asset-free checks pass on them, on a CPU with AVX2: the client window
  test and `test_linux_mp_client.py` on the client (with all three options),
  `run_linux_checks.py --arch x86_64` on the x86_64 server and the full
  `run_linux_checks.py` on the i386 server.
- Code size of the x86_64 client (`size`, text): about 5.2 MB as shipped, 6.2 MB
  with `x86-64-v3` and ThinLTO, 5.6 MB with assertions off as well. Bigger
  is not faster or slower by itself; it says inlining happened, and that
  assertions are about a tenth of the client's code.
- Build time on 4 cores, client and server together, from nothing: about 4
  minutes with ThinLTO.

Not verified:

- Speed of the exact Linux build. The PC measured the idea with other builds
  (next section); no Linux build with any of the options has run a map.
- Play between a speed build and a baseline build. The flags say the floats
  match; no game has checked it.

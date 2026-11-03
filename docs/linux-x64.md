# 64-bit (x86_64) track

i386 is the shipping Linux build and stays that way. This page covers the opt-in,
parallel track toward a 64-bit build, decided 2026-11-03. Nothing on it changes
the default build, `build-linux.sh`, the Windows builds or the i386 stdcall and
32-bit layout checks.

The track is staged:

1. **Compile census** (this page, done): measure what stops a 64-bit compile and
   where the 32-bit assumptions are, filed under the upstream issues.
2. **x86_64 dedicated server**: make `KisakCOD-dedi` build and run 64-bit, which
   is where the fastfile loader and script VM work lands.
3. **x86_64 client**: the renderer and client on DXVK Native x86_64.

## Upstream alignment

Upstream [SwagSoftware/KisakCOD](https://github.com/SwagSoftware/KisakCOD) tracks
this in the **"64-bit Support"** milestone. It has no x64 branch or 64-bit build
target; 64-bit fixes land on master in small commits (for example 8aadf94
"x64 gremlins", which replaced literal-size `Com_Memcpy`/`memset` calls with
struct copies and `sizeof`). This fork has every upstream commit through 8aadf94.
To keep one 64-bit port rather than two, fixes here should be small, sit in the
shared sources the way upstream's do, and not depend on Linux-only machinery.

| Upstream item | What it asks | How the census reports it |
|---|---|---|
| [#2 64-bit Support](https://github.com/SwagSoftware/KisakCOD/issues/2) (tracker) | Blockers: Miles; fast-file databases "need either real-time patches, or conversions"; "pointer arithmetic from the de-compilation"; Clang support; "struct sizing numbers from de-compilation to sizeof(struct)" | Fastfile `Load_Stream` count; pointer/integer cast warnings; literal-size copies; alignment errors. Clang support: the census is a Clang pass |
| [#28 Compiler/Windows types to STL types](https://github.com/SwagSoftware/KisakCOD/issues/28) | `int32_t`/`uint32_t` over `LONG`/`long`, no `HRESULT`/`LRESULT`, no `*_s` functions | "Compiler and Windows types" errors and 64-to-32 narrowing warnings |
| [#74 static_assert for multi-platform](https://github.com/SwagSoftware/KisakCOD/issues/74) | Title only, no body yet | "Struct layout static_asserts": every `static_assert(sizeof == 32-bit value)` that fails at 64 bits |
| [#75 Bink Video Replacement](https://github.com/SwagSoftware/KisakCOD/issues/75) | Open-source decoder (ScummVM's or FFmpeg), "needed ... for cross-platform and x64" | Bink call-site count |
| [#76 Miles Audio Replacement](https://github.com/SwagSoftware/KisakCOD/issues/76) | OpenAL replacement merged as PR #88 behind `KISAK_OPENAL`, with known bugs | Miles `AIL_` call-site count |
| Closed PR [#85](https://github.com/SwagSoftware/KisakCOD/pull/85) (Android arm64) | Declined: "doesn't really integrate well into the codebase"; no x64 support yet | Its loader kept the 32-bit on-disk layout and resolved pointers at use; one candidate for step 2 |

Three upstream points need agreeing before step 2 changes shared headers:

- **#74 has no stated design.** The i386 layout checks must stay (project rule),
  so the likely shape is per-architecture expected sizes, or the asserts limited
  to the serialized/network structs that must keep 32-bit layout. Upstream's
  choice should come first.
- **`long` differs between targets.** Linux x86_64 is LP64 (`long` is 8 bytes);
  Windows x64 is LLP64 (`long` is 4 bytes). #28's move to `int32_t`/`uint32_t`
  is what makes both work; `src/linux/compat.h` asserts `sizeof(long) == 4`
  today, and the census files that under the fork's own shims.
- Upstream #35 (remove calling-convention specifiers) conflicts with this fork's
  i386 stdcall checks; on x86_64 the specifiers are ignored, so it does not block
  this track.

## The census

```sh
python3 scripts/tests/run_x64_compile_census.py --jobs 4 --report-dir artifacts/x64-census
```

`-DKISAK_LINUX_ARCH=x86_64` (default `i386`) configures the Linux build with
`-m64` instead of `-m32`. It exists for the census: it accepts a 64-bit compiler
only when asked, refuses every other opt-in Linux target, and warns that
`KisakCOD-dedi` does not compile 64-bit yet. The runner configures such a build
directory, takes the exact `KisakCOD-dedi` compile commands, adds the multiplayer
client sources from the i386 client census with `-m64`, and runs one
`-fsyntax-only -ferror-limit=0` Clang pass per unit with these hazard warnings on:
`-Wpointer-to-int-cast`, `-Wint-to-pointer-cast` and their `void`/`enum` forms,
and `-Wshorten-64-to-32`. The 41 client sources that do not compile on i386 either
are skipped.

Diagnostics are deduplicated by location, so a header error counts once however
many units include it, and filed by category. A scan of live code under `src/`
(comments and `#if 0` regions removed, `src/radiant` excluded) adds what the
compiler cannot see from Linux.

The CI job `x86_64 compile census` runs it on every Linux workflow run and puts
the report in the job summary and the `KisakCOD-x64-census` artifact. It fails
only when a source in `scripts/tests/x64_compile_census_baseline.txt` stops
compiling for x86_64; newly compiling sources are reported as progress and
recorded with `--update-baseline`.

### First results

Local run, 2026-11-03, Clang 18.1.3, cloud sandbox (no GPU, no game data), on
master efff5b3 plus this change. Hosted CI publishes the same report on each run.

| Set | Units | Compile for x86_64 |
|---|---:|---:|
| `KisakCOD-dedi` | 215 | 34 (zlib, ODE, three `src/linux` units) |
| MP client | 370 | 16 (ODE, the two Miles units compiled out by `KISAK_OPENAL`, `cl_auth.cpp`) |

Errors (stop the compile):

| Category | Upstream | Sites | Files |
|---|---|---:|---:|
| Struct layout static_asserts | #74, #2 | 149 | 33 |
| Linux Win32 ABI shims (`src/linux/compat.h`, `d3d9_types.h`) | fork only | 7 | 2 |
| Alignment requests below the x86_64 minimum | #2 | 6 | 4 |
| Compiler and Windows types (`va_list`, `_BitScanReverse`, `_vsnprintf`) | #28 | 5 | 4 |
| Inline assembly | none | 0 | 0 |
| Other | triage | 3 | 2 |

Warnings (compile, but break at run time):

| Category | Upstream | Sites | Files |
|---|---|---:|---:|
| Pointer/integer casts | #2 | 613 | 92 |
| 64-to-32-bit narrowing | #2, #28 | 534 | 133 |

Source scan:

| Item | Upstream | Count | Files |
|---|---|---:|---:|
| Fastfile `Load_Stream` calls | #2 | 282 | 4 (279 in `db_load.cpp`) |
| Miles `AIL_` call sites | #76 | 125 | 3 |
| Bink call sites | #75 | 26 | 1 (`r_cinematic.cpp`) |
| `memcpy`/`memset` with a literal size | #2 (cf. 8aadf94) | 123 | 73 |
| `static_assert(sizeof/offsetof)` lines | #74 | 321 | 56 |
| Live inline `__asm` statements | none | 6 | 3 |

What this says:

- **The compile wall is almost entirely the layout asserts.** Remove the 149
  failing asserts and the 7 shim asserts and only 14 other error sites remain
  across both sets. That makes #74 the first decision for step 2.
- **The real work is at run time**, where the compiler cannot help: 613
  pointer/integer cast sites (the script VM, `com_memory`, `com_files`, `dvar`,
  the xanim and fastfile loaders lead), 123 literal-size copies of the kind
  8aadf94 fixed, and the fastfile loader copying 4-byte retail pointers into
  8-byte fields.
- **Inline assembly is smaller than a raw grep suggests.** 85 `__asm` matches
  exist under `src/`, but most are comments or Xbox 360 PowerPC code inside
  `#if 0`. Six live statements remain: x87 `fld`/`fistp` in `qcommon.h` (behind
  `_WIN32`), an `emms` macro and an `_M_X686` block in `common.cpp`, and an `ebp`
  read for stack walks in `assertive.cpp` (MSVC branch). None is reached by the
  Linux x86_64 compile; all of them matter for an MSVC x64 build, which has no
  inline assembler.

### What the census does not claim

It is a Clang front-end pass on Linux x86_64: not a link, not a run, not MSVC
x64, and not proof that a unit that compiles behaves correctly with 8-byte
pointers. The fastfile loader, for one, compiles today and is still wrong at
64 bits.

## Windows MSVC x64 census

The Windows workflow has a matching report-only job, "MSVC x64 compile census"
(upstream issue 1 asks for x64 CI rows). It configures the normal CMake project
with `-A x64`, builds `KisakCOD-sp`, `KisakCOD-mp` and `KisakCOD-dedi` with one
MSBuild log each, and `scripts/ci/summarize_msvc_x64.py` sorts the diagnostics
into the categories above, deduplicated by site. Compile errors never fail the
job; a broken configure or a missing log does. It runs on pushes to `master`
and manual runs only, not on pull request pushes. The summary is on the run
page and in the `KisakCOD-msvc-x64-census` artifact.

The first run (2026-11-03, before the per-platform layout asserts landed)
stopped all three targets in 37 to 38 files:

| Category | Sites | Notes |
|---|---:|---|
| Struct layout static_asserts | 153 | the same asserts Clang reports; the per-platform asserts change takes these out |
| Inline assembly | 1 | `assertive.cpp` stack walk; `qcommon.h` and `common.cpp` are not reached yet |
| Other errors | 49 | mostly MMX intrinsics MSVC x64 lacks (`_m_empty`, `_m_punpcklbw`) in the SSE skinning path, plus `CPUSTRING` |
| Pointer/integer casts (warning) | 1,271 | twice the Clang count; MSVC warns on more cast forms |
| 64-to-32 narrowing (warning) | 292 | `size_t` into `int`/`uint32_t` |

MSVC stops a file after 100 errors, so later errors in a file show up only
once the first ones are fixed.

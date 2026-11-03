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

Points where this fork had to choose before upstream did:

- **#74 (static_assert for multi-platform) has no stated design.** This fork
  uses `LAYOUT_32BIT(cond)` from `src/universal/layout_assert.h`:
  `static_assert(LAYOUT_32BIT(sizeof(XModel) == 220));` checks the retail value
  on 32-bit targets exactly as before and holds on 64-bit ones. It is applied
  only to the 151 checks that fail at 64 bits because of pointer size (found by
  compiling every MP, SP and dedicated unit both ways: none of them fails on
  i386). Every check that holds on both stays a plain `static_assert`, so 64-bit
  builds keep checking layouts that must not change, such as network structs.
  The Linux Windows-ABI shims (`src/linux/compat.h`) assert the Windows x86 or
  x64 value for the target instead (`WPARAM`/`LPARAM` pointer-sized, `OVERLAPPED`
  20 or 32 bytes). Plain standard C++, no compiler extension, so it suits
  upstream's MSVC build too.
- **`long` differs between targets.** Linux x86_64 is LP64 (`long` is 8 bytes);
  Windows x64 is LLP64 (`long` is 4 bytes). #28's move to `int32_t`/`uint32_t`
  is what makes both work. The Linux `Interlocked*` shims assert a 4-byte
  operand, which is where engine `long` counters fail at 64 bits today.
- Upstream #35 (remove calling-convention specifiers) conflicts with this fork's
  i386 stdcall checks; on x86_64 the specifiers are ignored, so it does not block
  this track.

## The census

```sh
python3 scripts/tests/run_x64_compile_census.py --jobs 4 --report-dir artifacts/x64-census
```

`-DKISAK_LINUX_ARCH=x86_64` (default `i386`) configures the Linux build with
`-m64` instead of `-m32`. It accepts a 64-bit compiler only when asked, refuses
every other opt-in Linux target, and builds `KisakCOD-dedi` (see
[the x86_64 dedicated server](#x86_64-dedicated-server) below). The runner configures such a build
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

### After the per-platform layout asserts

Local run, 2026-11-03, same setup, after `LAYOUT_32BIT` was applied (upstream
issue 74):

| Set | Units | Compile for x86_64 |
|---|---:|---:|
| `KisakCOD-dedi` | 215 | 49 (was 34) |
| MP client | 370 | 31 (was 16) |

Layout assert and `src/linux` shim errors are gone. The compile wall is now 19
sites: `_BitScanReverse` redefining a compiler builtin in `qcommon.h` (hit by
505 units), the five Linux `Interlocked*` shims rejecting 8-byte `long`
counters, `va_list` and `_vsnprintf` mismatches (all upstream issue 28), six
`align(N)` requests below the x86_64 minimum alignment (upstream issue 2), and
three follow-on errors.

### After the standard types cleanup

Local run, 2026-11-03, after the upstream issue 28 cleanup on top of the
per-platform asserts: all 585 units (215 dedicated, 370 MP client) compile for
x86_64, and i386 is unchanged. What changed:

- **`long` where retail meant 32 bits.** The `volatile long` counters handed to
  `Interlocked*` became `volatile LONG` (4 bytes on both Windows ABIs and in the
  Linux shim), `sint32` became `int32_t`, and the `_BitScanReverse` callers pass
  `DWORD`. `com_files.cpp` read a zip offset through an `unsigned long` straight
  into a `uint32_t` field, an 8-byte write on LP64; it now goes through a local.
- **`long double` became `double`.** MSVC's `long double` is `double`, so retail
  never had 80-bit values; on Linux it was 80-bit with 16-byte alignment, which
  is what the six `align(N)` errors were.
- **`va_list` and `*_s`.** Variadic helpers that kept a decompiled `char *`
  take `va_list`. The remaining `*_s` calls (`sscanf_s`, `strcpy_s`,
  `sprintf_s`, `vsnprintf_s`, `_vsnprintf_s`, `fopen_s`) became the portable
  call or `I_strncpyz`.
- **`_BitScanReverse`.** Clang on x86_64 already provides it as a builtin, so
  the Linux fallback is declared, not defined, there.
- **Kept on purpose.** `HRESULT`, `LRESULT`, `DWORD` and the other Windows types
  in D3D9 and Win32 signatures stay: they are API types with fixed sizes on
  both Windows ABIs and in the Linux shim. Upstream issue 28 targets engine
  data, not API boundaries.

The compile wall is gone; the rest is the run-time work above. Pointer/integer
cast warnings are unchanged at 613 sites, and 64-to-32 narrowing fell from 534
to 490.

### What the census does not claim

It is a Clang front-end pass on Linux x86_64: not a link, not a run, not MSVC
x64, and not proof that a unit that compiles behaves correctly with 8-byte
pointers. The fastfile loader, for one, compiles today and is still wrong at
64 bits.

## x86_64 dedicated server

Step two. `KisakCOD-dedi` builds and links as an ELF64 x86_64 program. Without
game data it boots to the same end state as the i386 build, and with the retail
files it loads a map (see [Loading a retail map](#loading-a-retail-map)).

```sh
cmake -S . -B build-x64 -G Ninja -DKISAK_PLATFORM=linux -DKISAK_LINUX_ARCH=x86_64 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-x64 --target KisakCOD-dedi
python3 scripts/tests/test_linux_bootstrap.py --arch x86_64 bin/KisakCOD-dedi
```

`build-linux.sh` stays i386-only. Both architectures write `bin/KisakCOD-dedi`
unless `CMAKE_RUNTIME_OUTPUT_DIRECTORY` says otherwise, so keep one build
directory per architecture and configuration. The build string is
`linux-x86_64` (`linux-x86_64-debug` in Debug builds).

The CI job `x86_64 dedicated server` builds Debug and RelWithDebInfo and runs
the bootstrap test against each binary with `--arch x86_64`.

### What had to change to boot

Every crash on the asset-free path was a dvar string pointer stored in a
32-bit integer, the decompiler's way of moving a `DvarValue`:

- `value.integer = (int)string` and `(char *)dvar->current.integer` became
  `value.string`, here and in every module that read string dvars that way.
- String ownership in `dvar.cpp` (`Dvar_ShouldFree*String`, the current,
  latched and reset string swaps) compared and copied `.integer`, which moves
  only the low 4 bytes of the pointer; it now uses `.string`.
- Enum dvars read their string table as `*(const char **)(domain.integer.max +
  4 * index)`; they use `domain.enumeration.strings[index]`, which is the same
  field on i386.
- Domain functions were called through a cast that split the `DvarValue` into
  four 32-bit arguments; they are called with the `DvarValue` itself.

On i386 each of these produces the same code as before.

### Fastfile records

Fastfiles store every record exactly as the 32-bit retail executable held it in
memory: 4-byte pointers, 4-byte alignment, block offsets that count retail
bytes. The i386 build streams records straight into engine structs. A 64-bit
build expands them while loading (upstream issue 2 calls this the "conversions"
route; the closed Android PR kept 32-bit records and resolved pointers at use,
which cannot hold DXVK objects later):

- `scripts/tools/gen_db_disk_layout.py` asks Clang for the i386 and x86_64
  layout of every type the loader streams (the pointee of each `var*` global in
  `db_load.cpp`) and writes `src/database/db_disk_layout_gen.h`: per type, the
  retail size and a list of copy and pointer-widen operations. Disk offsets are
  numbers; native offsets are `offsetof` expressions, so each compiler works
  them out for its own ABI. 166 of 341 types are identical on both targets and
  are copied as they are; 175 are converted.
- On i386 the table is only checked, at compile time: every native offset must
  equal its disk offset and every type must keep its retail size. A struct
  change that would break fastfile loading stops the i386 build.
- `Load_Stream(atStreamStart, varXModel, 220)` keeps the retail byte count and
  now takes the typed pointer. On 64-bit targets it reads the retail bytes,
  widens pointers (keeping the `-1`/`-2` "data follows" markers), and advances
  the native stream by native bytes. The xanim loaders' record heads (4 retail
  bytes of `XAnimPartTrans`, then its union) work the same way.
- Every stream also keeps its retail position, and records which native bytes
  each retail range became. `DB_ConvertOffsetToPointer` and
  `DB_ConvertOffsetToAlias` map a retail offset to the native record, field or
  one-past-the-end address through those records.
- Unions that mix pointers and data follow one fixed member per union
  (`UNION_POLICY` in the generator). A widened pointer keeps the retail value in
  its low 4 bytes, so an int or float member at offset 0 reads the same.
  `cLeafBrushNodeData_t` is the exception: it is copied as data, and the loader
  widens the leaf pointer once the node says it is a leaf.
- Zone blocks are reserved at 2.5 times their retail size plus 64 KB (no record
  more than doubles), the Linux physical memory arena is 512 MB instead of
  128 MB on x86_64, and a block that would still overflow stops the load with an
  error.

Checks, all asset-free:

```sh
/usr/bin/python3 scripts/tools/gen_db_disk_layout.py --build-dir build-x64 --check
python3 scripts/tests/test_db_record_conversion.py
```

The generator needs the libclang Python bindings (`python3-clang-18`, run with
the system Python) and any configured Linux build directory. The conversion
test compiles `db_stream.cpp` and `db_stream_load.cpp` unmodified for x86_64
with ASan and UBSan and feeds them retail bytes from memory. CI runs both in the
`x86_64 dedicated server` job.

### Loading a retail map

The x86_64 dedicated server loads `mp_crash` from the retail game files, runs
the gametype scripts and its first server frames, and shuts down cleanly, with
the same 18 checks as i386 (`scripts/tests/test_linux_asset_startup.py --map
mp_crash`). A `developer 1` / `developer_script 1` run prints no asserts or
script errors on either architecture, and the two logs match apart from timing.
This runs on a machine with the game installed, not in CI.

What the map load needed beyond the record conversion, each a 32-bit size or
width baked into decompiled code:

- Hunk user headers and the `qsort` element sizes used `uint32_t` and literal
  sizes; they use pointer-sized fields and `sizeof`.
- The asset clone table folded size functions of types that share a retail size
  (`LoadedSound` cloned at the `GameWorldSp` size); every slot names its type.
- About 60 allocations, clears and copies used retail struct sizes (`XAnim_s`
  entries, DObj model lists, `client_t`, file handles); they use `sizeof`.
- Script compiler and VM: operand and saved-stack widths (codepos, jumps, case
  tables, `SCR_STACK_ENTRY_SIZE`), parse tree values copied through their 4-byte
  `type` member or passed as a node's `Enum_t`, and script values copied as
  `u.intValue`. `test_script_parser_stack.py` now fails on any parse tree slot
  that holds a node pointer cut to 32 bits.
- Index math and offset tables: a default asset was linked into the hash with a
  byte difference `>> 4`, and the script entity and player field tables held
  i386 offsets into `gentity_s` and `gclient_s`; both use typed indices and
  `offsetof`.

### Hosting bots

With bots connected, two more 32-bit assumptions showed up:

- `G_GetClientSize()` returned the retail 12676. The server strides archived
  client and player states by that value, so on x86_64 (where `gclient_s` is
  12680 bytes) every client after the first was read a few bytes early. It now
  returns `sizeof(gclient_s)`.
- `&scrVmPub.top[-paramcount]` with a `uint32_t` count: the negated count is
  2^32 - n, which wraps back on i386 but on x86_64 points about 2^32 values
  past the stack. Engine-started script threads with arguments (the first
  damage callback) crashed. These read `top - paramcount` now, and the CI check
  below rejects the pattern:

```sh
/usr/bin/python3 scripts/tools/check_unsigned_negation.py --build-dir build-x64
```

It parses every source in the build with libclang and reports a unary minus on
an `unsigned int` that is used as an index or added to a pointer.

Still i386-shaped and off the dedicated path: the SP save game stack format in
`scr_readwrite.cpp`, client-side literal offset and size tables (effects,
devgui, static model cache, server browser), and the pointer/integer casts the
census lists in the raw asset (`*_load_obj.cpp`) loaders.

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

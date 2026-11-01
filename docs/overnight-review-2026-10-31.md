# Overnight Linux review handoff — 2026-10-31

This review branch combines the independently preserved client and server series.
It does not update master. The combined code tree matches all 1,426 files in the
fresh tested reconstruction exactly; only this handoff and its supporting text
files were added afterward. No runtime rerun is claimed for documentation-only
publication. The merge retains both original histories.

| Provenance | Commit |
|---|---|
| Common base / master observed before publication | `f8bde51a0e44ff464d97a03f24c9ce83429799c1` |
| Client: `dot/client-string-owner-2026-10-31` | `fef1422199729e9b58dbc11c14a523d533989da8` |
| Server: `dot/bot-refill-2026-10-31` | `b5e3d408d683768d34842574c71bdf8db295eb04` |

The client series was applied first, then the server series. Their cumulative
patches were verified and applied to a fresh base checkout before testing, with
new Debug/RelWithDebInfo build directories and no reused objects or binaries.
The combined authored-source fingerprint was
`e7aa658eeb078ffa9fa0e7ec1740bfe573d8f53997a57c2be9a9a117820157cf`.

## What to review

Server changes address bot disconnect/refill and map-transition ownership,
weapon-string lifetimes, database publication and shutdown reliability.
The optional Linux frontend runs actual command/input and admitted menu logic,
owned images/fonts/audio services and bounded CPU rendering. Recent changes add
production menu scheduling, generation-safe frame cancellation and actual
interned-string ownership. Real allocation exhaustion reproduced and then fixed
string-lock retention and partially published dvar ownership. Shared string
wrappers were extracted unchanged; MP/SP compilation and independent review passed.

Start with the individual commits and `docs/linux-interned-strings.md`,
`docs/linux-render-frame-owner.md`, `docs/linux-offline-menu-frame.md`, and
`docs/linux-ui-registration.md`. Defaults remain dedicated/client-option-OFF;
the optional frontend is not a playable native game client.

## Validation and limits

[Machine-readable validation](overnight-review-2026-10-31/validation.json) records
source/binary hashes, exact supported checks and omissions. Both configurations
passed core 518/window 182, pool/UI 9, dvar 11+11, consumers 51, genuine owner 64/cleanup 8/
images 30, supported dedicated 15, and X11 5,208 comparisons. Counts overlap.
Live tests used three bots, three full-cohort refills, two map cycles, five-second
holds and clean quit. Neither reproduced the earlier !g_loadingAssets observation.
A newly compiled offline exporter produced 21 files identical to the earlier
licensed export. Private game inputs were not committed.

ELF32 executables ran under the existing QEMU adapter, not direct native32 here.
Default server checks `handshake_auth`, `udp`, and `server_test_clients` were
explicitly skipped because they require unavailable direct ELF32/32-bit sanitizer
paths. Existing host64 fatal ASan+UBSan/mutation and ELF32/QEMU test-client variants
passed separately. Full Windows/SP runtime and physical audio were not run; the
full SP UI retains known baseline compile blockers. The old isolated X11 startup
flake remains unresolved/nonreproduced despite passing later runs.

Full CL_Frame/device/world rendering, general game audio and native authentication
remain incomplete. Scene blur is reported unrendered; CPU sampling is approximate.
No two-human-player gameplay or completed retail/Steam/CoD4x compatibility is claimed.
Denied CL_PacketEvent extraction/validation was not resumed or implemented indirectly.

## Reproduce on a native Linux laptop

Use an ELF32-capable host, Clang 19/multilib C++20 toolchain, CMake/Ninja/Python,
i386 SDL2 and matching Tracy v0.12.2 source. Optional X11 tests also need Xvfb,
xdotool and Pillow. No new credentials or game downloads are required.

```sh
export REPO="$PWD"
export DELIVERY="$REPO/docs/overnight-review-2026-10-31"
export TRACY_SOURCE=/absolute/path/to/existing/tracy
export SDL2_CMAKE_DIR=/absolute/path/to/i386/SDL2/cmake/SDL2
export BUILD_ROOT=/absolute/path/to/new/review-build
export CC=clang-19 CXX=clang++-19 JOBS=3
bash "$DELIVERY/build-and-check.sh"
```

Set SDK include/link/runtime paths before running the helper if SDL is unpacked
outside system directories. [BUILD.md](overnight-review-2026-10-31/BUILD.md)
contains those settings and exact dedicated/existing-suite/live-bot commands.
[ASSETS.md](overnight-review-2026-10-31/ASSETS.md) describes export/preparation from
an existing authorized installation and the genuine-menu invocation. Keep private
outputs outside the repository. The local QEMU adapter is not distributed; these
commands are supported as written on a native ELF32-capable host.

Review the code first, then run the documented native checks, the genuine X11
menu, and physical audio where available. Record unsupported checks explicitly.
This branch adds no new packet/auth implementation and does not resolve the denied
packet-work boundary. No PR or merge into master is requested by this handoff.

The separately saved source-only ZIP has SHA256
`de4c882e5db1bc57ec327b76930aec187b0d03179a43530cc24f120672290081`.
That ZIP and rendered screenshot remain local because Library delivery returned
HTTP 401 before preparation; no Library retry was made. This Git branch is a
separately authorized source publication, with no raw assets, screenshots,
executables, build output or credentials added.

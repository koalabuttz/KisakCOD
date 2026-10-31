# Retail CoD4 client compatibility evidence

This records the bounded comparison of the examined Windows 1.0 and 1.7
multiplayer clients with KisakCOD's networking source. It is not a claim of
complete version equivalence or a successful 1.0 gameplay test. Server setup and
opt-in authentication are described in [Linux dedicated server](linux-dedicated.md).

## Targets and method

| Client | Size (bytes) | SHA-256 |
| --- | ---: | --- |
| Examined 1.0 `iw3mp.exe` | 3,325,952 | `c7a5f9b296f23efa9055868c58afde986b0bd0d45533de667b11fe01d800b3e4` |
| Installed 1.7 `iw3mp.exe` | 3,330,048 | `f50f8a520581754e73cb7cae67b02adcbad5f2e71aaa412a013bb3ef57a391a5` |

The 1.0 file was extracted from `Setup/Data/iw3mp.exe` in the supplied ISO.
Its banner identifies it as `CoD4 MP 1.0`; the ISO's authenticity was not
independently established. The separate modified single-player executable was
not used. Addresses below are virtual addresses specific to these PE32 builds
(image base `0x400000`), not portable signatures or file offsets.

The investigation read tables and disassembly without installing or natively
launching either executable. A bounded Unicorn emulation ran the 1.0 client's
Huffman initialization, tree builder and statically linked sort routine to
recover symbol codes; that executes selected machine instructions in an
emulator, not the game or installer. No executable bytes are included in this
repository.

## Table and codec observations

| Item | Examined 1.0 | Examined 1.7 | Result |
| --- | --- | --- | --- |
| `msg_hData` | `0x6bac10` | `0x6bbc10` | All 256 weights match the source exactly. |
| `constantConfigStrings` | `0x71f990` | `0x7209b0` | All 833 configstring-number/name pairs match the source. |
| Netfield arrays | First array `0x6b6aa0` | First array `0x6b7aa0` | All 18 arrays match source names, order, bit encodings and change hints. |
| Netfield structure offsets | 1,047 entries | 1,047 entries | All offsets match between the two executables. Source offsets were not independently recomputed. |
| Huffman symbol codes | Emulated tree | Previously recovered test expectations | All 256 codes match; total code length 2,182 bits, maximum length 11. |

The arrays cover entity variants, archived entities, clients, players, HUD
entries and objectives. Matching table entries is stronger evidence than merely
finding the same strings, but does not independently establish every caller or
runtime path.

In particular, the 1.0 Huffman result agrees with the expectations already in
[`test_huffman_tree.py`](../scripts/tests/test_huffman_tree.py). The tie-order
fix required by Linux is therefore not specific to the 1.7 client. The pinned
regression compiles the production codec, checks all symbol codes and checks
encoded bytes and round trips without requiring either executable.

## Protocol-number observations

| Location | Examined 1.0 | Examined 1.7 |
| --- | --- | --- |
| Version banner | `1.0` at `0x6ce5f0` | `1.7` at `0x6cf580` |
| Outgoing connect protocol | `push 1` at `0x46af25` | `push 6` at `0x46b075` |
| Server `protocol` dvar default | `1` at `0x52f6c5` | `6` at `0x52ff55` |
| Browser protocol comparison default | `1` at `0x470dc2` | `6` at `0x470d8e` |

A mismatched `getinfo` protocol fails the examined browser's version check.
That is separate from connection admission: advertising 6 does not make a
protocol-1 direct connection invalid, and advertising 1 does not prevent a
protocol-6 direct connection when `sv_allowProtocol6` permits it.

## Selected instruction comparisons

Exact disassembly differences were retained for selected gamestate-parser,
entity-decoder and channel-transmit ranges, not proven complete functions.
Changed call/global addresses were not resolved to equivalent objects. These
comparisons therefore do **not** establish whole-function equivalence. The
selected 42-byte channel XOR loop is byte-identical between the two images;
that does not verify the surrounding command or authentication paths.

An early comparator erased hexadecimal operands and could hide constants. Its
zero-difference claims were discarded. The retained comparator preserves exact
bytes and operands, validates complete disassembly of its selected ranges and
prints all differences. The table and Huffman observations above do not depend
on that comparator.

## Conclusions and limits

The examined table layouts and Huffman codes support using the same message
encoding for these 1.0 and 1.7 clients. Together with the earlier brief, real
1.7 gameplay test, this supports keeping 1.7 admission available as an explicit
opt-in rather than changing KisakCOD's protocol-1 default.

A playable 1.0 connection remains **unverified**. The comparison did not cover:

- a live 1.0 handshake, authentication or gameplay session;
- the full Steam-ticket/CD-key authentication paths;
- every reliable-command, download, voice, snapshot or game/script behavior;
- all server-side writers against a separately shipped 1.0 server binary;
- a complete source-to-binary struct-layout or semantic-equivalence proof;
- mixed-version multiplayer, extended sessions or real browser listing.

Do not interpret matching selected code or data as proof that the versions
differ only in their protocol numbers.

## CoD4x 21.1 loopback test

The restored installed client identified itself on the wire as `xver=21.1` and
selected legacy `protocol=6`. Its `cod4x_021.dll` SHA-256 was
`bf5965ea484e0b899119ebb99428b8f7aaf175f2052e610c1ce466119e6b0d2b`.
The test used `mp_crash`, `sv_loopbackTestAuth 1`, `sv_allowProtocol6 1`, and
`sv_advertisedProtocol 6` on a loopback-only native Linux server, with a local
UDP relay recording packet flow. No client files were changed.

Observed before the random-range fix:

- Challenge, protocol-6 admission, stats transfer and gamestate transfer
  completed; the server then sent player snapshots.
- The player reported a disconnect after a team switch, and another after a
  grenade death. The server logged an out-of-map entity coordinate and failed
  again during shutdown.
- A targeted server debugger stopped before the second coordinate error. It
  identified entity 323 as an item (`ET_ITEM`), with an X trajectory velocity of
  approximately 2,157,791 units/second. Its X position moved from approximately
  1,167 to 109,057. The player's own position remained in the map.

The shared random helper divided libc `rand()` by 32,768, assuming the
15-bit Windows range. Linux libc returns up to 2,147,483,647; `crandom()` therefore
produced values far outside its intended range, and `Drop_Item` multiplied them
by the configured drop-speed amplitudes. The multiplayer `G_rand()` family had
the same assumption, including the helper used for death-related grenade drops.
An isolated production-function probe reproduced million-unit item velocities.
Both producers now constrain their input to 15 bits, preserving the Windows
mapping and restoring their consumers' expected bounds. The snapshot writer's
coordinate guard was not relaxed.

After rebuilding with that fix, the player repeated **a grenade death and a
team switch without a crash**. The server retained the same process with no
restart or coordinate error; the recorded session exchanged snapshots for more
than 90 seconds without a server disconnect response. This supports a brief,
single-player CoD4x 21.1 legacy-protocol connection, including those two
previously failing actions. It does not establish multi-player, map-rotation,
long-session, public-authentication or every CoD4x-version compatibility.

The earlier claim that CoD4x necessarily used a different legacy entity decoder
and still desynchronized was not supported by this fresh test and has been
withdrawn. The available public client source identifies itself as 21.8, not the
installed 21.1; its legacy parser calls the original retail decoder, but that
source is not a byte-for-byte specification of this installed DLL.

## Local reproduction artifacts

The investigation scripts and structured outputs are retained locally under
`build-linux/retail10-comparison/` (gitignored). They contain no executables and
are not part of the normal regression suite. Its `README.md` records local
prerequisites, paths, selected instruction ranges and remaining code-comparison
limitations. The table scan and Huffman emulation can be rerun against the
hashed inputs; the temporary extracted executable and emulator environment may
need to be recreated on another host.

The normal, executable-free codec regression is:

```sh
python3 scripts/tests/test_huffman_tree.py
```

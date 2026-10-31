# Local UI assets for the Linux diagnostic frontend

Use an existing authorized CoD4 installation. Game files, exported fonts, and
atlas images remain local runtime inputs; none are included in this repository.
This path provides genuine font metrics and CPU-decoded images. It does not start
full gameplay or the game renderer.

## Offline font export

`KISAK_BUILD_UI_ASSET_EXPORT` defaults to `OFF`. Enabling it adds the separate
`KisakCOD-ui-asset-export` target. The normal dedicated executable has no export
hook. The tool uses the real database loader and exits after export, before
`Netchan_Init`, `SV_Init`, or `NET_Init`. Tracy is disabled for this target.

Use the existing 32-bit Linux build toolchain and local dependency sources:

```sh
cmake -S . -B build-export -DKISAK_PLATFORM=linux \
  -DKISAK_BUILD_UI_ASSET_EXPORT=ON \
  -DFETCHCONTENT_SOURCE_DIR_TRACY=/path/to/existing/tracy \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/build-export/bin"
cmake --build build-export --target KisakCOD-ui-asset-export

KISAK_UI_ASSET_EXPORT_DIR="$PWD/private-ui-export" \
  build-export/bin/KisakCOD-ui-asset-export \
  +set fs_basepath /path/to/authorized/game \
  +set fs_homepath "$PWD/export-home" \
  +set dedicated 2 +set useFastFile 1
```

The export directory must be absolute and empty. A new directory is created
with mode `0700`; an existing directory must already have that mode. Use an
isolated `fs_homepath`. Where native ELF32 execution is unavailable, use the
existing authorized QEMU runner for the executable.

A successful run exits with status zero and prints `asset_export=complete`.
Only then use `manifest.json` and its listed `font_N.font` files. Failed runs can
leave partial files or an incomplete manifest; preserve them for diagnosis and
retry into a new empty directory. File streams close before process exit. This
one-shot tool leaves database arenas and workers process-owned; it does not
claim ordinary game shutdown.

The font files use the existing `R_LoadFont` format: four little-endian 32-bit
values (font-name offset, pixel height, glyph count, material-name offset), then
the original 24-byte `Glyph` records and NUL-terminated names. Offsets are from
the beginning of the file. The first 96 glyphs represent codepoints 32–127;
the separate sorted extra table can contain control characters below 32 as well
as codepoints above 127. The exporter preserves these records unchanged.

The manifest records every base/glow material texture's semantic, name hash,
and image name. Resolve the atlas through that association; do not assume the
first texture or invent a font-to-image mapping. In the tested English assets,
`fonts/consoleFont` contains 254 glyphs at height 16 and refers to
`gamefonts_pc`, present as `images/gamefonts_pc.iwi` inside
`main/localized_english_iw00.iwd`. Extract only the needed authorized entry into
a private directory outside the repository and provide its explicit path to
the frontend.

## Supported image subset

`LoadIwiFile`/`DecodeIwi` validate the complete file before calling the shared
engine `Image_DecodeDxtc` decoder. They accept IWI version 6, 2D DXT1/DXT3/DXT5,
power-of-two base dimensions from 4 through 4096, and a complete stored mip
chain (or the explicit no-mipmap flag). File size is limited to 32 MiB, and all
four picmip-size fields must match the computed layout. Output is owned RGBA.

Bitmap/wavelet images, cubemaps, volume textures, unsupported dimensions,
truncated data, and trailing data reject explicitly. DXT3/5 color offsets and
DXT5 packed alpha selectors are corrected in the shared decoder. Existing
RGB565 expansion and interpolation behavior is retained; pixel-identical D3D
rendering is not claimed. The frontend may reject additional unsupported font
or rendering features rather than substituting guessed metrics.

## Actual UI text path and ownership

The optional `KisakCOD-client-input` host accepts explicit asset paths:

```sh
KisakCOD-client-input --font-file /private/export/font_3.font \
  --font-atlas /private/images/gamefonts_pc.iwi \
  --ui-image /private/images/menu_button.iwi --text 'Native Linux UI'
```

Use the manifest to select the correct atlas for the selected font. `--font-file`
and `--font-atlas` must appear together. Missing or unsupported specified assets
fail before the window-ready marker; there is no synthetic font fallback. The
existing image fixture remains the default only when `--ui-image` is omitted.

Text executes the actual `UI_DrawText`/screen placement → `CL_DrawTextPhysical` →
`R_AddCmdDrawText` path. The CPU entry uses the shared engine text-command
emitter and real `R_NormalizedTextScale` and `R_GetCharacterGlyph` functions.
Normal game entry points retain their existing implementations and validation.
The CPU renderer accepts only its own scoped font/material handles and command
bindings. It copies names, glyphs, and text; font ownership pins the atlas material
and texture. Release, resize, shutdown, and restart invalidate pending work without
leaving borrowed font or command pointers. Stale handles remain invalid on restart.

Supported text is printable ASCII with CR/LF, style zero, no rotation or cursor,
using the genuine glyph offsets, advances, UVs, and engine half-texel baseline.
CR/LF do not consume `maxChars`. Zero-area whitespace still advances. The parser
preserves valid sorted control/extended glyph metadata, but drawing unsupported
characters rejects explicitly. Color escapes, icons, tabs, shadows/glow, localized
text decoding, and arbitrary shader techniques are not implemented. Font files are
bounded to 128 KiB, 96–4096 glyphs, and pixel heights 1–512; text is bounded to 1024
bytes. Whole-frame text raster work is bounded to 64 million pixels, with complete
preflight before pixel mutation. Glyphs use bilinear atlas sampling and the existing
bounded CPU source-over blend into an opaque target.

This is a real UI primitive and resource path, not an initialized menu system,
`CL_Frame`, full renderer, or playable client. Menu assets, fonts, and game shaders
are not automatically registered. No packet, authentication, or receive integration
is added by this work.

## Validation checkpoint (2026-10-31)

Debug and RelWithDebInfo each passed the window aggregate (121/121), client-core
aggregate (497/497), actual text fixture (19/19), renderer allocation/lifetime
fixture (33/33), and synthetic IWI decoder checks (21/21). All nine genuinely
exported fonts parsed successfully (19/19 including parser checks). Normal and
console fonts each completed two in-process upload/register/draw/pending-shutdown
cycles using their genuine atlas (16/16 including parser checks), in both builds.

Independent X11 readback verified 65 genuine glyph, background, and menu-button
samples at each of three viewport sizes in both builds. Its oracle uses exported
metrics and an independent image decoder; a no-font negative control fails.
Independent source review closed without remaining findings. Evidence is retained
outside the repository at `/workspace/kisakcod-client-fonts-results/`.

The initial export's extra-glyph assumption was corrected after real console-font
metadata showed codepoint 1; a fresh complete export then produced all nine fonts.
No font metrics were guessed. Private game assets and exports are excluded from
source patches. Prior combined server/client evidence remains a separate checkpoint;
these results do not claim a new combined gameplay test.

The separately denied packet task remains stopped. Its exact automated agent
cybersecurity-risk rejection (not an exec permission approval) is recorded in
`/workspace/kisakcod-client-input-startup-results/packet-work-blocker.json`.

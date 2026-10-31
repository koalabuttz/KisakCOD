# Genuine static menu text slice

This checkpoint runs a selected real menu item's auto-wrapped text painter in the
existing Linux input/window host. It does not initialize `Menu_Paint`, execute menu
scripts, evaluate menu visibility, or implement interactive settings changes.

The offline database inventory found 360 menus and 18,718 items. The exercised
item is `options_graphics_defaults:69`, using its genuine localized confirmation
text from the private export. Its original centered
alignment, rectangle, scale `.375`, font enum zero, foreground color, and style
three are preserved. No lookalike menu item or guessed glyph metrics substitute
for the exported item.

## Production path

`Item_Text_AutoWrapped_Paint` calls the original wrapped-text layout, font metrics,
line-wrap selection, `UI_DrawText`, screen placement, and shared text command
producer. Those routines were moved to shared translation units; game entry
points retain their normal behavior. `UI_GetFontHandle` and the CPU adapter use
the same extracted selection body, including strict threshold comparisons and
explicit role mappings. Missing roles reject instead of falling back.

The CPU scope owns the input string, copies each temporary wrapped line into its
command buffer, and restores placement and command/font-binding counters if any
later line fails. The caller's text rectangle changes only after successful
recording. Full-frame preflight still precedes pixel mutation. The original
painter does not clip to the item rectangle: long unbroken words can overflow it;
output is clipped to the surface. This behavior is preserved.

Style three now follows `RB_DrawText`: each glyph first draws black using the
original packed alpha at a one-physical-pixel offset, then the foreground glyph.
The offset does not scale with the font. Both passes count toward the existing
whole-frame raster budget. Other shadow/glow styles, cursor, icons, and color
escapes remain unsupported.

Actual screen-placement calculations now have an explicit display-extent and
pixel-aspect entry. The original game wrapper supplies `cls.vidConfig`; the host
supplies its real SDL extent and square pixels, without claiming a game renderer
or altering shared video configuration.

## Genuine resources and material limits

The private bundle contains one bounded item record and seven genuine font roles.
It preserves the original font/material association and resolves the image by the
validated material description. Shader program identities, shader arguments,
state bits, texture semantic/name hash, and sampler state must match the supported
unlit UI profile. Unknown programs or states reject; no shader-name-only fallback
exists. Private shader bytes, game images, and exported font/menu data are not
included in patches.

The actual sampler uses anisotropic/minification and mip selection. This backend
supports its base-level magnification subset only: metadata-backed images and
font glyphs reject minification during preflight. Synthetic fixture materials
retain their explicitly separate base-image behavior. The initial menu viewport
is 640×480; 900×675 is also supported for the selected item. At 720×540, real font
selection chooses a larger font whose glyphs would be minified, so rendering
fails explicitly instead of changing the font or disabling the guard.

The offline exporter does not initialize the UI runtime. Font role registration
names come from actual UI initialization code and are matched against exported
assets. Thresholds `.25/.4/.55` are the source-backed initialization defaults,
not claimed to be live UI dvar values. Menu-level scripts/expressions and visibility
are inventoried but not executed by this selected-item preview.

## Running with authorized local assets

```sh
python3 scripts/tools/prepare_linux_menu.py \
  --export /private/completed-offline-export \
  --output /private/new-menu-bundle \
  --menu options_graphics_defaults --item 69

KisakCOD-client-input --menu-bundle /private/new-menu-bundle \
  --ui-asset-dir /private/extracted-images --run-ms 5000
```

Preparation accepts only static supported text items without item scripts,
expressions, dvars, ownerdraw, background, or focus sounds. It reads bounded input
files and creates a new private directory. Native material validation remains
mandatory at load. Explicit missing/unsupported resources fail before the ready
marker. Bundle loading rolls back newly allocated resources on failure; successful
resources belong to the renderer and are released on normal shutdown. Handles
cannot be reused after restart. The current loader copies each role's atlas;
shared atlas caching is a future optimization.

## Evidence and remaining boundaries

Focused runtime checks cover real wrapping/alignment/borders/newlines, temporary
line lifetime, empty and unbreakable words, exact role selection, late recording
rollback, style-three alpha/offsets, and atomic minification rejection. Parser
checks cover malformed bounds, invalid fields, unsupported controls, and owned
string lifetime. Genuine seven-role load/draw/pending-shutdown/reload and missing
atlas failure run separately from synthetic fixtures.

Evidence is retained at `/workspace/kisakcod-client-menu-results/`. No packet,
receive, authentication, or equivalent denied work was attempted. The exact prior
automated agent cybersecurity-risk rejection remains in
`/workspace/kisakcod-client-input-startup-results/packet-work-blocker.json`.
Full gameplay, initialized menus, arbitrary shaders, text effects, and mipmapped
minification remain outside the implemented slice.

Final Debug and RelWithDebInfo checks each passed: window aggregate 124/124,
client-core aggregate 500/500, focused menu painter 25/25, menu parser 11/11,
and strict material parser 11/11. The final genuine-bundle fixture passed 15/15
in both modes, including two render/reload cycles and a deliberately corrupted
last role that rolls back six completed roles while preserving an existing
texture. Ten host failure cases (five per build) reject missing bundles/atlases,
unsupported materials, missing option pairs, and mixed fixture/menu options
without a window-ready marker. Six preparation checks include exact genuine
item byte parity and rejection of unsupported text, scripts, dvars, localization,
and styles. Independent source review closed without remaining findings.

Independent X11 readback passed in both builds: 94 samples at 640×480 using the
actual selected text-font role and 93 samples at 900×675 using the actual selected
big-font role. The independent oracle uses balanced wrapping, genuine glyph
metrics, separately decoded atlas alpha, original foreground tint, and style-three
shadow composition. A no-shadow negative control fails the glyph-pixel comparison.
Resizing to 720×540 exits with the explicit minification error, as required by the
supported material profile. These are real window/readback results, not full menu
interaction or gameplay evidence.

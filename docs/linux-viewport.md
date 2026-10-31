# Native viewport commands

The bounded CPU backend now consumes the actual `RC_SET_VIEWPORT` command.
`CpuRenderer::SetViewport` scopes the existing `CL_SetViewport` client thunk,
which calls the shared typed command producer. The original thunk came from
the single-player client; moving it into the shared draw translation unit also
makes it available to the standalone native UI. The game renderer entry retains
its command allocation and assertions. The command remains exactly 20 bytes.

The existing device-free `R_SetViewportStruct` and `R_SetViewportValues` bodies
are shared by game and CPU backends. The CPU consumer owns a command-source
state for each list and uses its actual `sceneViewport` for UI/text origin and
clipping. Every list starts at the full owned target, matching the backend's
scene-viewport initialization. Commands affect following draws; no viewport
state carries into another list or across resize/restart.

For this admitted 2D path, coordinates remain local pixel coordinates and gain
the integer viewport origin at rasterization. Coverage and bilinear UV sampling
retain the existing pixel-center and half-open edge conventions. Clipping does
not resize or resample the original rectangle. The implementation does not add
unused projection matrices or claim D3D vertex-shader floating-point parity.
Projection/device startup remains a separate task.

Whole-list preflight rejects negative origins, nonpositive dimensions, extents
outside the owned target, malformed sizes and truncated commands before any
pixel replay. Integer bounds use widened arithmetic. Existing material, font,
resource-generation and raster-budget checks remain in force.

Two ordering restrictions are explicit. A clear following any nondefault
viewport in the same list is rejected, even if a later command restores the full
viewport: the legacy backend's dirty viewport and device-clear ordering cannot
be represented by assuming the newest source rectangle is already applied.
Raw `StretchRect` copies bypass the UI viewport in the device backend, so the
CPU subset rejects raw copying while the current viewport is nondefault. Raw
copying after an explicit full-viewport restore retains absolute coordinates.

The existing input host exposes `--ui-viewport X Y W H` for image/font mode;
menu modes do not accept that option. It uses the same window, renderer and
real image/text callers. Genuine menu controls record the full viewport each
frame. No renderer-ready/game-running flag, packet handler, authentication
provider or online action is added.

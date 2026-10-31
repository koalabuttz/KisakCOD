# Native local menu control slice

The optional Linux window host can load a private, explicitly prepared
`options_look` control bundle with `--menu-controls DIR --ui-asset-dir DIR`.
It owns the genuine exported labels and yes/no controls at source indices
105, 106, 111 and 112. No proprietary assets or localized strings belong in
source patches. Preparation uses the existing authorized game assets and the
offline export tool; it does not download replacement content.

The host routes real client key/mouse consumers into the extracted production
`Menu_HandleKey`, `Menu_HandleMouseMove`, focus and menu-stack routines.
`Item_YesNo_HandleKey` changes the real `cl_freelook` and `m_filter` dvars.
Actual item text/yes-no painters use the real font selector, placement, glyph
metrics and shared rendering commands. This is a bounded local control view,
not full `Menu_Paint`, `UI_Init`, `CL_Frame` or gameplay.

Escape executes the admitted close-self script. Once closed, the existing
console command `menu_open` reopens the same menu through production menu
lifecycle code. Window close and termination signals exit the host. Focus loss
clears actual engine key/action/mouse state and menu capture/focus pointers.
Resizing recomputes placement and invalidates actual cached text extents.

The owned script registry uses real parsing and handlers for the exact exported
local-variable, close and two sound commands. It validates a whole registered
script before execution and rejects all other script profiles. Only exact
literal-label expressions are admitted by the host; the game's normal
expression evaluator remains unchanged. Errors from external services are
reported; previously executed local operations are not represented as rolled
back. Menu blur, arbitrary bindings/owner draw/listboxes/edit controls and
other menus are unavailable rather than emulated.

Sound uses the actual exported single-variant PCM profile and SDL's real
callback device. The standalone mixer uses unity master/channel gain and the
original alias volume. It is not the full game sound system. Dummy-driver tests
exercise callback lifetime and samples but do not establish audibility.

The renderer now owns complete IWI mip chains and supports the admitted
isotropic nearest-mip/bilinear clamp profile. Incomplete base-only textures
still reject minification. This does not establish anisotropic GPU parity.
Text style 6 uses the actual two-physical-pixel black shadow convention.

`MenuInputOwner` requires an exclusively stopped renderer. It initializes one
resource generation and shuts it down on failed construction or destruction,
including pending frames. Existing live renderer resources are rejected before
mutation. This permits repeated owners with the same input and renderer while
preventing dangling font/material pointers. UI readiness is scoped to this
bounded provider; prior readiness and catcher state are restored when detached.

No packet, receive, authentication or admission path is modified by this work.
The previous automated agent cybersecurity-risk rejection remains in force.

The optional real-asset integration test is
`scripts/tests/test_linux_menu_x11.py`. Supply `--binary`, `--bundle`, `--assets`,
`--menu-inventory` and `--output`; `--emulator`, `--xvfb` and `--xdotool` select
existing local tools when needed. It requires Pillow for independent atlas
decoding and starts a private Xvfb display with TCP disabled. It checks actual
keyboard/mouse settings changes, close/reopen, focus loss, resize and shutdown,
plus label pixels at five sizes. SDL audio uses the explicitly selected dummy
driver. The test does not obtain assets or establish audible playback or gameplay.

## Validation boundaries

The genuine-asset lifecycle executable is `KisakCOD-menu-input-owner-test
CONTROL_BUNDLE IMAGE_DIRECTORY`. It covers failed asset initialization,
preservation of an unrelated live renderer, and three create/destroy cycles
on the same input and renderer, including pending-frame teardown. Its direct
menu-key calls complement the separate X11 tests, which travel through actual
SDL events and client key/mouse consumers.

The asset-free window suite separately covers admitted script parsing,
unsupported callbacks, audio metadata and real dummy-device teardown,
exception-safe borrowed-owner detachment, stored-mip selection and style-6
pixels. The default-OFF check confirms these targets remain optional.

Actual extracted UI translation units compile for multiplayer and applicable
single-player source variants. Existing single-player `ui_shared.cpp` and
`ui_expressions.cpp` Linux compilation still has baseline `SaveType`/`_iobuf`
header failures; this work does not claim a complete single-player client
build. Radiant does not select the game UI source set.

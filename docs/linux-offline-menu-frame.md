# Offline production menu frame scheduling

The owned Linux menu host executes actual `Menu_PaintAll`. That body and `Menu_Count` moved unchanged into `ui_frame.cpp`, shared by normal game builds and the CPU UI host. Game visible-list helpers remain unchanged; the host rejects that unsupported diagnostic mode before entry and supplies explicit throwing adapters as a secondary boundary.

This replaces the owner's manual blur reset and single-menu paint. The scheduler resets blur once, scans the open stack backward for the topmost fullscreen menu, paints registered nonstacked menus in registration order, then paints the selected stack suffix. Fullscreen selection intentionally ignores visibility: a hidden fullscreen menu still suppresses earlier menus. The engine cursor remains last.

The offline bridge admits at most eight registered menus, twelve items each, and twelve named material bindings. It validates the entire registry, stack membership/uniqueness, graphs, supported features and resource ownership before scheduling. Capture callbacks, debug overlays, visible-list diagnostics and fullscreen menu background images are unsupported. Fullscreen flags without background remain meaningful; they are not cleared to bypass engine policy.

Late paint failure restores commands, all affected menu/item state and the UI context. Owned bindings and placement are restored. SDL presentation failure retains the existing transaction policy: CPU pixels may already have changed, pending commands are discarded and the error propagates; no pixel rollback is claimed.

`DrawMenuFrame` reports `painted` when drawing commands were emitted. Single-menu `DrawMenuTraversal` retains `Menu_Paint`'s boolean return. A visible empty menu can emit no commands while requesting blur, so callers inspect `requestedBlur` and `postprocessUnsupported` independently. The post-presentation `menu_frame` trace identifies the scheduler and registered/open counts. Blur remains explicitly unrendered rather than applied incorrectly to the final UI surface.

Synthetic owned-resource tests execute real ordering, no duplicate painting, hidden fullscreen suppression, empty and blur-only frames, unsupported-mode rejection, and late cross-menu rollback. Genuine assets and X11 interaction/readback are separate integration checks.

This is not full `CL_Frame`, `SCR_UpdateFrame`, `UI_Refresh`, or renderer initialization. `UI_Refresh` includes online browser work; renderer frame roots require world/material/debug/skin-cache/device ownership absent here. Those paths remain untouched. No authentication or packet-processing work is introduced.

# Offline UI preparation prerequisite

The native UI host consumes a bounded subset of metadata exported from a locally owned game installation. Preparation now derives the selected `options_look` controls directly from the offline export's `menu-inventory.json`; an externally assembled `selected-control-profile.json` is neither required nor read.

The static text and control preparers share a bounded 128 MiB inventory reader and validate the exporter schema and byte encoding. The control preparer still admits only the four selected label/control records, their exact literal expressions, empty material expressions, supported dvars, and role-specific click/focus/back scripts. It serializes scripts without executing them. The runtime binary format and its independent admission checks are unchanged. Unexpected hooks, fields, expressions, and output replacement fail explicitly.

The preparation sequence is:

1. Run the existing optional offline asset exporter against the authorized local installation. Keep its output private and outside the source tree.
2. Run `scripts/tools/prepare_linux_menu_input.py --export EXPORT --output NEW_BUNDLE`. This copies genuine font-role metadata and creates control metadata.
3. Run `scripts/tools/prepare_linux_menu_composition.py --export EXPORT --bundle NEW_BUNDLE` to create the admitted background/cursor composition and material profiles.
4. Run `scripts/tools/prepare_linux_ui_audio.py --inventory SOUND_EXPORT/ui-sounds.json --output-dir NEW_BUNDLE` to validate and copy the exact supported PCM resources.
5. Supply the private bundle and separately prepared genuine images to the existing `--menu-controls` and `--ui-asset-dir` host options.

The dependency is actual exported metadata → strict preparation → owned runtime fonts/materials/sounds → production menu traversal and input. Fixing this chain makes fresh local preparation reproducible; it does not initialize the full game frame, world renderer, authentication, or network services. The real menu blur request remains an explicitly unrendered scene-compositor requirement. Applying blur to the final UI surface would not implement that engine boundary.

`test_linux_menu_input_prepare.py` exercises current exporter-shaped metadata without a profile, deterministic output, no overwrite, larger inventories, schema bounds, and rejected script/expression changes. It runs in the Linux window checker alongside composition and audio preparation regressions. Actual installation validation must remain separate from these asset-free tests; never commit exported metadata, localized text, images, fonts, or PCM data.

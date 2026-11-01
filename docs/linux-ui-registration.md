# Shared UI context and owned-menu registration

The Linux menu owner now uses shared production registration code instead of manually publishing `menuCount`, `Menus[0]`, and local-variable initialization. This is a bounded UI startup prerequisite, not full `UI_Init` or `CL_Frame` initialization.

Normal game entry points retain their responsibilities:

- `Menu_Setup` calls the shared count-reset/setup path with the real parser callback. The callback still initializes item keywords then menu keywords before actual `UILocalVar_Init` runs. Setup does not implicitly free existing local variables.
- `UI_AddMenu` retains its original capacity checks, assertions, and `IsFastFileLoad` / `DB_FindXAssetHeader` identity check before the shared bounded append. Duplicate references remain permitted on this legacy path.

The exported-resource owner uses a deliberately different provenance contract: `UI_ReplaceOwnedMenus` registers borrowed references to already owned metadata, never claims DB registration, and does not initialize the text parser. It validates the whole replacement before mutation, rejects active menu stacks, null entries, unsupported counts, duplicate pointers, and case-insensitive duplicate names. Names are bounded to 255 bytes. A successful replacement releases actual local-variable allocations, clears borrowed registry and stack references, and shares the same initialization and append implementation. A snapshot allows replacing from the context's own registry array.

Menu objects and their name storage must remain owned by the caller through registration and reload. They must not borrow storage from that context's local variables. Shutdown releases local variables and clears borrowed pointers; it does not invoke engine asset-memory deletion on C++-owned export records. The owner closes/detaches actual input before teardown, and its exception path also clears the context.

The asset-free registration fixture exercises real local-variable allocation/release, complete-capacity registration, invalid replacement atomicity, alias reload, legacy duplicate behavior, keyword callback order/error propagation, and repeated teardown/recreation. The genuine-asset owner fixture additionally rejects reload while a real menu is open, then executes actual close, registry reload, open, input, and presentation across repeated owner lifetimes. No saved configuration is written and settings retain their existing in-memory runtime lifetime.

The full renderer, scene blur, game-frame orchestration, and online services remain separate prerequisites. No packet, authentication, or network code is changed by this registration step.

The associated local-variable string setter allocates a replacement before releasing the old value. This preserves string and scalar values if copying throws and supports assigning the existing string to itself. Tracked-allocation regressions reproduce the old failure without reading or freeing released memory, then verify safe subsequent shutdown and balanced ownership.

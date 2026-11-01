#pragma once
struct UiContext;
struct menuDef_t;

// Shared setup order: reset counts, optional parser keyword setup, localvar init.
// Caller supplies a fresh context or has already shut down its local variables.
void UI_SetupMenuContext(UiContext *dc, void (*setupKeywords)());
// Shared bounded reference append; legacy caller retains its own DB validation.
// Deliberately permits duplicates, matching the normal game's registration.
bool UI_AppendMenuReference(UiContext *dc, menuDef_t *menu);

enum class UiOwnedMenuRegistration { ok, invalidContext, invalidMenu, duplicate, capacity, activeMenus };
// Export-owned references, NOT DB assets. Validate the whole replacement first.
// Requires no open menus; on success releases old localvars and borrowed refs.
// Menus and bounded (1..255 byte) names must remain caller-owned and live
// through registration/reload; they must not borrow this context's localvar
// storage. Does not free caller menu/item memory. Names compare as the real
// case-insensitive Menus_FindByName does.
UiOwnedMenuRegistration UI_ReplaceOwnedMenus(UiContext *dc, menuDef_t *const *menus, int count);
void UI_ShutdownOwnedMenuContext(UiContext *dc);

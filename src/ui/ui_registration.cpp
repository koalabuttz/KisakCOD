#include "ui_registration.h"
#include "ui_shared.h"
#include <cstring>

void UI_SetupMenuContext(UiContext *dc, void (*setupKeywords)())
{
    dc->menuCount = 0;
    dc->openMenuCount = 0;
    if (setupKeywords)
        setupKeywords();
    UILocalVar_Init(&dc->localVars);
}

bool UI_AppendMenuReference(UiContext *dc, menuDef_t *menu)
{
    if (!dc || !menu || dc->menuCount < 0 || dc->menuCount >= 640)
        return false;
    dc->Menus[dc->menuCount++] = menu;
    return true;
}

void UI_ShutdownOwnedMenuContext(UiContext *dc)
{
    if (!dc)
        return;
    UILocalVar_Shutdown(&dc->localVars);
    std::memset(dc->Menus, 0, sizeof(dc->Menus));
    std::memset(dc->menuStack, 0, sizeof(dc->menuStack));
    dc->menuCount = 0;
    dc->openMenuCount = 0;
}

UiOwnedMenuRegistration UI_ReplaceOwnedMenus(UiContext *dc, menuDef_t *const *menus, int count)
{
    if (!dc || dc->menuCount < 0 || dc->menuCount > 640 || dc->openMenuCount < 0 || dc->openMenuCount > 16)
        return UiOwnedMenuRegistration::invalidContext;
    if (count < 0 || count > 640)
        return UiOwnedMenuRegistration::capacity;
    if (dc->openMenuCount)
        return UiOwnedMenuRegistration::activeMenus;
    if (count && !menus)
        return UiOwnedMenuRegistration::invalidMenu;
    // Snapshot references permits callers to reload from dc->Menus itself.
    menuDef_t *replacement[640];
    for (int n = 0; n < count; ++n)
    {
        auto *menu = menus[n];
        if (!menu || !menu->window.name || !*menu->window.name || strnlen(menu->window.name, 256) == 256)
            return UiOwnedMenuRegistration::invalidMenu;
        for (int prior = 0; prior < n; ++prior)
            if (replacement[prior] == menu || !I_stricmp(replacement[prior]->window.name, menu->window.name))
                return UiOwnedMenuRegistration::duplicate;
        replacement[n] = menu;
    }
    UI_ShutdownOwnedMenuContext(dc);
    UI_SetupMenuContext(dc, nullptr); // No text parser or DB registration for owned exports.
    for (int n = 0; n < count; ++n)
        UI_AppendMenuReference(dc, replacement[n]); // Entire list validated before mutation.
    return UiOwnedMenuRegistration::ok;
}

#include <universal/q_shared.h>
#include <win32/win_local.h>

void Sys_DestroySplashWindow()
{
    // No window is created by the terminal-only Linux dedicated server.
    iassert(g_splashWnd == nullptr);
}

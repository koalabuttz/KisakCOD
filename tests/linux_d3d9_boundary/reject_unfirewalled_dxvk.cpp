// Must NOT compile: the engine's Win32 base types and DXVK Native's
// windows_base.h disagree (HWND, HINSTANCE, WCHAR, POINT). This control shows
// what src/linux/d3d9_native.h exists to prevent. If a future DXVK release makes
// this compile, the runner fails so the firewall's rename list is re-examined
// rather than silently outliving its reason.
#include <linux/compat.h>
#include <d3d9.h>
int main() { return 0; }

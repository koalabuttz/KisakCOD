// A stand-in for libdxvk_d3d9.so on a machine without a Vulkan driver.
//
// CI links KisakCOD-device-probe against this library (as
// KISAK_DXVK_NATIVE_LIBRARY) to prove the probe's link closure and to run its
// fail-closed path without a GPU. Its only export is the one the engine calls,
// and it answers the way DXVK Native's Direct3DCreate9 does when no Vulkan
// device can be created: with no interface. It is never installed, never
// shipped, and never linked into anything but that CI check.
//
// Built with: c++ -m32 -shared -fPIC -Wl,-soname,libd3d9_unavailable.so
struct IDirect3D9;

extern "C" __attribute__((visibility("default"))) IDirect3D9 *Direct3DCreate9(unsigned int)
{
    return nullptr;
}

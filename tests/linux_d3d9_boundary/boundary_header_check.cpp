// boundary_header_check.cpp -- the D3D9 entry point <linux/d3d9_types.h> in each
// of its two modes, with nothing else from the engine.
//
// Built by scripts/tests/run_native_d3d9_boundary_checks.py with g++ and clang++,
// -Wall -Wextra -Werror, in both modes:
//
//   * stand-in mode (default): the interfaces are incomplete, so no D3D9 method
//     can be called, and the embedded layouts hold;
//   * native mode (-DKISAK_LINUX_NATIVE_D3D9 plus the DXVK Native include
//     directory): the interfaces are DXVK's real declarations. A local class
//     implements IDirect3DQuery9 and is called through the DXVK-declared vtable,
//     the D3D9 structures the engine fills keep their Windows x86-32 sizes, and
//     the probe functions below give the runner something to disassemble: the
//     engine's __stdcall/WINAPI/CALLBACK must still pop their arguments
//     (`ret $0x8`), while DXVK's COM methods stay caller-cleaned like the
//     functions in libdxvk_d3d9.so.
//
// Asset-free and device-free: no DXVK library is linked, no device is created,
// Direct3DCreate9 is never called.

#include <linux/d3d9_types.h>

#include <cstdio>
#include <type_traits>

namespace
{
int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

template <class T, class = void>
struct IsComplete : std::false_type
{
};
template <class T>
struct IsComplete<T, std::void_t<decltype(sizeof(T))>> : std::true_type
{
};
} // namespace

// --- Calling-convention probes ------------------------------------------------
// Named, external and not inlined so the runner can find each one in the binary.
// They are defined in both modes; only native mode puts DXVK's headers between
// the engine's macros and these definitions, which is what the runner checks.
extern "C" __attribute__((noinline, used)) int __stdcall kisak_d3d9_probe_stdcall(int a, int b)
{
    return a + b;
}
extern "C" __attribute__((noinline, used)) int WINAPI kisak_d3d9_probe_winapi(int a, int b)
{
    return a - b;
}
extern "C" __attribute__((noinline, used)) int CALLBACK kisak_d3d9_probe_callback(int a, int b)
{
    return a * b;
}
extern "C" __attribute__((noinline, used)) int kisak_d3d9_probe_cdecl(int a, int b)
{
    return a ^ b;
}

#if defined(KISAK_LINUX_NATIVE_D3D9)

// The D3D9 structures the engine fills or reads by value keep the Windows
// x86-32 layout the original code was written against.
static_assert(sizeof(D3DPRESENT_PARAMETERS) == 0x38, "D3DPRESENT_PARAMETERS x86-32 size");
static_assert(offsetof(D3DPRESENT_PARAMETERS, hDeviceWindow) == 0x1C, "D3DPRESENT_PARAMETERS.hDeviceWindow");
static_assert(offsetof(D3DPRESENT_PARAMETERS, PresentationInterval) == 0x34, "D3DPRESENT_PARAMETERS.PresentationInterval");
static_assert(sizeof(D3DCAPS9) == 0x130, "D3DCAPS9 x86-32 size");
static_assert(sizeof(D3DADAPTER_IDENTIFIER9) == 0x44C, "D3DADAPTER_IDENTIFIER9 x86-32 size (pack 4)");
static_assert(sizeof(D3DSURFACE_DESC) == 0x20, "D3DSURFACE_DESC x86-32 size");
static_assert(sizeof(D3DLOCKED_RECT) == 0x8, "D3DLOCKED_RECT x86-32 size");
static_assert(sizeof(D3DVIEWPORT9) == 0x18, "D3DVIEWPORT9 x86-32 size");
static_assert(sizeof(D3DVERTEXELEMENT9) == 0x8, "D3DVERTEXELEMENT9 x86-32 size");
static_assert(sizeof(D3DRECT) == 0x10, "D3DRECT x86-32 size");
static_assert(sizeof(D3DMATRIX) == 0x40, "D3DMATRIX x86-32 size");

// A minimal IDirect3DQuery9, implemented here against DXVK's declaration. If the
// interface were declared with a different convention or method order than
// DXVK's, this would either fail to override or dispatch to the wrong slot.
class ProbeQuery final : public IDirect3DQuery9
{
  public:
    HRESULT QueryInterface(REFIID riid, void **object) override
    {
        if (!object)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DQuery9))
        {
            *object = this;
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++refs; }
    ULONG Release() override { return --refs; }
    HRESULT GetDevice(IDirect3DDevice9 **device) override
    {
        *device = nullptr;
        return D3DERR_INVALIDCALL;
    }
    D3DQUERYTYPE GetType() override { return D3DQUERYTYPE_EVENT; }
    DWORD GetDataSize() override { return sizeof(BOOL); }
    HRESULT Issue(DWORD flags) override
    {
        lastIssue = flags;
        return D3D_OK;
    }
    HRESULT GetData(void *data, DWORD size, DWORD flags) override
    {
        if (!data || size != sizeof(BOOL) || flags != D3DGETDATA_FLUSH)
            return D3DERR_INVALIDCALL;
        *static_cast<BOOL *>(data) = lastIssue == D3DISSUE_END ? TRUE : FALSE;
        return S_OK;
    }

    ULONG refs = 1;
    DWORD lastIssue = 0;
};

// Engine-shaped code: the way gfx_d3d calls a query (rb_backend/r_init fence
// pool), through the interface pointer only.
__attribute__((noinline)) static bool kisak_d3d9_probe_fence(IDirect3DQuery9 *query)
{
    if (FAILED(query->Issue(D3DISSUE_END)))
        return false;
    BOOL done = FALSE;
    return query->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) == S_OK && done == TRUE;
}

#endif // KISAK_LINUX_NATIVE_D3D9

int main()
{
    typedef int(__attribute__((stdcall)) * StdcallFn)(int, int);
    typedef int (*CdeclFn)(int, int);
    volatile StdcallFn stdcallFn = &kisak_d3d9_probe_stdcall;
    volatile StdcallFn winapiFn = &kisak_d3d9_probe_winapi;
    volatile StdcallFn callbackFn = &kisak_d3d9_probe_callback;
    volatile CdeclFn cdeclFn = &kisak_d3d9_probe_cdecl;
    check(stdcallFn(40, 2) == 42, "engine __stdcall probe returns through its stdcall type");
    check(winapiFn(44, 2) == 42, "engine WINAPI probe returns through its stdcall type");
    check(callbackFn(21, 2) == 42, "engine CALLBACK probe returns through its stdcall type");
    check(cdeclFn(40, 2) == (40 ^ 2), "plain cdecl probe");

    check(sizeof(D3DDISPLAYMODE) == 16, "D3DDISPLAYMODE is 16 bytes");
    check(D3DFMT_DXT1 == static_cast<D3DFORMAT>(KISAK_D3D9_FOURCC('D', 'X', 'T', '1')), "D3DFMT_DXT1 FourCC");

#if defined(KISAK_LINUX_NATIVE_D3D9)
    std::printf("mode native (DXVK Native D3D9 declarations)\n");
    static_assert(IsComplete<IDirect3DDevice9>::value, "native mode: IDirect3DDevice9 is DXVK's complete interface");
    static_assert(IsComplete<IDirect3DQuery9>::value, "native mode: IDirect3DQuery9 is complete");
    static_assert(std::is_base_of_v<IUnknown, IDirect3DDevice9>, "native mode: COM inheritance from DXVK");

    ProbeQuery query;
    IDirect3DQuery9 *asInterface = &query;
    check(asInterface->GetType() == D3DQUERYTYPE_EVENT, "virtual GetType dispatches through DXVK's vtable");
    check(asInterface->GetDataSize() == sizeof(BOOL), "virtual GetDataSize dispatches through DXVK's vtable");
    check(kisak_d3d9_probe_fence(asInterface), "engine-shaped fence Issue/GetData round trip");
    check(query.lastIssue == D3DISSUE_END, "Issue argument reached the implementation");

    IUnknown *unknown = nullptr;
    check(asInterface->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void **>(&unknown)) == S_OK &&
              unknown == static_cast<IUnknown *>(&query),
          "QueryInterface with DXVK's __uuidof");
    check(asInterface->Release() == 1, "AddRef/Release reference count");
    IDirect3DDevice9 *device = reinterpret_cast<IDirect3DDevice9 *>(1);
    check(asInterface->GetDevice(&device) == D3DERR_INVALIDCALL && device == nullptr, "GetDevice out-parameter");

    // The engine's window handle converts to DXVK's without a cast.
    D3DPRESENT_PARAMETERS parameters = {};
    HWND engineWindow = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(0x1000));
    parameters.hDeviceWindow = engineWindow;
    check(parameters.hDeviceWindow == static_cast<void *>(engineWindow), "engine HWND stored in D3DPRESENT_PARAMETERS");
#else
    std::printf("mode stand-in (type surface only)\n");
    static_assert(!IsComplete<IDirect3DDevice9>::value, "stand-in mode: IDirect3DDevice9 must stay incomplete");
    static_assert(!IsComplete<IDirect3DQuery9>::value, "stand-in mode: IDirect3DQuery9 must stay incomplete");
    static_assert(!IsComplete<D3DPRESENT_PARAMETERS>::value, "stand-in mode: D3DPRESENT_PARAMETERS stays incomplete");
    check(true, "stand-in interfaces are incomplete (compile-time)");
#endif

    if (g_failures)
    {
        std::printf("FAIL %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS boundary header checks\n");
    return 0;
}

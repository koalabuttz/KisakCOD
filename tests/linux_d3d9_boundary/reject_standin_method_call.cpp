// Must NOT compile: without KISAK_LINUX_NATIVE_D3D9 the D3D9 interfaces are
// incomplete, so no translation unit can call a D3D9 method by accident (and no
// call can quietly succeed in the dedicated server).
#include <linux/d3d9_types.h>
int kisak_reject_standin_call(IDirect3DQuery9 *query)
{
    return static_cast<int>(query->GetDataSize());
}
int main() { return 0; }

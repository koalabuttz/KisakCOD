// Must NOT compile: d3d9_native.h is reached only through <linux/d3d9_types.h>.
// Built by run_native_d3d9_boundary_checks.py with -DKISAK_LINUX_NATIVE_D3D9 and
// the DXVK include directory; the runner requires the boundary's own #error text.
#include <linux/d3d9_native.h>
int main() { return 0; }

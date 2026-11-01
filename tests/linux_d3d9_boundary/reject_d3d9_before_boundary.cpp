// Must NOT compile: a D3D9 header included ahead of the boundary would declare
// DXVK's types with DXVK's macros and no firewall. The runner requires the
// boundary's own #error text, not merely some failure.
#include <d3d9.h>
#include <linux/d3d9_types.h>
int main() { return 0; }

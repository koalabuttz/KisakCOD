# Recast & Detour

Navigation mesh building (Recast) and path finding (Detour) for KisakCOD's
multiplayer bots (`src/bots/bot_navmesh.cpp`, docs/bots.md).

- Upstream: https://github.com/recastnavigation/recastnavigation
- Version: v1.6.0 (commit 6dc1667f580357e8a2154c28b7867bea7e8ad3a7)
- Licence: zlib (`License.txt`), compatible with KisakCOD's GPLv3.

Only the `Recast` and `Detour` libraries are here, unmodified. Each library's
`Include/` and `Source/` files are flattened into one folder, so the sources'
own `#include "Recast.h"` style lines resolve without extra include paths.
KisakCOD's code includes them as `<recastnavigation/Recast/Recast.h>` and
`<recastnavigation/Detour/DetourNavMesh.h>`.

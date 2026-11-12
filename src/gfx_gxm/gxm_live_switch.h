// Run switches of the GXM back end that can change while the game runs.
//
// The back end's comparing switches are empty marker files in the program
// folder, read once at start-up, so comparing two ways took two starts on
// two sides of an FTP round trip. The ones listed in gxm_legacy_draw.cpp
// (kLiveSwitches) can also be turned by the console command
// "vita_gxm <marker> <0|1>" (src/vita/client/vita_d3d9.cpp), which a run's
// step file sends: both ways are then measured minutes apart in one start,
// at the same temperature and battery state. 1 is "as if the marker were
// there". The marker files are not touched.

#pragma once

namespace gfx_gxm
{
// Asks for the switch to be turned at the back end's next frame; any thread.
// False when the name is not a switch that can change while the game runs.
bool RequestLiveSwitch(const char *name, bool on);
} // namespace gfx_gxm

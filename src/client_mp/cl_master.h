#pragma once

#include <qcommon/master_server.h>

// The master the player's last `globalservers` asked; see qcommon/master_server.h.
// Owned by cl_main_mp.cpp, written by CL_GlobalServers_f, read by
// CL_ServersResponsePacket.
extern MasterQuery cl_masterQuery;

// Makes masterServerName and masterPort refuse changes while the client is
// connected to a remote server, so a game server cannot choose the master.
void CL_ProtectMasterDvars();

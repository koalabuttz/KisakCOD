#pragma once

#include <universal/q_shared.h>
// Cross-TU declarations for connection receive helpers that remain in cl_main.
void CL_UpdateInfoPacket(netadr_t from);
void CL_ServersResponsePacket(netadr_t from, struct msg_t *msg);
extern int32_t autoupdateChecked;

// Reset on cancellation/disconnect. A response can advance the handshake only
// after this session actually emitted an authenticated request to that endpoint.
void CL_ResetConnectionHandshake(int localClientNum);
bool CL_ChallengeResponseAllowed(int localClientNum, netadr_t from);

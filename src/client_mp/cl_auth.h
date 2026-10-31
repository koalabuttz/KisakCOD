#pragma once

#include <cstddef>
#include <cstdint>

enum class ClientAuthResult { ready, unavailable, invalidTicket, bufferTooSmall };

// Formats the existing Steam getchallenge request. Borrows ticket storage only
// for this call; never acquires, cancels, prints or retains a ticket.
ClientAuthResult CL_FormatSteamChallenge(const unsigned char *ticket, uint32_t size,
    uint64_t steamId, char *output, size_t capacity);
// Uses the platform's existing ticket lifetime. Linux currently fails closed.
ClientAuthResult CL_GetPlatformChallenge(char *output, size_t capacity);
void CL_CancelPlatformAuthTicket();
const char *CL_AuthResultName(ClientAuthResult result);

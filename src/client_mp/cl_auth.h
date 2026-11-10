#pragma once

#include <cstddef>
#include <cstdint>

enum class KeyCodeStatus;
enum class ClientAuthResult { ready, unavailable, invalidTicket, bufferTooSmall };
enum class ClientAuthMethod { none, steam, keyCode };

struct netadr_t;

// Formats the existing Steam getchallenge request. Borrows ticket storage only
// for this call; never acquires, cancels, prints or retains a ticket.
ClientAuthResult CL_FormatSteamChallenge(const unsigned char *ticket, uint32_t size,
    uint64_t steamId, char *output, size_t capacity);
// Formats the retail two-field request, getchallenge 0 "<guid>", for a GUID of
// exactly 32 hex digits.
ClientAuthResult CL_FormatKeyCodeChallenge(const char *guid, char *output, size_t capacity);
// Picks how the next remote connection authenticates, before it starts:
// a Steam ticket bound to the server when Steam is running (unless
// keyCodeOnly), otherwise the GUID of the player's CoD4 key code, read from
// <homePath>/codkey.txt or, on Windows, the retail install's registry value.
// Returns none, with a printed reason, when neither is available.
ClientAuthMethod CL_PrepareRemoteAuth(const netadr_t &server, const char *homePath, bool keyCodeOnly);
// Formats the getchallenge request for the method CL_PrepareRemoteAuth picked
// (the Steam ticket when it never ran). Native Linux without KISAK_LINUX_STEAM
// has only the key code.
ClientAuthResult CL_GetPlatformChallenge(char *output, size_t capacity);
void CL_CancelPlatformAuthTicket();
// The game's key code menu (ui_main_mp.cpp, verifyCDKey and getCDKey).
// CL_SaveKeyCode checks a key code the player typed and saves it as
// <homePath>/codkey.txt, where CL_PrepareRemoteAuth reads it; it prints what
// happened, with the GUID and never the key. CL_StoredKeyCodeStatus says
// whether a connect would find a usable key code now, without printing.
KeyCodeStatus CL_SaveKeyCode(const char *homePath, const char *keyText);
KeyCodeStatus CL_StoredKeyCodeStatus(const char *homePath);
const char *CL_AuthResultName(ClientAuthResult result);

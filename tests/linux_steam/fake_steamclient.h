#pragma once

// Shared by the stand-in steamclient.so and the test that loads it. Plain
// data only, so both sides agree on the layout without Steam's headers.

#include <cstdint>

struct FakeSteamCallback
{
    int id;
    int size; // -1: the payload's natural size
    uint64_t steamId;
    uint64_t ownerSteamId;
    int response;
};

struct FakeSteamControl
{
    // What the library answers.
    bool serveClient = true;
    bool serveUser = true;
    int32_t pipe = 7;
    int32_t user = 3;
    bool loggedOn = true;
    uint64_t steamId = 76561197960287930ull;
    uint32_t ticketHandle = 55;
    unsigned char ticket[64] = {};
    uint32_t ticketSize = 0;
    uint32_t reportedTicketSize = 0; // nonzero: claim this size instead
    int beginResult = 0;
    FakeSteamCallback callbacks[8] = {};
    int callbackCount = 0;
    int callbackNext = 0;

    // What was asked.
    int createCalls = 0;
    int pipesCreated = 0;
    int pipesReleased = 0;
    int32_t releasedPipe = 0;
    int32_t connectPipe = 0;
    int usersReleased = 0;
    int32_t releasedUser = 0;
    int32_t releasedUserPipe = 0;
    uint32_t appIdEnvAtPipe = 0;
    uint32_t gameIdEnvAtPipe = 0;
    bool userVersionMatches = false;
    bool userInterfaceHandlesMatch = false;
    int ticketRequests = 0;
    bool identityIsIp = false;
    uint32_t identityIPv4 = 0;
    uint16_t identityPort = 0;
    int ticketCapacity = 0;
    int beginCalls = 0;
    uint64_t beginSteamId = 0;
    int beginSize = 0;
    bool beginTicketMatches = false;
    int endCalls = 0;
    uint64_t endSteamId = 0;
    int cancelCalls = 0;
    uint32_t cancelledHandle = 0;
    int getCallbackCalls = 0;
    int freeCallbackCalls = 0;
    int freeWithoutCallback = 0;
    int callbackReadWithoutFree = 0;
};

extern "C" FakeSteamControl *FakeSteam_Control();

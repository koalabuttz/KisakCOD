// Read-only discovery frontend sharing the engine command/network runtime.
#include <client_mp/cl_auth.h>
#include <linux/net_internal.h>
#include "diagnostic_host.h"
#include "discovery_session.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace {
bool decimal(std::string_view text, unsigned maximum, unsigned &value)
{
    if (text.empty()) return false;
    value = 0;
    for (char c : text) {
        if (c < '0' || c > '9' || value > (maximum - unsigned(c - '0')) / 10)
            return false;
        value = value * 10 + unsigned(c - '0');
    }
    return value > 0 && value <= maximum;
}

}

int main(int argc, char **argv)
{
    const char *server = nullptr;
    unsigned timeout = 1000;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--help")) {
            std::puts("Usage: KisakCOD-client-headless --server IPv4:port [--timeout-ms 1..10000]\n"
                      "Sends read-only getinfo discovery. No default server; never authenticates or joins.");
            return 0;
        }
        if (!std::strcmp(argv[i], "--server") && i + 1 < argc && !server) server = argv[++i];
        else if (!std::strcmp(argv[i], "--timeout-ms") && i + 1 < argc) {
            if (!decimal(argv[++i], 10000, timeout)) return 2;
        } else {
            std::fputs("Invalid arguments; use --help\n", stderr);
            return 2;
        }
    }
    if (!server) {
        std::fputs("An explicit --server IPv4:port is required\n", stderr);
        return 2;
    }
    try {
        kisak::client::DiscoverySession discovery(server, timeout);
        kisak::client::Runtime runtime;
        char authRequest[1536];
        ClientAuthResult auth = CL_GetPlatformChallenge(authRequest, sizeof(authRequest));
        std::printf("auth=%s connected=false\n", CL_AuthResultName(auth));
        // This frontend never sends authRequest or executes response text.
        // The shared runtime owns NET and platform-ticket teardown.
        std::puts("network=engine-udp connected=false");
        if (!discovery.Start()) {
            std::fprintf(stderr, "%s\n", discovery.Detail().c_str());
            return 5;
        }
        std::puts("discovery=sent connected=false");
        std::fflush(stdout);
        while (discovery.State() == kisak::client::DiscoveryState::pending) {
            NET_Sleep(discovery.MillisecondsRemaining());
            discovery.Poll();
        }
        if (discovery.State() == kisak::client::DiscoveryState::received) {
            std::printf("discovery=received discarded=%u connected=false\n", discovery.Discarded());
            return auth == ClientAuthResult::ready ? 0 : 3;
        }
        std::printf("discovery=timeout discarded=%u connected=false\n", discovery.Discarded());
        return 4;
    } catch (const std::invalid_argument &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Client runtime failed: %s\n", error.what());
        return 5;
    }
}

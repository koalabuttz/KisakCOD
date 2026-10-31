// Read-only discovery diagnostic, not the engine CL_Frame loop or a game client.
#include <client_mp/cl_auth.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {
struct Socket {
    int fd = -1;
    ~Socket() { if (fd >= 0) close(fd); }
};

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

bool validResponse(std::string_view packet, std::string_view challenge)
{
    constexpr std::string_view prefix = "\xff\xff\xff\xffinfoResponse\n";
    if (!packet.starts_with(prefix)) return false;
    packet.remove_prefix(prefix.size());
    if (!packet.empty() && packet.back() == '\0') packet.remove_suffix(1);
    if (packet.find('\0') != packet.npos) return false;
    bool matched = false;
    while (!packet.empty()) {
        if (packet.front() != '\\') return false;
        packet.remove_prefix(1);
        size_t separator = packet.find('\\');
        if (!separator || separator == packet.npos) return false;
        auto key = packet.substr(0, separator);
        packet.remove_prefix(separator + 1);
        separator = packet.find('\\');
        auto value = packet.substr(0, separator);
        if (key == "challenge") {
            if (matched || value != challenge) return false;
            matched = true;
        }
        if (separator == packet.npos) break;
        packet.remove_prefix(separator);
    }
    return matched;
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
    std::string endpoint(server);
    size_t colon = endpoint.find(':');
    unsigned port;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (colon == endpoint.npos || !decimal(std::string_view(endpoint).substr(colon + 1), 65535, port)
        || inet_pton(AF_INET, endpoint.substr(0, colon).c_str(), &address.sin_addr) != 1) {
        std::fputs("Invalid IPv4:port\n", stderr);
        return 2;
    }
    address.sin_port = htons(port);
    char authRequest[1536];
    ClientAuthResult auth = CL_GetPlatformChallenge(authRequest, sizeof(authRequest));
    std::printf("auth=%s connected=false\n", CL_AuthResultName(auth));
    // This diagnostic has no send path for authRequest, even if a provider is
    // added later. A discovery response must never be interpreted as admission.
    Socket socket;
    socket.fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (socket.fd < 0 || connect(socket.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        std::perror("socket startup");
        return 5;
    }
    unsigned char random[8];
    ssize_t generated;
    do { generated = getrandom(random, sizeof(random), 0); } while (generated < 0 && errno == EINTR);
    if (generated != sizeof(random)) { std::fputs("challenge entropy unavailable\n", stderr); return 5; }
    char challenge[17];
    for (unsigned i = 0; i < sizeof(random); ++i) std::snprintf(challenge + i * 2, 3, "%02x", random[i]);
    std::string query("\xff\xff\xff\xffgetinfo ");
    query += challenge;
    if (send(socket.fd, query.data(), query.size(), 0) != static_cast<ssize_t>(query.size())) {
        std::perror("discovery send");
        return 5;
    }
    std::puts("discovery=sent connected=false");
    std::fflush(stdout);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    unsigned discarded = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        pollfd event{socket.fd, POLLIN, 0};
        int ready = poll(&event, 1, static_cast<int>(remaining + 1));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { std::perror("event wait"); return 5; }
        if (!ready) continue;
        char response[2048];
        // Connected UDP restricts replies to the selected source IP and port.
        // MSG_TRUNC reports oversize packets so prefixes cannot masquerade as
        // complete responses. Invalid replies never extend the deadline.
        ssize_t size = recv(socket.fd, response, sizeof(response), MSG_TRUNC);
        if (size < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (size < 0) { std::perror("discovery receive"); return 5; }
        if (size > sizeof(response) || !validResponse(std::string_view(response, size), challenge)) {
            ++discarded;
            continue;
        }
        std::printf("discovery=received discarded=%u connected=false\n", discarded);
        return auth == ClientAuthResult::ready ? 0 : 3;
    }
    std::printf("discovery=timeout discarded=%u connected=false\n", discarded);
    return 4;
}

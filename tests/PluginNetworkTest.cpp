#include "PluginNetwork.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <set>
#include <thread>

namespace {
int failures = 0;
void check(bool ok, const std::string& message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

template <class F>
bool waitUntil(F ready, int ms = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!ready()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// A listening socket that counts how many TCP connections reached it (completed handshakes are queued in
// the backlog even if nobody calls accept, so "pending" covers any connection WinHTTP might have opened).
struct Listener {
    SOCKET s = INVALID_SOCKET;
    unsigned short port = 0;
    mutable int bytes = 0;
    explicit Listener(int family) {
        s = socket(family, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return;
        sockaddr_storage address{};
        int size = 0;
        if (family == AF_INET) {
            auto* in = reinterpret_cast<sockaddr_in*>(&address);
            in->sin_family = AF_INET;
            in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            size = sizeof(*in);
        } else {
            auto* in = reinterpret_cast<sockaddr_in6*>(&address);
            in->sin6_family = AF_INET6;
            in->sin6_addr = in6addr_loopback;
            size = sizeof(*in);
        }
        if (bind(s, reinterpret_cast<sockaddr*>(&address), size) != 0 || listen(s, 8) != 0) {
            closesocket(s);
            s = INVALID_SOCKET;
            return;
        }
        getsockname(s, reinterpret_cast<sockaddr*>(&address), &size);
        port = ntohs(family == AF_INET ? reinterpret_cast<sockaddr_in*>(&address)->sin_port
                                       : reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    }
    ~Listener() { if (s != INVALID_SOCKET) closesocket(s); }
    int pending(int waitMs) const {
        int count = 0;
        for (;;) {
            fd_set read;
            FD_ZERO(&read);
            FD_SET(s, &read);
            timeval timeout{0, (count ? 0 : waitMs) * 1000};
            if (select(0, &read, nullptr, nullptr, &timeout) != 1) return count;
            SOCKET client = accept(s, nullptr, nullptr);
            if (client == INVALID_SOCKET) return count;
            char buffer[512];
            DWORD wait = 500;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&wait), sizeof(wait));
            const int got = recv(client, buffer, sizeof(buffer), 0);
            if (got > 0) bytes += got;
            closesocket(client);
            ++count;
        }
    }
};

void connectRaw(int family, unsigned short port) {
    SOCKET c = socket(family, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_storage address{};
    int size = 0;
    if (family == AF_INET) {
        auto* in = reinterpret_cast<sockaddr_in*>(&address);
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        in->sin_port = htons(port);
        size = sizeof(*in);
    } else {
        auto* in = reinterpret_cast<sockaddr_in6*>(&address);
        in->sin6_family = AF_INET6;
        in->sin6_addr = in6addr_loopback;
        in->sin6_port = htons(port);
        size = sizeof(*in);
    }
    connect(c, reinterpret_cast<sockaddr*>(&address), size);
    closesocket(c);
}

// transportGet has no separate DNS pre-check: the only lookup is the tunnel's, and the tunnel connects to the
// exact address it validated, so there is no window for rebinding between check and connect.
void guardBlocksLoopback(int family, const char* host, const char* label, const char* expected) {
    Listener listener(family);
    if (listener.s == INVALID_SOCKET) {
        std::cout << "skip: cannot listen for " << label << '\n';
        return;
    }
    std::atomic<bool> cancelled{false};
    const auto result = sf::PluginNetwork::transportGet(host, listener.port, "/probe", cancelled);
    check(!result.ok, std::string(label) + ": loopback fetch must fail");
    check(result.text.find(expected) != std::string::npos,
          std::string(label) + ": expected guard error, got: " + result.text);
    const int connections = listener.pending(300);
    check(connections == 0 && listener.bytes == 0,
          std::string(label) + ": no TCP connection or bytes may reach a loopback listener");
    connectRaw(family, listener.port);
    check(listener.pending(1000) == 1, std::string(label) + ": control connection is observed by the listener");
}

void cancellationDropsLateResults() {
    std::atomic<bool> gate{false};
    std::atomic<int> started{0}, finished{0}, sawCancel{0};
    sf::PluginNetwork network([&](const std::string&, const std::atomic<bool>& cancelled) {
        ++started;
        // Deliberately ignores `cancelled` until released, like a transfer stuck in a slow read.
        while (!gate) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (cancelled) ++sawCancel;
        ++finished;
        return sf::PluginNetwork::Result{0, 0, true, 200, std::string(1 << 20, 'x')};
    });
    std::string error;
    check(network.submit(1, 1, "https://a.example/", error) && network.submit(1, 2, "https://a.example/", error) &&
          network.submit(2, 1, "https://a.example/", error) && network.submit(3, 1, "https://a.example/", error),
          "submit four requests");
    check(waitUntil([&] { return started == 4; }), "four workers start");
    network.cancelOwner(1);  // e.g. plugin disabled or permission revoked
    network.cancel(2, 1);
    gate = true;
    check(waitUntil([&] { return finished == 4; }), "fetchers return");
    std::set<std::uint64_t> delivered;
    check(waitUntil([&] {
        for (auto& r : network.take()) delivered.insert(r.owner);
        return delivered.count(3) == 1;
    }), "uncancelled request is delivered");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (auto& r : network.take()) delivered.insert(r.owner);
    check(delivered == std::set<std::uint64_t>{3}, "no results after cancelOwner/cancel");
    check(sawCancel == 3, "cancelled in-flight transfers see their abort flag");
}

void queueIsBounded() {
    std::atomic<bool> gate{false};
    std::atomic<int> started{0};
    sf::PluginNetwork network([&](const std::string&, const std::atomic<bool>& cancelled) {
        ++started;
        while (!gate && !cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return sf::PluginNetwork::Result{};
    });
    std::string error;
    for (std::uint64_t i = 0; i < sf::PluginNetwork::kWorkers; ++i) network.submit(9, i, "https://a.example/", error);
    check(waitUntil([&] { return started == sf::PluginNetwork::kWorkers; }), "workers busy");
    bool accepted = true;
    for (std::uint64_t i = 0; i < sf::PluginNetwork::kQueueLimit; ++i)
        accepted = accepted && network.submit(9, 100 + i, "https://a.example/", error);
    check(accepted, "queue accepts up to its limit");
    check(!network.submit(9, 999, "https://a.example/", error) && error.find("queue is full") != std::string::npos,
          "queue rejects beyond its limit");
    network.cancelOwner(9);
    gate = true;
}

// The resolver must never hold a worker past cancellation: a pending lookup is cancelled and abandoned. Unique,
// uncached names keep the lookup in flight at the moment of cancellation whenever DNS is reachable.
void resolverIsCancellable() {
    std::string error;
    std::atomic<bool> never{false};
    check(!sf::PluginNetwork::resolvePublic("localhost.", 443, never, error) &&
          error.find("private or reserved") != std::string::npos, "resolver refuses loopback answers: " + error);
    int abandoned = 0;
    for (int i = 0; i < 5; ++i) {
        const std::string name = "scifi-" + std::to_string(GetTickCount64()) + "-" + std::to_string(i) + ".example.com";
        std::atomic<bool> cancelled{true};
        const auto start = std::chrono::steady_clock::now();
        const bool ok = sf::PluginNetwork::resolvePublic(name, 443, cancelled, error);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        check(!ok && ms < 500, "cancelled lookup returns promptly (" + std::to_string(ms) + " ms): " + error);
        if (error == "network: cancelled") ++abandoned;
    }
    std::cout << "resolver: " << abandoned << "/5 lookups were abandoned before completing (GetAddrInfoExCancel)\n";
}

}  // namespace

int main(int argc, char** argv) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    // Manual smoke test against a real public host (not run by ctest): PluginNetworkTest --live host /path
    if (argc == 4 && std::string(argv[1]) == "--live") {
        std::atomic<bool> cancelled{false};
        const auto start = std::chrono::steady_clock::now();
        const auto r = sf::PluginNetwork::transportGet(argv[2], 443, argv[3], cancelled);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        std::cout << "ok=" << r.ok << " status=" << r.status << " bytes=" << r.text.size() << " ms=" << ms.count()
                  << "\n" << r.text.substr(0, 160) << '\n';
        return r.ok ? 0 : 2;
    }
    // Names WinHTTP would connect to directly are refused before WinHTTP sees them.
    guardBlocksLoopback(AF_INET, "127.0.0.1", "IPv4 literal", "not an allowed DNS host name");
    guardBlocksLoopback(AF_INET, "localhost", "localhost", "not an allowed DNS host name");
    guardBlocksLoopback(AF_INET6, "[::1]", "IPv6 literal", "not an allowed DNS host name");
    guardBlocksLoopback(AF_INET6, "[::ffff:127.0.0.1]", "IPv4-mapped IPv6 literal", "not an allowed DNS host name");
    // An ordinary DNS name whose answer is loopback (what a rebinding attacker serves) goes through the
    // pinning tunnel, which resolves, refuses and never connects.
    guardBlocksLoopback(AF_INET, "localhost.", "DNS name resolving to 127.0.0.1",
                        "resolved to a private or reserved address; request not sent");
    guardBlocksLoopback(AF_INET6, "localhost.", "DNS name resolving to ::1",
                        "resolved to a private or reserved address; request not sent");
    resolverIsCancellable();
    cancellationDropsLateResults();
    queueIsBounded();
    if (failures) return 1;
    std::cout << "PluginNetworkTest passed\n";
    return 0;
}

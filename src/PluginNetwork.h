#pragma once
// Host-side HTTPS text fetcher for gameplay plugins. Requests are validated by the Lua runtime against
// the consented host list; this service enforces transport policy (public addresses only, no proxy,
// redirects, cookies or credentials, identity encoding, size and time limits) on worker threads and
// hands sanitized UTF-8 text back to the main thread through take().
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sf {

class PluginNetwork {
public:
    struct Result {
        std::uint64_t owner = 0;
        std::uint64_t id = 0;
        bool ok = false;
        int status = 0;
        std::string text;  // body text on success, otherwise a short error message
    };

    static constexpr int kWorkers = 4;
    static constexpr size_t kQueueLimit = 32;

    // Performs one GET of an already-validated URL; must return promptly once `cancelled` becomes true.
    using Fetcher = std::function<Result(const std::string& url, const std::atomic<bool>& cancelled)>;

    PluginNetwork();
    explicit PluginNetwork(Fetcher fetcher);  // tests inject a fetcher; the default is the WinHTTP transport
    ~PluginNetwork();
    PluginNetwork(const PluginNetwork&) = delete;
    PluginNetwork& operator=(const PluginNetwork&) = delete;

    // owner identifies one plugin run; results for a cancelled owner are discarded. url must already
    // have passed pluginnet::parseUrl. Fails only when the global queue is full.
    bool submit(std::uint64_t owner, std::uint64_t id, const std::string& url, std::string& error);
    void cancel(std::uint64_t owner, std::uint64_t id);
    void cancelOwner(std::uint64_t owner);
    std::vector<Result> take();

    // The guarded HTTPS transport (Windows/WinHTTP only; elsewhere it reports "unavailable"). WinHTTP goes
    // through a per-request loopback CONNECT tunnel that resolves the host, rejects any non-public answer
    // and connects only to the address it validated, so a private/loopback/link-local/metadata endpoint
    // receives no TCP connection, TLS handshake or HTTP bytes. Exposed for tests.
    static Result transportGet(const std::string& host, unsigned short port, const std::string& path,
                               const std::atomic<bool>& cancelled);
    // The transport's resolver: asynchronous, bounded by a 5 s timeout and abandoned as soon as `cancelled`
    // is set (the pending lookup is cancelled, never joined). Succeeds only if every answer is public.
    static bool resolvePublic(const std::string& host, unsigned short port, const std::atomic<bool>& cancelled,
                              std::string& error);

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace sf

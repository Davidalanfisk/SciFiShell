#include "PluginNetwork.h"

#include "PluginNetPolicy.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#endif

namespace sf {

namespace {
struct Job {
    std::uint64_t owner = 0, id = 0;
    std::string url;
};

#ifdef _WIN32
constexpr int kResolveMs = 5000, kConnectMs = 5000, kSendMs = 5000, kReceiveMs = 10000;
constexpr auto kTotalTime = std::chrono::seconds(15);

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

std::wstring widen(std::string_view ascii) { return std::wstring(ascii.begin(), ascii.end()); }

bool publicAddress(const sockaddr* address) {
    if (address->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(address);
        return pluginnet::publicIpv4(reinterpret_cast<const std::uint8_t*>(&in->sin_addr));
    }
    if (address->sa_family == AF_INET6) {
        const auto* in = reinterpret_cast<const sockaddr_in6*>(address);
        return pluginnet::publicIpv6(reinterpret_cast<const std::uint8_t*>(&in->sin6_addr));
    }
    return false;
}

std::wstring queryHeader(HINTERNET request, DWORD info) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 4096) return {};
    std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    value.resize(size / sizeof(wchar_t));
    return value;
}

std::string narrow(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) out.push_back(c > 0 && c < 128 ? static_cast<char>(c) : '?');
    return out;
}

// One request's state shared between the worker and WinHTTP's status callback. The callback holds a
// reference through the request handle's context value until WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING.
struct Transfer {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HINTERNET request = nullptr;
    std::atomic<bool> closed{false}, blocked{false}, verified{false}, sent{false}, headers{false}, secureFailure{false};
    std::atomic<DWORD> error{0};
    std::atomic<std::uint32_t> reads{0};
    std::atomic<DWORD> bytes{0};
    std::shared_ptr<std::atomic<bool>> tunnelAccepted;
    char buffer[16384];
    ~Transfer() { if (event) CloseHandle(event); }
};

void closeRequest(Transfer& t) {
    if (t.request && !t.closed.exchange(true)) WinHttpCloseHandle(t.request);
}

bool socketsReady() {
    static const bool started = [] { WSADATA data; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
    return started;
}

// WinHTTP must only ever connect to the request's own loopback tunnel.
bool tunnelAddressText(const wchar_t* text) {
    if (!text) return false;
    IN_ADDR v4{};
    return InetPtonW(AF_INET, text, &v4) == 1 && v4.s_addr == htonl(INADDR_LOOPBACK);
}

// Runs on WinHTTP threads. Any connection other than to the loopback tunnel (e.g. if WinHTTP ever bypassed
// the proxy) closes the request before SENDING_REQUEST; SENDING_REQUEST refuses unless the connect was verified.
void CALLBACK onStatus(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD length) {
    if (!context) return;
    auto* holder = reinterpret_cast<std::shared_ptr<Transfer>*>(context);
    Transfer& t = **holder;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER:
    case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER:
        if (!t.blocked && tunnelAddressText(static_cast<const wchar_t*>(info))) {
            t.verified = true;
        } else {
            t.blocked = true;
            t.verified = false;
            closeRequest(t);
            SetEvent(t.event);
        }
        break;
    case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST:
        if (!t.verified || t.blocked || !t.tunnelAccepted || !*t.tunnelAccepted) {
            t.blocked = true;
            closeRequest(t);
            SetEvent(t.event);
        }
        break;
    case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
        t.secureFailure = true;
        break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        t.sent = true;
        SetEvent(t.event);
        break;
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        t.headers = true;
        SetEvent(t.event);
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        t.bytes = length;
        ++t.reads;
        SetEvent(t.event);
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        const auto* result = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
        t.error = result && result->dwError ? result->dwError : static_cast<DWORD>(ERROR_WINHTTP_INTERNAL_ERROR);
        SetEvent(t.event);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        t.closed = true;
        SetEvent(t.event);
        delete holder;
        break;
    default:
        break;
    }
}

struct Socket {
    SOCKET s = INVALID_SOCKET;
    Socket() = default;
    explicit Socket(SOCKET value) : s(value) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { reset(); }
    void reset(SOCKET value = INVALID_SOCKET) {
        if (s != INVALID_SOCKET) closesocket(s);
        s = value;
    }
};

// Cancellable DNS. GetAddrInfoExW runs asynchronously with its own timeout; the caller polls `aborted` and
// cancels a pending lookup, so a stalled resolver can never hold a worker or tunnel thread past its deadline.
// The operation owns itself: whichever of the caller and the completion routine finishes last frees it.
struct ResolveOp {
    OVERLAPPED overlapped{};  // must stay first: the completion routine receives this pointer
    ADDRINFOEXW* result = nullptr;
    HANDLE cancelHandle = nullptr;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<DWORD> status{0};
    std::atomic<int> refs{2};
    void release() {
        if (--refs != 0) return;
        if (result) FreeAddrInfoExW(result);
        if (done) CloseHandle(done);
        delete this;
    }
};

void CALLBACK resolveComplete(DWORD error, DWORD, LPWSAOVERLAPPED overlapped) {
    auto* op = reinterpret_cast<ResolveOp*>(overlapped);
    op->status = error;
    SetEvent(op->done);
    op->release();
}

}  // namespace

static bool resolveHost(const std::string& host, unsigned short port, const std::function<bool()>& aborted,
                 std::vector<std::pair<sockaddr_storage, int>>& addresses, bool& allPublic, std::string& error) {
    addresses.clear();
    allPublic = true;
    if (!socketsReady()) { error = "network: sockets unavailable"; return false; }
    std::string name = host;
    if (name.size() > 2 && name.front() == '[' && name.back() == ']') name = name.substr(1, name.size() - 2);
    auto* op = new ResolveOp;
    if (!op->done) { op->refs = 1; op->release(); error = "network: out of resources"; return false; }
    ADDRINFOEXW hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    timeval timeout{kResolveMs / 1000, 0};
    const std::wstring wideName = widen(name), widePort = std::to_wstring(port);
    const int started = GetAddrInfoExW(wideName.c_str(), widePort.c_str(), NS_DNS, nullptr, &hints, &op->result,
                                       &timeout, &op->overlapped, resolveComplete, &op->cancelHandle);
    if (started != WSA_IO_PENDING) {
        op->refs = 1;  // completed synchronously; the completion routine will not run
        op->status = static_cast<DWORD>(started);
    } else {
        for (;;) {
            if (aborted() && WaitForSingleObject(op->done, 0) == WAIT_TIMEOUT) {
                GetAddrInfoExCancel(&op->cancelHandle);
                op->release();
                error = "network: cancelled";
                return false;
            }
            if (WaitForSingleObject(op->done, 50) != WAIT_TIMEOUT) break;
        }
    }
    bool ok = op->status == 0 && op->result;
    if (ok) {
        for (auto* it = op->result; it; it = it->ai_next) {
            if (!it->ai_addr || it->ai_addrlen > sizeof(sockaddr_storage) || !publicAddress(it->ai_addr)) {
                allPublic = false;
                continue;
            }
            sockaddr_storage copy{};
            std::memcpy(&copy, it->ai_addr, it->ai_addrlen);
            addresses.emplace_back(copy, static_cast<int>(it->ai_addrlen));
        }
        // One private answer refuses the host: a mixed answer is a classic rebinding/SSRF setup.
        if (!allPublic || addresses.empty()) {
            ok = false;
            addresses.clear();
            error = "network: " + host + " resolved to a private or reserved address; request not sent";
        }
    } else {
        error = op->status == WSAETIMEDOUT ? "network: DNS lookup for " + host + " timed out"
                                           : "network: cannot resolve " + host;
    }
    op->release();
    return ok;
}

namespace {

// Address pinning. WinHTTP sends the request through this single-use loopback CONNECT proxy, which resolves
// the declared host itself, refuses when *any* answer is private/loopback/link-local/metadata/reserved, and
// opens a TCP connection only to an address it has just validated. Nothing (not even a TCP SYN) reaches a
// non-public address, and there is no window for DNS rebinding between validation and connect. TLS stays end
// to end between WinHTTP and the server, so SNI and certificate validation use the declared host name; the
// tunnel only relays opaque bytes, with byte, time and cancellation limits.
class Tunnel {
public:
    Tunnel(std::string host, unsigned short port, std::chrono::steady_clock::time_point deadline,
           const std::atomic<bool>& cancelled)
        : host_(std::move(host)), port_(port), deadline_(deadline), cancelled_(cancelled) {}
    Tunnel(const Tunnel&) = delete;
    Tunnel& operator=(const Tunnel&) = delete;
    ~Tunnel() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    bool start() {
        if (!socketsReady()) return false;
        listener_.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (listener_.s == INVALID_SOCKET) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof(address);
        if (bind(listener_.s, reinterpret_cast<sockaddr*>(&address), size) != 0 || listen(listener_.s, 1) != 0 ||
            getsockname(listener_.s, reinterpret_cast<sockaddr*>(&address), &size) != 0)
            return false;
        listenPort_ = ntohs(address.sin_port);
        try {
            thread_ = std::thread([this] { run(); });
        } catch (const std::system_error&) {
            return false;
        }
        return true;
    }
    unsigned short listenPort() const { return listenPort_; }
    const std::shared_ptr<std::atomic<bool>>& accepted() const { return accepted_; }
    bool established() const { return established_; }
    std::string error() const {
        std::lock_guard lock(mutex_);
        return error_;
    }

private:
    static constexpr size_t kMaxDown = pluginnet::kMaxBodyBytes + 512 * 1024;  // body + headers + TLS framing
    static constexpr size_t kMaxUp = 256 * 1024;

    bool aborted() const { return stop_ || cancelled_ || std::chrono::steady_clock::now() >= deadline_; }
    void setError(std::string message) {
        std::lock_guard lock(mutex_);
        if (error_.empty()) error_ = std::move(message);
    }
    static bool ready(SOCKET s, bool write) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        fd_set failed = set;
        timeval timeout{0, 50 * 1000};
        return select(0, write ? nullptr : &set, write ? &set : nullptr, write ? &failed : nullptr, &timeout) > 0;
    }
    bool sendAll(SOCKET s, const char* data, size_t size) {
        while (size > 0) {
            const int sent = send(s, data, static_cast<int>(std::min<size_t>(size, 65536)), 0);
            if (sent > 0) {
                data += sent;
                size -= static_cast<size_t>(sent);
            } else if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                if (aborted()) return false;
                ready(s, true);
            } else {
                return false;
            }
        }
        return true;
    }
    void refuse(SOCKET client, std::string message) {
        setError(std::move(message));
        static const char reply[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        sendAll(client, reply, sizeof(reply) - 1);
    }

    void run() {
        Socket client;
        while (client.s == INVALID_SOCKET) {
            if (aborted()) return;
            if (ready(listener_.s, false)) client.reset(accept(listener_.s, nullptr, nullptr));
        }
        listener_.reset();
        *accepted_ = true;
        u_long nonBlocking = 1;
        ioctlsocket(client.s, FIONBIO, &nonBlocking);

        std::string request;
        char buffer[16384];
        while (request.find("\r\n\r\n") == std::string::npos) {
            if (request.size() > 8192 || aborted()) return;
            if (!ready(client.s, false)) continue;
            const int got = recv(client.s, buffer, sizeof(buffer), 0);
            if (got <= 0) return;
            request.append(buffer, static_cast<size_t>(got));
        }
        const std::string expected = "CONNECT " + host_ + ":" + std::to_string(port_) + " HTTP/1.";
        if (request.compare(0, expected.size(), expected) != 0)
            return refuse(client.s, "network: unexpected tunnel request");

        std::vector<std::pair<sockaddr_storage, int>> addresses;
        bool allPublic = true;
        std::string resolveError;
        if (!resolveHost(host_, port_, [this] { return aborted(); }, addresses, allPublic, resolveError)) {
            if (aborted()) return;
            return refuse(client.s, std::move(resolveError));
        }
        // One private answer refuses the host: a mixed answer is a classic rebinding/SSRF setup.
        if (!allPublic || addresses.empty())
            return refuse(client.s, "network: " + host_ + " resolved to a private or reserved address; request not sent");

        Socket upstream;
        for (auto& [address, size] : addresses) {
            Socket candidate(socket(address.ss_family, SOCK_STREAM, IPPROTO_TCP));
            if (candidate.s == INVALID_SOCKET) continue;
            ioctlsocket(candidate.s, FIONBIO, &nonBlocking);
            if (connect(candidate.s, reinterpret_cast<const sockaddr*>(&address), size) != 0 &&
                WSAGetLastError() != WSAEWOULDBLOCK)
                continue;
            const auto connectDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectMs);
            bool connected = false;
            while (!aborted() && std::chrono::steady_clock::now() < connectDeadline) {
                if (!ready(candidate.s, true)) continue;
                int code = 0, codeSize = sizeof(code);
                getsockopt(candidate.s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &codeSize);
                connected = code == 0;
                break;
            }
            if (aborted()) return;
            if (connected) {
                upstream.reset(candidate.s);
                candidate.s = INVALID_SOCKET;
                break;
            }
        }
        if (upstream.s == INVALID_SOCKET) {
            setError("network: cannot connect to " + host_);
            static const char reply[] = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client.s, reply, sizeof(reply) - 1);
            return;
        }
        static const char ok[] = "HTTP/1.1 200 Connection established\r\n\r\n";
        if (!sendAll(client.s, ok, sizeof(ok) - 1)) return;
        established_ = true;

        size_t up = 0, down = 0;
        while (!aborted()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(client.s, &set);
            FD_SET(upstream.s, &set);
            timeval timeout{0, 50 * 1000};
            if (select(0, &set, nullptr, nullptr, &timeout) <= 0) continue;
            for (const bool fromClient : {true, false}) {
                SOCKET from = fromClient ? client.s : upstream.s;
                if (!FD_ISSET(from, &set)) continue;
                const int got = recv(from, buffer, sizeof(buffer), 0);
                if (got == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
                if (got <= 0) return;
                size_t& total = fromClient ? up : down;
                total += static_cast<size_t>(got);
                if (total > (fromClient ? kMaxUp : kMaxDown)) {
                    if (!fromClient) setError("network: response exceeds 1 MiB");
                    return;
                }
                if (!sendAll(fromClient ? upstream.s : client.s, buffer, static_cast<size_t>(got))) return;
            }
        }
    }

    std::string host_;
    unsigned short port_;
    std::chrono::steady_clock::time_point deadline_;
    const std::atomic<bool>& cancelled_;
    std::atomic<bool> stop_{false}, established_{false};
    std::shared_ptr<std::atomic<bool>> accepted_ = std::make_shared<std::atomic<bool>>(false);
    Socket listener_;
    unsigned short listenPort_ = 0;
    mutable std::mutex mutex_;
    std::string error_;
    std::thread thread_;
};

enum class Wait { Done, Failed, TimedOut, Cancelled };

template <class Ready>
Wait waitFor(Transfer& t, Ready ready, std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& cancelled) {
    for (;;) {
        if (t.blocked || t.error) return Wait::Failed;
        if (ready()) return Wait::Done;
        if (cancelled.load()) return Wait::Cancelled;
        if (std::chrono::steady_clock::now() >= deadline) return Wait::TimedOut;
        WaitForSingleObject(t.event, 50);
    }
}

}  // namespace

PluginNetwork::Result PluginNetwork::transportGet(const std::string& host, unsigned short port, const std::string& path,
                                                  const std::atomic<bool>& cancelled) {
    Result result;
    const auto deadline = std::chrono::steady_clock::now() + kTotalTime;
    auto fail = [&](std::string message, int status = 0) {
        result.ok = false;
        result.status = status;
        result.text = std::move(message);
        return result;
    };
    // WinHTTP connects directly (bypassing any proxy) to IP literals and "localhost", so those never reach it.
    // Plugin URLs already exclude them; this keeps the transport fail-closed on its own.
    {
        std::string name = host;
        if (name.size() > 2 && name.front() == '[' && name.back() == ']') name = name.substr(1, name.size() - 2);
        IN_ADDR v4{};
        IN6_ADDR v6{};
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool symbolsOk = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-';
        });
        if (!symbolsOk || lower == "localhost" || InetPtonA(AF_INET, name.c_str(), &v4) == 1 ||
            InetPtonA(AF_INET6, name.c_str(), &v6) == 1)
            return fail("network: " + host + " is not an allowed DNS host name; request not sent");
    }
    // Declared first so it outlives (and is joined after) every WinHTTP handle below.
    Tunnel tunnel(host, port, deadline, cancelled);
    if (!tunnel.start()) return fail("network: cannot start address guard");
    const std::wstring proxy = L"127.0.0.1:" + std::to_wstring(tunnel.listenPort());
    // "<-loopback>" removes any implicit loopback bypass, so every connection goes through the tunnel.
    Handle session{WinHttpOpen(L"SciFiShell-Plugin/1", WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy.c_str(),
                               L"<-loopback>", WINHTTP_FLAG_ASYNC)};
    if (!session.h) return fail("network: cannot open HTTP session");
    if (WinHttpSetStatusCallback(session.h, onStatus, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0) ==
        WINHTTP_INVALID_STATUS_CALLBACK)
        return fail("network: cannot install address guard");
    WinHttpSetTimeouts(session.h, kResolveMs, kConnectMs, kSendMs, kReceiveMs);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(session.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    // A fresh session per request: no pooled connection or cached tunnel is ever reused.
    Handle connection{WinHttpConnect(session.h, widen(host).c_str(), port, 0)};
    if (!connection.h) return fail("network: cannot connect to " + host);
    LPCWSTR accept[] = {L"text/*", L"application/json", L"application/xml", L"application/rss+xml",
                        L"application/atom+xml", nullptr};
    HINTERNET request = WinHttpOpenRequest(connection.h, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                           accept, WINHTTP_FLAG_SECURE | WINHTTP_FLAG_ESCAPE_DISABLE);
    if (!request) return fail("network: cannot create request");
    auto transfer = std::make_shared<Transfer>();
    transfer->request = request;
    transfer->tunnelAccepted = tunnel.accepted();
    auto* holder = new (std::nothrow) std::shared_ptr<Transfer>(transfer);
    DWORD_PTR contextValue = reinterpret_cast<DWORD_PTR>(holder);
    if (!transfer->event || !holder ||
        !WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &contextValue, sizeof(contextValue))) {
        delete holder;
        WinHttpCloseHandle(request);
        return fail("network: cannot create request");
    }
    // From here the callback owns `holder`; the request is closed exactly once, by the guard or below.
    struct CloseGuard {
        Transfer& t;
        ~CloseGuard() { closeRequest(t); }
    } guard{*transfer};
    Transfer& t = *transfer;
    DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon));

    auto failed = [&](Wait wait, int status = 0) {
        if (auto message = tunnel.error(); !message.empty()) return fail(std::move(message), status);
        if (t.blocked) return fail("network: connection bypassed the address guard; request not sent", status);
        if (wait == Wait::Cancelled) return fail("network: cancelled", status);
        const DWORD code = t.error;
        if (wait == Wait::TimedOut || code == ERROR_WINHTTP_TIMEOUT) return fail("network: request timed out", status);
        if (t.secureFailure || code == ERROR_WINHTTP_SECURE_FAILURE)
            return fail("network: TLS certificate validation failed", status);
        if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED) return fail("network: cannot resolve " + host, status);
        if (code == ERROR_WINHTTP_CANNOT_CONNECT) return fail("network: cannot connect to " + host, status);
        return fail("network: request failed (WinHTTP error " + std::to_string(code) + ")", status);
    };
    const wchar_t* headers = L"Accept-Encoding: identity\r\n";
    if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        if (!t.error) t.error = GetLastError();
        return failed(Wait::Failed);
    }
    if (const auto w = waitFor(t, [&] { return t.sent.load(); }, deadline, cancelled); w != Wait::Done) return failed(w);
    if (!WinHttpReceiveResponse(request, nullptr)) {
        if (!t.error) t.error = GetLastError();
        return failed(Wait::Failed);
    }
    if (const auto w = waitFor(t, [&] { return t.headers.load(); }, deadline, cancelled); w != Wait::Done) return failed(w);

    // A response that did not come through an established, validated tunnel is never used.
    if (!t.verified || !tunnel.established()) {
        if (auto message = tunnel.error(); !message.empty()) return fail(std::move(message));
        return fail("network: request did not pass the address guard");
    }

    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status >= 300 && status < 400) return fail("network: redirects are not followed", static_cast<int>(status));
    if (status < 200 || status >= 300) return fail("network: HTTP status " + std::to_string(status), static_cast<int>(status));
    const std::string encoding = narrow(queryHeader(request, WINHTTP_QUERY_CONTENT_ENCODING));
    if (!encoding.empty() && encoding != "identity")
        return fail("network: compressed responses are not accepted", static_cast<int>(status));
    const std::string contentType = narrow(queryHeader(request, WINHTTP_QUERY_CONTENT_TYPE));
    if (!pluginnet::textContentType(contentType))
        return fail("network: response is not text (" + (contentType.empty() ? std::string("no type") : contentType) + ")",
                    static_cast<int>(status));
    DWORD length = 0, lengthSize = sizeof(length);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &length, &lengthSize, WINHTTP_NO_HEADER_INDEX) &&
        length > pluginnet::kMaxBodyBytes)
        return fail("network: response exceeds 1 MiB", static_cast<int>(status));

    std::string body;
    for (;;) {
        const std::uint32_t before = t.reads;
        if (!WinHttpReadData(request, t.buffer, sizeof(t.buffer), nullptr)) {
            if (!t.error) t.error = GetLastError();
            return failed(Wait::Failed, static_cast<int>(status));
        }
        if (const auto w = waitFor(t, [&] { return t.reads.load() != before; }, deadline, cancelled); w != Wait::Done)
            return failed(w, static_cast<int>(status));
        const DWORD read = t.bytes;
        if (read == 0) break;
        if (read > sizeof(t.buffer) || body.size() + read > pluginnet::kMaxBodyBytes)
            return fail("network: response exceeds 1 MiB", static_cast<int>(status));
        body.append(t.buffer, read);
    }
    std::string error;
    if (!pluginnet::sanitizeText(body, contentType, error)) return fail("network: " + error, static_cast<int>(status));
    result.ok = true;
    result.status = static_cast<int>(status);
    result.text = std::move(body);
    return result;
}

namespace {
PluginNetwork::Result fetch(const std::string& jobUrl, const std::atomic<bool>& cancelled) {
    PluginNetwork::Result result;
    pluginnet::Url url;
    std::string error;
    // The runtime already validated the URL against declared hosts; re-parse against its own host.
    const auto hostStart = jobUrl.find("://") + 3;
    const auto hostEnd = jobUrl.find_first_of("/:?", hostStart);
    const std::vector<std::string> hosts{jobUrl.substr(hostStart, hostEnd - hostStart)};
    if (!pluginnet::parseUrl(jobUrl, hosts, url, error)) {
        result.text = "network: " + error;
        return result;
    }
    return PluginNetwork::transportGet(url.host, INTERNET_DEFAULT_HTTPS_PORT, url.path, cancelled);
}
#else
PluginNetwork::Result fetch(const std::string&, const std::atomic<bool>&) {
    return {0, 0, false, 0, "network: unavailable on this platform"};
}
#endif
}  // namespace

#ifdef _WIN32
bool PluginNetwork::resolvePublic(const std::string& host, unsigned short port, const std::atomic<bool>& cancelled,
                                  std::string& error) {
    std::vector<std::pair<sockaddr_storage, int>> addresses;
    bool allPublic = true;
    return resolveHost(host, port, [&] { return cancelled.load(); }, addresses, allPublic, error);
}
#else
PluginNetwork::Result PluginNetwork::transportGet(const std::string&, unsigned short, const std::string&,
                                                  const std::atomic<bool>&) {
    return {0, 0, false, 0, "network: unavailable on this platform"};
}

bool PluginNetwork::resolvePublic(const std::string&, unsigned short, const std::atomic<bool>&, std::string& error) {
    error = "network: unavailable on this platform";
    return false;
}
#endif

struct PluginNetwork::State {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> queue;
    std::vector<Result> done;
    std::set<std::uint64_t> cancelledOwners;
    std::set<std::pair<std::uint64_t, std::uint64_t>> cancelledJobs;
    // In-flight requests' abort flags, so cancel/cancelOwner/shutdown stop a transfer mid-way.
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::shared_ptr<std::atomic<bool>>> running;
    PluginNetwork::Fetcher fetcher;
    bool stopping = false;
    int workers = 0, busy = 0;

    bool dropped(std::uint64_t owner, std::uint64_t id) const {
        return stopping || cancelledOwners.count(owner) || cancelledJobs.count({owner, id});
    }
};

PluginNetwork::PluginNetwork() : PluginNetwork(Fetcher(fetch)) {}

PluginNetwork::PluginNetwork(Fetcher fetcher) : state_(std::make_shared<State>()) {
    state_->fetcher = std::move(fetcher);
}

PluginNetwork::~PluginNetwork() {
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->queue.clear();
        for (auto& [key, flag] : state_->running) *flag = true;
    }
    state_->wake.notify_all();
    // Workers own a reference to the state and finish their current bounded request on their own; they
    // are detached so app shutdown never waits on a slow server.
}

bool PluginNetwork::submit(std::uint64_t owner, std::uint64_t id, const std::string& url, std::string& error) {
    std::unique_lock lock(state_->mutex);
    if (state_->queue.size() >= kQueueLimit) {
        error = "network: host request queue is full";
        return false;
    }
    state_->queue.push_back({owner, id, url});
    if (state_->workers < kWorkers && state_->workers - state_->busy < static_cast<int>(state_->queue.size())) {
        ++state_->workers;
        auto state = state_;
        try {
            std::thread([state] {
                std::unique_lock lock(state->mutex);
                for (;;) {
                    state->wake.wait_for(lock, std::chrono::seconds(30),
                                         [&] { return state->stopping || !state->queue.empty(); });
                    if (state->stopping || state->queue.empty()) break;
                    Job job = std::move(state->queue.front());
                    state->queue.pop_front();
                    ++state->busy;
                    const std::pair key{job.owner, job.id};
                    auto flag = std::make_shared<std::atomic<bool>>(state->dropped(job.owner, job.id));
                    state->running[key] = flag;
                    lock.unlock();
                    Result result;
                    try {
                        result = state->fetcher(job.url, *flag);
                    } catch (const std::exception&) {
                        result = {0, 0, false, 0, "network: out of memory"};
                    }
                    result.owner = job.owner;
                    result.id = job.id;
                    lock.lock();
                    --state->busy;
                    state->running.erase(key);
                    // Checked under the same lock cancel()/cancelOwner() take, so nothing is delivered
                    // after either returns.
                    if (!*flag && !state->dropped(job.owner, job.id)) state->done.push_back(std::move(result));
                    state->cancelledJobs.erase(key);
                }
                --state->workers;
            }).detach();
        } catch (const std::system_error&) {
            --state_->workers;
            if (state_->workers == 0) {
                state_->queue.pop_back();
                error = "network: cannot start worker";
                return false;
            }
        }
    }
    lock.unlock();
    state_->wake.notify_one();
    return true;
}

void PluginNetwork::cancel(std::uint64_t owner, std::uint64_t id) {
    std::lock_guard lock(state_->mutex);
    auto& queue = state_->queue;
    const auto before = queue.size();
    queue.erase(std::remove_if(queue.begin(), queue.end(),
                               [&](const Job& job) { return job.owner == owner && job.id == id; }), queue.end());
    if (queue.size() == before) {
        if (auto it = state_->running.find({owner, id}); it != state_->running.end()) {
            *it->second = true;
            state_->cancelledJobs.insert({owner, id});
        }
    }
    auto& done = state_->done;
    done.erase(std::remove_if(done.begin(), done.end(),
                              [&](const Result& r) { return r.owner == owner && r.id == id; }), done.end());
}

void PluginNetwork::cancelOwner(std::uint64_t owner) {
    std::lock_guard lock(state_->mutex);
    auto& queue = state_->queue;
    queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const Job& job) { return job.owner == owner; }),
                queue.end());
    auto& done = state_->done;
    done.erase(std::remove_if(done.begin(), done.end(), [&](const Result& r) { return r.owner == owner; }), done.end());
    // Owners are unique per plugin run, so remembering them is bounded by the number of runs.
    state_->cancelledOwners.insert(owner);
    for (auto& [key, flag] : state_->running)
        if (key.first == owner) *flag = true;
}

std::vector<PluginNetwork::Result> PluginNetwork::take() {
    std::lock_guard lock(state_->mutex);
    std::vector<Result> result;
    result.swap(state_->done);
    return result;
}

}  // namespace sf

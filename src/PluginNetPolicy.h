#pragma once
// Pure validation and text helpers for the gameplay plugin network API. No sockets here: the
// policy is shared by manifest parsing, the Lua runtime and the WinHTTP host so it can be tested.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sf::pluginnet {

inline constexpr std::size_t kMaxHosts = 4;
inline constexpr std::size_t kMaxHostsLength = 100;
inline constexpr std::size_t kMaxHostLength = 63;
inline constexpr std::size_t kMaxUrlLength = 2048;
inline constexpr std::size_t kMaxBodyBytes = 1024 * 1024;
inline constexpr std::size_t kMaxPendingPerPlugin = 4;
inline constexpr double kRequestBurst = 4;
inline constexpr double kRequestRefillSeconds = 5;
inline constexpr std::size_t kMaxFeedItems = 50;

// Lowercase DNS name with at least two labels; IP literals, wildcards, ports and "localhost" are rejected.
bool validHostName(std::string_view host);
bool validHostList(const std::vector<std::string>& hosts, std::string& error);

struct Url {
    std::string host;
    std::string path;  // path plus optional query, always starts with '/'
};
// Accepts only https://<declared host>[:443]/<printable ASCII path/query>; no userinfo or fragment.
bool parseUrl(std::string_view url, const std::vector<std::string>& hosts, Url& out, std::string& error);

// True only for globally routable unicast addresses. Private, loopback, link-local (including cloud
// metadata 169.254.0.0/16), CGNAT, multicast, reserved, documentation and IPv4-mapped forms are refused.
bool publicIpv4(const std::uint8_t address[4]);
bool publicIpv6(const std::uint8_t address[16]);

// Text-like content types only: text/*, application/json, application/xml and +json/+xml/rss/atom.
bool textContentType(std::string_view contentType);
// Validates and normalizes a response body to UTF-8 text: strips a BOM, converts ISO-8859-1 when the
// declared charset says so, rejects invalid UTF-8, NUL and other control characters except tab/CR/LF.
bool sanitizeText(std::string& body, std::string_view contentType, std::string& error);

struct FeedItem {
    std::string title, link, summary, published;
};
// Extracts RSS <item> / Atom <entry> elements as plain text. Markup is stripped and entities decoded;
// nothing is interpreted or executed. Returns false only for text that contains no feed at all.
bool parseFeed(std::string_view text, std::size_t maxItems, std::vector<FeedItem>& out, std::string& error);

}  // namespace sf::pluginnet

#include "PluginNetPolicy.h"

#include <algorithm>
#include <cctype>

namespace sf::pluginnet {
namespace {
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Returns the length of the UTF-8 sequence at i, or 0 when it is malformed.
size_t utf8Length(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    if (c < 0x80) return 1;
    size_t n = 0;
    std::uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    else return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
        cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return n;
}

std::string mediaType(std::string_view contentType, std::string* charset) {
    std::string lowered;
    lowered.reserve(contentType.size());
    for (char c : contentType) lowered += lower(c);
    const auto semi = lowered.find(';');
    std::string type = lowered.substr(0, semi);
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) type.pop_back();
    while (!type.empty() && (type.front() == ' ' || type.front() == '\t')) type.erase(type.begin());
    if (charset) {
        charset->clear();
        const auto at = lowered.find("charset=", semi == std::string::npos ? lowered.size() : semi);
        if (at != std::string::npos) {
            size_t i = at + 8;
            if (i < lowered.size() && (lowered[i] == '"' || lowered[i] == '\'')) ++i;
            while (i < lowered.size() && lowered[i] != ';' && lowered[i] != '"' && lowered[i] != '\'' &&
                   lowered[i] != ' ')
                *charset += lowered[i++];
        }
    }
    return type;
}

constexpr std::uint16_t kWindows1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

// ---- feed text extraction ----
bool tagBoundary(char c) { return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

struct Element {
    bool found = false;
    size_t start = 0, contentStart = 0, contentEnd = 0, end = 0;
    std::string_view attrs;
};

Element findElement(std::string_view text, std::string_view name, size_t from, size_t limit) {
    Element e;
    limit = std::min(limit, text.size());
    size_t p = from;
    while (p < limit) {
        p = text.find('<', p);
        if (p == std::string_view::npos || p + 1 + name.size() >= limit) return e;
        if (text.compare(p + 1, name.size(), name) == 0 && tagBoundary(text[p + 1 + name.size()])) break;
        ++p;
    }
    if (p >= limit) return e;
    const size_t close = text.find('>', p);
    if (close == std::string_view::npos || close >= limit) return e;
    e.start = p;
    e.attrs = text.substr(p + 1 + name.size(), close - p - 1 - name.size());
    if (close > p && text[close - 1] == '/') {
        e.found = true;
        e.contentStart = e.contentEnd = close + 1;
        e.end = close + 1;
        if (!e.attrs.empty()) e.attrs.remove_suffix(1);
        return e;
    }
    e.contentStart = close + 1;
    size_t q = e.contentStart;
    while (true) {
        q = text.find("</", q);
        if (q == std::string_view::npos || q + 2 + name.size() > limit) return e;
        if (text.compare(q + 2, name.size(), name) == 0 && q + 2 + name.size() < text.size() &&
            (text[q + 2 + name.size()] == '>' || text[q + 2 + name.size()] == ' '))
            break;
        q += 2;
    }
    const size_t end = text.find('>', q);
    if (end == std::string_view::npos || end >= limit + 1) return e;
    e.found = true;
    e.contentEnd = q;
    e.end = end + 1;
    return e;
}

std::string attribute(std::string_view attrs, std::string_view name) {
    size_t p = 0;
    while ((p = attrs.find(name, p)) != std::string_view::npos) {
        const bool startOk = p == 0 || attrs[p - 1] == ' ' || attrs[p - 1] == '\t' || attrs[p - 1] == '\n' ||
                             attrs[p - 1] == '\r';
        size_t i = p + name.size();
        while (i < attrs.size() && attrs[i] == ' ') ++i;
        if (startOk && i < attrs.size() && attrs[i] == '=') {
            ++i;
            while (i < attrs.size() && attrs[i] == ' ') ++i;
            if (i < attrs.size() && (attrs[i] == '"' || attrs[i] == '\'')) {
                const char quote = attrs[i];
                const size_t end = attrs.find(quote, i + 1);
                if (end != std::string_view::npos) return std::string(attrs.substr(i + 1, end - i - 1));
            }
            return {};
        }
        p += name.size();
    }
    return {};
}

std::string stripTags(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in.compare(i, 9, "<![CDATA[") == 0) {
            const size_t end = in.find("]]>", i + 9);
            const size_t stop = end == std::string_view::npos ? in.size() : end;
            out.append(in.substr(i + 9, stop - i - 9));
            i = end == std::string_view::npos ? in.size() : end + 3;
            continue;
        }
        if (in[i] != '<') { out += in[i++]; continue; }
        if (in.compare(i, 4, "<!--") == 0) {
            const size_t end = in.find("-->", i + 4);
            i = end == std::string_view::npos ? in.size() : end + 3;
            out += ' ';
            continue;
        }
        std::string tag;
        for (size_t k = i + 1; k < in.size() && k < i + 8 && std::isalpha(static_cast<unsigned char>(in[k])); ++k)
            tag += lower(in[k]);
        const size_t close = in.find('>', i);
        if (close == std::string_view::npos) break;
        i = close + 1;
        if (tag == "script" || tag == "style") {
            const std::string closing = "</" + tag;
            size_t end = i;
            while (end < in.size()) {
                end = in.find("</", end);
                if (end == std::string_view::npos) break;
                bool match = true;
                for (size_t k = 0; k < closing.size() && match; ++k)
                    match = end + k < in.size() && lower(in[end + k]) == closing[k];
                if (match) break;
                end += 2;
            }
            const size_t after = end == std::string_view::npos ? std::string_view::npos : in.find('>', end);
            i = after == std::string_view::npos ? in.size() : after + 1;
        }
        out += ' ';
    }
    return out;
}

std::string decodeEntities(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] != '&') { out += in[i++]; continue; }
        const size_t semi = in.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) { out += in[i++]; continue; }
        const std::string_view name = in.substr(i + 1, semi - i - 1);
        std::uint32_t cp = 0;
        bool ok = true;
        if (name == "amp") cp = '&';
        else if (name == "lt") cp = '<';
        else if (name == "gt") cp = '>';
        else if (name == "quot") cp = '"';
        else if (name == "apos") cp = '\'';
        else if (name == "nbsp") cp = ' ';
        else if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string_view digits = name.substr(hex ? 2 : 1);
            ok = !digits.empty();
            for (char c : digits) {
                const int v = c >= '0' && c <= '9' ? c - '0' :
                              hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                              hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (v < 0 || cp > 0x10FFFF) { ok = false; break; }
                cp = cp * (hex ? 16 : 10) + static_cast<std::uint32_t>(v);
            }
            if (ok && (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || cp < 0x20 || cp == 0x7F)) cp = ' ';
        } else ok = false;
        if (!ok) { out += in[i++]; continue; }
        appendUtf8(out, cp);
        i = semi + 1;
    }
    return out;
}

std::string cleanText(std::string_view raw, size_t maxBytes) {
    std::string text = decodeEntities(stripTags(raw));
    if (text.find('<') != std::string::npos) text = decodeEntities(stripTags(text));  // escaped HTML summaries
    std::string out;
    out.reserve(std::min(text.size(), maxBytes));
    bool space = false;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || static_cast<unsigned char>(c) < 0x20) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    if (out.size() > maxBytes) {
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    return out;
}

std::string firstText(std::string_view text, size_t from, size_t limit, std::initializer_list<std::string_view> names,
                      size_t maxBytes) {
    for (auto name : names) {
        const auto e = findElement(text, name, from, limit);
        if (!e.found) continue;
        std::string value = cleanText(text.substr(e.contentStart, e.contentEnd - e.contentStart), maxBytes);
        if (!value.empty()) return value;
    }
    return {};
}
}  // namespace

bool validHostName(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    if (host == "localhost" || endsWith(host, ".localhost") || endsWith(host, ".local") ||
        endsWith(host, ".internal") || endsWith(host, ".arpa") || endsWith(host, ".lan") || endsWith(host, ".home"))
        return false;
    size_t labels = 0, start = 0;
    bool lastAllDigits = false;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        if (dot == std::string_view::npos) dot = host.size();
        const std::string_view label = host.substr(start, dot - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        bool digits = true;
        for (char c : label) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
            digits = digits && c >= '0' && c <= '9';
        }
        lastAllDigits = digits;
        ++labels;
        start = dot + 1;
        if (dot == host.size()) break;
    }
    return labels >= 2 && !lastAllDigits;
}

bool validHostList(const std::vector<std::string>& hosts, std::string& error) {
    if (hosts.empty() || hosts.size() > kMaxHosts) {
        error = "network_hosts must list 1 to 4 hosts";
        return false;
    }
    size_t total = 0;
    for (size_t i = 0; i < hosts.size(); ++i) {
        if (hosts[i].size() > kMaxHostLength || !validHostName(hosts[i])) {
            error = "network_hosts entries must be lowercase public DNS names of at most 63 characters without scheme, port, path or wildcard";
            return false;
        }
        total += hosts[i].size();
        for (size_t j = 0; j < i; ++j)
            if (hosts[i] == hosts[j]) { error = "Duplicate network host"; return false; }
    }
    if (total > kMaxHostsLength) { error = "network_hosts exceed 100 characters in total"; return false; }
    return true;
}

bool parseUrl(std::string_view url, const std::vector<std::string>& hosts, Url& out, std::string& error) {
    out = {};
    if (url.empty() || url.size() > kMaxUrlLength) { error = "network URL is empty or exceeds 2048 characters"; return false; }
    if (url.size() < 8) { error = "network URL must start with https://"; return false; }
    for (size_t i = 0; i < 8; ++i)
        if (lower(url[i]) != "https://"[i]) { error = "network URL must start with https://"; return false; }
    for (char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F || c == '\\' || c == '"' || c == '<' || c == '>') {
            error = "network URL must be printable ASCII without spaces (percent-encode other characters)";
            return false;
        }
        if (c == '#') { error = "network URL must not contain a fragment"; return false; }
    }
    const std::string_view rest = url.substr(8);
    const size_t pathAt = std::min(rest.find('/'), rest.find('?'));
    std::string_view authority = rest.substr(0, pathAt);
    if (authority.find('@') != std::string_view::npos) { error = "network URL must not contain credentials"; return false; }
    if (const size_t colon = authority.find(':'); colon != std::string_view::npos) {
        if (authority.substr(colon + 1) != "443") { error = "network URL must use the default HTTPS port"; return false; }
        authority = authority.substr(0, colon);
    }
    std::string host;
    for (char c : authority) host += lower(c);
    if (!validHostName(host)) { error = "network URL host is not a public DNS name"; return false; }
    if (std::find(hosts.begin(), hosts.end(), host) == hosts.end()) {
        error = "network host '" + host + "' is not declared in plugin.json network_hosts";
        return false;
    }
    std::string path(pathAt == std::string_view::npos ? std::string_view{} : rest.substr(pathAt));
    if (path.empty() || path[0] == '?') path.insert(path.begin(), '/');
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '%') continue;
        if (i + 2 >= path.size() || !std::isxdigit(static_cast<unsigned char>(path[i + 1])) ||
            !std::isxdigit(static_cast<unsigned char>(path[i + 2])) || (path[i + 1] == '0' && path[i + 2] == '0')) {
            error = "network URL has an invalid percent escape";
            return false;
        }
    }
    out.host = std::move(host);
    out.path = std::move(path);
    error.clear();
    return true;
}

bool publicIpv4(const std::uint8_t a[4]) {
    const unsigned b0 = a[0], b1 = a[1], b2 = a[2];
    if (b0 == 0 || b0 == 10 || b0 == 127 || b0 >= 224) return false;           // this-net, private, loopback, multicast/reserved
    if (b0 == 100 && (b1 & 0xC0) == 64) return false;                          // CGNAT 100.64/10
    if (b0 == 169 && b1 == 254) return false;                                  // link-local / cloud metadata
    if (b0 == 172 && (b1 & 0xF0) == 16) return false;                          // 172.16/12
    if (b0 == 192 && b1 == 168) return false;                                  // 192.168/16
    if (b0 == 192 && b1 == 0 && (b2 == 0 || b2 == 2)) return false;            // IETF, TEST-NET-1
    if (b0 == 192 && b1 == 88 && b2 == 99) return false;                       // 6to4 relay
    if (b0 == 198 && (b1 & 0xFE) == 18) return false;                          // benchmarking 198.18/15
    if (b0 == 198 && b1 == 51 && b2 == 100) return false;                      // TEST-NET-2
    if (b0 == 203 && b1 == 0 && b2 == 113) return false;                       // TEST-NET-3
    return true;
}

bool publicIpv6(const std::uint8_t a[16]) {
    if ((a[0] & 0xE0) != 0x20) return false;                                   // only 2000::/3 global unicast
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0d && a[3] == 0xb8) return false;  // documentation
    if (a[0] == 0x20 && a[1] == 0x01 && (a[2] & 0xFE) == 0x00) return false;  // 2001::/23 incl. Teredo
    if (a[0] == 0x20 && a[1] == 0x02) return false;                            // 6to4 can embed private IPv4
    return true;
}

bool textContentType(std::string_view contentType) {
    const std::string type = mediaType(contentType, nullptr);
    if (type.empty()) return false;
    if (type.rfind("text/", 0) == 0 && type.size() > 5) return true;
    return type == "application/json" || type == "application/xml" || endsWith(type, "+json") ||
           endsWith(type, "+xml") || type == "application/rss" || type == "application/atom";
}

bool sanitizeText(std::string& body, std::string_view contentType, std::string& error) {
    if (!textContentType(contentType)) { error = "network response is not a text content type"; return false; }
    if (body.size() > kMaxBodyBytes) { error = "network response exceeds 1 MiB"; return false; }
    std::string charset;
    mediaType(contentType, &charset);
    if (charset == "iso-8859-1" || charset == "latin1" || charset == "windows-1252" || charset == "cp1252") {
        const bool cp1252 = charset == "windows-1252" || charset == "cp1252";
        std::string converted;
        converted.reserve(body.size() + body.size() / 8);
        for (char c : body) {
            const auto u = static_cast<unsigned char>(c);
            std::uint32_t cp = u;
            if (cp1252 && u >= 0x80 && u < 0xA0) cp = kWindows1252[u - 0x80];
            if (u >= 0x80 && u < 0xA0 && (!cp1252 || cp == 0)) { error = "network response contains control characters"; return false; }
            appendUtf8(converted, cp);
        }
        if (converted.size() > kMaxBodyBytes + kMaxBodyBytes / 2) { error = "network response exceeds 1 MiB"; return false; }
        body = std::move(converted);
    } else if (!charset.empty() && charset != "utf-8" && charset != "utf8" && charset != "us-ascii") {
        error = "network response charset '" + charset + "' is not supported (UTF-8 or ISO-8859-1 only)";
        return false;
    }
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF)
        body.erase(0, 3);
    for (size_t i = 0; i < body.size();) {
        const auto u = static_cast<unsigned char>(body[i]);
        if (u < 0x80) {
            if ((u < 0x20 && u != '\t' && u != '\n' && u != '\r') || u == 0x7F) {
                error = "network response contains binary or control characters";
                return false;
            }
            ++i;
            continue;
        }
        const size_t n = utf8Length(body, i);
        if (!n) { error = "network response is not valid UTF-8 text"; return false; }
        i += n;
    }
    error.clear();
    return true;
}

bool parseFeed(std::string_view text, size_t maxItems, std::vector<FeedItem>& out, std::string& error) {
    out.clear();
    maxItems = std::min(maxItems, kMaxFeedItems);
    if (text.size() > kMaxBodyBytes + kMaxBodyBytes / 2) { error = "feed text exceeds 1 MiB"; return false; }
    const bool feedRoot = text.find("<rss") != std::string_view::npos || text.find("<feed") != std::string_view::npos ||
                          text.find("<rdf:RDF") != std::string_view::npos || text.find("<channel") != std::string_view::npos;
    size_t pos = 0;
    while (out.size() < maxItems) {
        const auto item = findElement(text, "item", pos, text.size());
        const auto entry = findElement(text, "entry", pos, text.size());
        const Element* e = nullptr;
        if (item.found && (!entry.found || item.start < entry.start)) e = &item;
        else if (entry.found) e = &entry;
        if (!e) break;
        const size_t from = e->contentStart, limit = e->contentEnd;
        FeedItem result;
        result.title = firstText(text, from, limit, {"title"}, 256);
        for (size_t p = from; p < limit;) {
            const auto link = findElement(text, "link", p, limit);
            if (!link.found) break;
            std::string value = cleanText(text.substr(link.contentStart, link.contentEnd - link.contentStart), 512);
            if (value.empty()) {
                const std::string rel = attribute(link.attrs, "rel");
                if (rel.empty() || rel == "alternate") value = cleanText(attribute(link.attrs, "href"), 512);
            }
            if (!value.empty()) { result.link = std::move(value); break; }
            p = link.end;
        }
        if (result.link.empty()) result.link = firstText(text, from, limit, {"guid", "id"}, 512);
        result.summary = firstText(text, from, limit, {"description", "summary", "content"}, 1024);
        result.published = firstText(text, from, limit, {"pubDate", "published", "updated", "dc:date"}, 64);
        out.push_back(std::move(result));
        pos = e->end;
    }
    if (out.empty() && !feedRoot) { error = "text is not an RSS or Atom feed"; return false; }
    error.clear();
    return true;
}

}  // namespace sf::pluginnet

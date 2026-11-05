#include "Net.h"

#include <cctype>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#endif

namespace sf::net {

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

#ifdef _WIN32
bool get(const std::string& url, std::vector<unsigned char>& out, int* status) {
    HINTERNET net = InternetOpenA("SciFiShell/1.0 (media library)", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!net) return false;
    DWORD to = 8000;
    InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof to);
    InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof to);
    InternetSetOptionA(net, INTERNET_OPTION_SEND_TIMEOUT, &to, sizeof to);
    HINTERNET h = InternetOpenUrlA(net, url.c_str(), "Accept: */*\r\n", static_cast<DWORD>(-1),
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_SECURE * (url.rfind("https", 0) == 0), 0);
    bool ok = false;
    if (h) {
        DWORD code = 0, len = sizeof code;
        HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, nullptr);
        if (status) *status = static_cast<int>(code);
        if (code == 200) {
            ok = true;
            unsigned char buf[16384];
            DWORD n = 0;
            while (InternetReadFile(h, buf, sizeof buf, &n) && n > 0) {
                out.insert(out.end(), buf, buf + n);
                if (out.size() > 16u * 1024 * 1024) { ok = false; break; }
            }
        }
        InternetCloseHandle(h);
    }
    InternetCloseHandle(net);
    return ok;
}
#else
bool get(const std::string&, std::vector<unsigned char>&, int*) { return false; }
#endif

bool getString(const std::string& url, std::string& out, int* status) {
    std::vector<unsigned char> b;
    if (status) *status = 0;
    if (!get(url, b, status)) return false;
    out.assign(b.begin(), b.end());
    return true;
}

}  // namespace sf::net

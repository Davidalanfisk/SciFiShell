#pragma once
// Tiny blocking HTTP GET client (WinINet on Windows). Always call from a worker thread.
#include <string>
#include <vector>

namespace sf::net {

// Returns true on HTTP 200. Body is appended to `out`. Times out after a few seconds.
bool get(const std::string& url, std::vector<unsigned char>& out, int* status = nullptr);
bool getString(const std::string& url, std::string& out, int* status = nullptr);
std::string urlEncode(const std::string& s);

}  // namespace sf::net

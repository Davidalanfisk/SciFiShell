#include "PluginPackage.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <vector>

namespace sf {
namespace {
constexpr size_t kMaxBytes = 16384, kMaxPackages = 64;

struct Value { bool array = false; std::string text; std::vector<std::string> items; };
class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    bool parse(std::map<std::string, Value>& out) {
        if (!take('{')) return false;
        if (take('}')) return finish();
        do {
            std::string key;
            Value value;
            if (!string(key) || !take(':')) return false;
            if (take('[')) {
                value.array = true;
                if (!take(']')) {
                    do { std::string v; if (!string(v)) return false; value.items.push_back(v); } while (take(','));
                    if (!take(']')) return false;
                }
            } else if (!string(value.text)) return false;
            if (!out.emplace(key, value).second) return false;
            if (take('}')) return finish();
        } while (take(','));
        return false;
    }
private:
    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\r' || s_[i_] == '\n' || s_[i_] == '\t')) ++i_; }
    bool take(char c) { ws(); if (i_ < s_.size() && s_[i_] == c) { ++i_; return true; } return false; }
    bool finish() { ws(); return i_ == s_.size(); }
    bool string(std::string& out) {
        if (!take('"')) return false;
        while (i_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"') return true;
            if (c < 0x20 || c > 0x7e) return false;
            if (c == '\\') {
                if (i_ == s_.size()) return false;
                c = static_cast<unsigned char>(s_[i_++]);
                if (c != '"' && c != '\\' && c != '/') return false;
            }
            out += static_cast<char>(c);
        }
        return false;
    }
    const std::string& s_;
    size_t i_ = 0;
};

bool validId(const std::string& id) {
    if (id.size() < 3 || id.size() > 80 || id.find('.') == std::string::npos ||
        id.front() == '.' || id.back() == '.' || id.find("..") != std::string::npos) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    return id.rfind("org.scifishell.", 0) != 0;
}
bool version(const std::string& s) {
    int dots = 0;
    bool digit = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') digit = true;
        else if (c == '.' && digit) { ++dots; digit = false; }
        else return false;
    }
    return digit && dots == 2 && s.size() <= 24;
}
bool colour(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < s.size(); ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return true;
}
}

PluginPackageStore::PluginPackageStore(std::filesystem::path directory, Register reg)
    : dir_(std::move(directory)), register_(std::move(reg)) {}

bool PluginPackageStore::parse(const std::string& text, ThemePackage& out, std::string& error) {
    std::map<std::string, Value> fields;
    if (text.size() > kMaxBytes || !Parser(text).parse(fields)) { error = "Invalid package JSON"; return false; }
    const char* keys[] = {"id", "name", "version", "author", "description", "type", "platforms", "permissions", "accent", "alt"};
    if (fields.size() != sizeof(keys) / sizeof(keys[0])) { error = "Unexpected or missing manifest fields"; return false; }
    for (const char* key : keys) if (!fields.count(key)) { error = "Missing manifest field"; return false; }
    for (const auto& [key, value] : fields)
        if (value.array != (key == "platforms" || key == "permissions")) { error = "Invalid manifest field type"; return false; }
    out = {fields["id"].text, fields["name"].text, fields["version"].text, fields["author"].text,
           fields["description"].text, fields["accent"].text, fields["alt"].text};
    if (!validId(out.id) || !version(out.version) || out.name.empty() || out.name.size() > 40 ||
        out.author.empty() || out.author.size() > 80 || out.description.empty() || out.description.size() > 160 ||
        fields["type"].text != "theme" || fields["platforms"].items != std::vector<std::string>{"any"} ||
        !fields["permissions"].items.empty() || !colour(out.accent) || !colour(out.alt)) {
        error = "Unsupported or invalid theme manifest";
        return false;
    }
    error.clear();
    return true;
}

bool PluginPackageStore::load(const std::filesystem::path& path, ThemePackage& out) {
    std::error_code ec;
    auto st = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(st)) { error_ = "Package must be a regular file"; return false; }
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxBytes) { error_ = "Package is unreadable or too large"; return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { error_ = "Cannot open package"; return false; }
    std::string data(static_cast<size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!in || in.peek() != std::ifstream::traits_type::eof()) { error_ = "Cannot read package"; return false; }
    return parse(data, out, error_);
}

void PluginPackageStore::discover() {
    if (dir_.empty()) { error_ = "Plugin package directory is unavailable"; return; }
    std::error_code ec;
    if (!std::filesystem::exists(dir_, ec)) {
        if (ec) error_ = "Cannot inspect plugin directory: " + ec.message();
        return;
    }
    std::vector<std::filesystem::path> paths;
    std::filesystem::directory_iterator it(dir_, ec), end;
    while (!ec && it != end) {
        if (it->path().extension() == ".sfplugin") paths.push_back(it->path());
        it.increment(ec);
        if (paths.size() > kMaxPackages) { error_ = "Too many installed packages"; return; }
    }
    if (ec) { error_ = "Cannot read plugin directory: " + ec.message(); return; }
    std::sort(paths.begin(), paths.end());
    std::string errors;
    for (const auto& path : paths) {
        ThemePackage pkg;
        if (!load(path, pkg) || path.filename().string() != pkg.id + ".sfplugin" || !register_(pkg)) {
            if (!errors.empty()) errors += "; ";
            errors += "Invalid or duplicate package: " + path.filename().string();
        }
    }
    error_ = errors;
}

bool PluginPackageStore::install(const std::filesystem::path& source) {
    if (dir_.empty()) { error_ = "Plugin package directory is unavailable"; return false; }
    if (source.extension() != ".sfplugin") { error_ = "Select a .sfplugin package"; return false; }
    ThemePackage input;
    if (!load(source, input)) return false;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) { error_ = "Cannot create plugin directory: " + ec.message(); return false; }
    size_t count = 0;
    std::filesystem::directory_iterator it(dir_, ec), end;
    while (!ec && it != end) {
        if (it->path().extension() == ".sfplugin") ++count;
        it.increment(ec);
    }
    if (ec || count >= kMaxPackages) { error_ = "Cannot install: package directory unreadable or full"; return false; }
    const auto target = dir_ / (input.id + ".sfplugin");
    if (!std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, ec)) {
        error_ = "Cannot install package (already installed or inaccessible): " + ec.message();
        return false;
    }
    ThemePackage copied;
    if (!load(target, copied) || copied.id != input.id || !register_(copied)) {
        std::error_code cleanup;
        std::filesystem::remove(target, cleanup);
        error_ = cleanup ? "Package rejected; could not remove copied file: " + cleanup.message() : "Package changed or plugin ID already exists";
        return false;
    }
    error_.clear();
    return true;
}

}  // namespace sf

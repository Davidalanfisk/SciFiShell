#include "PluginPackage.h"
#include "GameplayPackage.h"
#include "GameplayFileLock.h"
#include "GameplayLimits.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <vector>
#include <cstring>

#include "miniz.h"

namespace sf {
namespace {
constexpr size_t kMaxBytes = kPluginTextLimit, kMaxPackages = 64;

struct Value { bool array = false; std::string text; std::vector<std::string> items; };
// Bound parsed metadata separately from the larger source/formatting allowance.
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
                    do {
                        std::string v;
                        if (value.items.size() >= 12 || !string(v)) return false;
                        value.items.push_back(v);
                    } while (take(','));
                    if (!take(']')) return false;
                }
            } else if (!string(value.text)) return false;
            if (out.size() >= 11 || !out.emplace(key, value).second) return false;
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
            if (out.size() >= 160) return false;
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

bool copyBoundedZip(const std::filesystem::path& source, const std::filesystem::path& target,
                    std::uint64_t size, std::string& error) {
    std::ifstream in(source, std::ios::binary);
    if (!in) { error = "Cannot open ZIP for copying"; return false; }
#ifdef _WIN32
    FILE* out = nullptr;
    _wfopen_s(&out, target.c_str(), L"wbx");
#else
    FILE* out = std::fopen(target.c_str(), "wbx");
#endif
    if (!out) { error = "Cannot create installed ZIP (already installed or inaccessible)"; return false; }
    bool ok = true;
    std::array<char, 65536> buffer{};
    std::uint64_t remaining = size;
    while (remaining) {
        const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
        if (!in.read(buffer.data(), count) ||
            std::fwrite(buffer.data(), 1, static_cast<size_t>(count), out) != static_cast<size_t>(count)) {
            ok = false;
            break;
        }
        remaining -= static_cast<std::uint64_t>(count);
    }
    if (ok && (in.peek() != std::ifstream::traits_type::eof() || in.bad())) ok = false;
    if (std::fclose(out) != 0) ok = false;
    if (!ok) {
        std::error_code cleanup;
        std::filesystem::remove(target, cleanup);
        error = cleanup ? "Cannot copy ZIP or remove incomplete file: " + cleanup.message() :
                          "Cannot copy ZIP or source changed while copying";
        return false;
    }
    return true;
}
}

PluginPackageStore::PluginPackageStore(std::filesystem::path directory, Register reg, RegisterScript script,
                                       RegisterGameplay gameplay)
    : dir_(std::move(directory)), register_(std::move(reg)), registerScript_(std::move(script)),
      registerGameplay_(std::move(gameplay)) {}

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

bool PluginPackageStore::parseScript(const std::string& text, ScriptPackage& out, std::string& error) {
    std::map<std::string, Value> fields;
    if (text.size() > kMaxBytes || !Parser(text).parse(fields)) { error = "Invalid script manifest JSON"; return false; }
    const char* keys[] = {"id", "name", "version", "author", "description", "entry", "type", "platforms", "permissions", "min_shell_version"};
    if (fields.size() != sizeof(keys) / sizeof(keys[0])) { error = "Unexpected or missing script manifest fields"; return false; }
    for (const char* key : keys) if (!fields.count(key)) { error = "Missing script manifest field"; return false; }
    for (const auto& [key, value] : fields)
        if (value.array != (key == "platforms" || key == "permissions")) { error = "Invalid manifest field type"; return false; }
    out = {fields["id"].text, fields["name"].text, fields["version"].text, fields["author"].text, fields["description"].text, {}};
    if (!validId(out.id) || !version(out.version) || out.name.empty() || out.name.size() > 40 ||
        out.author.empty() || out.author.size() > 80 || out.description.empty() || out.description.size() > 160 ||
        fields["type"].text != "script" || fields["entry"].text != "scripts/main.lua" ||
        fields["platforms"].items != std::vector<std::string>{"any"} || !fields["permissions"].items.empty() ||
        fields["min_shell_version"].text != "0.1.0") {
        error = "Unsupported or invalid script manifest";
        return false;
    }
    error.clear();
    return true;
}

bool PluginPackageStore::parseGameplayManifest(const std::string& text, GameplayPackage& out, std::string& error) {
    out = {};
    std::map<std::string, Value> fields;
    if (text.size() > kMaxBytes || !Parser(text).parse(fields)) { error = "Invalid gameplay manifest JSON"; return false; }
    const char* keys[] = {"id", "name", "version", "author", "description", "entry", "type",
                          "platforms", "permissions", "api_version"};
    const bool downtown = fields.count("downtown_model") != 0;
    if (fields.size() != sizeof(keys) / sizeof(keys[0]) + (downtown ? 1 : 0)) { error = "Unexpected or missing gameplay manifest fields"; return false; }
    for (const char* key : keys) if (!fields.count(key)) { error = "Missing gameplay manifest field"; return false; }
    for (const auto& [key, value] : fields)
        if (value.array != (key == "platforms" || key == "permissions")) { error = "Invalid gameplay manifest field type"; return false; }
    const auto& permissions = fields["permissions"].items;
    bool scene = false, world = false, layout = false, impulse = false, control = false,
         input = false, controller = false, view = false, sound = false, hud = false, storage = false,
         messaging = false, invalidPermission = false;
    for (const auto& permission : permissions) {
        bool* flag = permission == "scene" ? &scene : permission == "world" ? &world :
                     permission == "input" ? &input : permission == "view" ? &view :
                     permission == "sound" ? &sound : permission == "hud" ? &hud :
                     permission == "layout" ? &layout : permission == "impulse" ? &impulse :
                     permission == "control" ? &control : permission == "storage" ? &storage :
                     permission == "controller" ? &controller : permission == "messaging" ? &messaging : nullptr;
        if (!flag || *flag) invalidPermission = true;
        else *flag = true;
    }
    if (!validId(fields["id"].text) || !version(fields["version"].text) ||
        fields["name"].text.empty() || fields["name"].text.size() > 40 ||
        fields["author"].text.empty() || fields["author"].text.size() > 80 ||
        fields["description"].text.empty() || fields["description"].text.size() > 160 ||
        fields["type"].text != "gameplay" || fields["entry"].text != "scripts/main.lua" ||
        fields["platforms"].items != std::vector<std::string>{"any"} || fields["api_version"].text != "1" ||
        !scene || ((layout || impulse) && !world) || (control && (!world || !input || !view)) ||
        (controller && !input) || invalidPermission) {
        error = "Unsupported or invalid gameplay manifest";
        return false;
    }
    out.id = fields["id"].text;
    out.name = fields["name"].text;
    out.version = fields["version"].text;
    out.author = fields["author"].text;
    out.description = fields["description"].text;
    out.scene = true;
    out.storage = storage;
    out.messaging = messaging;
    out.controller = controller;
    out.world = world;
    out.layout = layout;
    out.impulse = impulse;
    out.control = control;
    out.input = input;
    out.view = view;
    out.sound = sound;
    out.hud = hud;
    if (downtown) {
        const auto& model = fields["downtown_model"].text;
        if (model.size() < 12 || model.size() > 120 || model.rfind("assets/", 0) != 0 ||
            model.substr(model.size() - 4) != ".glb" || model.find("..") != std::string::npos ||
            model.find('\\') != std::string::npos) {
            error = "downtown_model must name a bounded assets/*.glb package asset"; return false;
        }
        out.downtownModel = model;
    }
    error.clear();
    return true;
}

bool PluginPackageStore::readZip(const std::string& data, ScriptPackage& out, std::string& error) {
    if (data.empty() || data.size() > kScriptArchiveLimit) { error = "Script ZIP is empty or exceeds 20 MiB + 64 KiB"; return false; }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) { error = "Invalid ZIP"; return false; }
    auto done = [&](const std::string& message) {
        mz_zip_reader_end(&zip);
        error = message;
        return message.empty();
    };
    const auto count = mz_zip_reader_get_num_files(&zip);
    if (count < 2 || count > 3) return done("ZIP must contain only plugin.json and scripts/main.lua");
    std::string manifest, script;
    bool seenManifest = false, seenScript = false, seenDirectory = false;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) return done("Invalid ZIP entry");
        char name[256]{};
        const auto length = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
        if (!length || length > sizeof(name) ||
            mz_zip_reader_get_filename(&zip, i, name, sizeof(name)) != length || std::strlen(name) + 1 != length)
            return done("Invalid ZIP entry name");
        const std::string path(name);
        const auto type = (stat.m_external_attr >> 16) & 0170000;
        const bool directory = path == "scripts/" && stat.m_is_directory;
        if ((type && type != (directory ? 0040000 : 0100000)) || stat.m_is_encrypted || !stat.m_is_supported ||
            stat.m_uncomp_size > kMaxBytes || (stat.m_is_directory && !directory))
            return done("Unsupported ZIP entry type or size");
        if (directory) {
            if (seenDirectory || stat.m_uncomp_size) return done("Duplicate or invalid directory");
            seenDirectory = true;
            continue;
        }
        std::string* target = nullptr;
        if (path == "plugin.json" && !seenManifest) { seenManifest = true; target = &manifest; }
        else if (path == "scripts/main.lua" && !seenScript) { seenScript = true; target = &script; }
        else return done("Unexpected or duplicate ZIP path");
        target->resize(static_cast<size_t>(stat.m_uncomp_size));
        if (!mz_zip_reader_extract_to_mem(&zip, i, target->data(), target->size(), 0))
            return done("Cannot extract ZIP entry or CRC mismatch");
    }
    if (!seenManifest || !seenScript || script.empty()) return done("ZIP is missing required files");
    if (!parseScript(manifest, out, error)) { mz_zip_reader_end(&zip); return false; }
    out.script = std::move(script);
    return done({});
}

bool PluginPackageStore::loadZip(const std::filesystem::path& path, ScriptPackage& out) {
    std::error_code ec;
    const auto st = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(st)) { error_ = "ZIP must be a regular file"; return false; }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kScriptArchiveLimit) { error_ = "Script ZIP is unreadable or exceeds 20 MiB + 64 KiB"; return false; }
    std::ifstream in(path, std::ios::binary);
    std::string data(static_cast<size_t>(size), '\0');
    if (!in || !in.read(data.data(), static_cast<std::streamsize>(data.size())) || in.peek() != std::ifstream::traits_type::eof()) {
        error_ = "Cannot read ZIP"; return false;
    }
    return readZip(data, out, error_);
}

bool PluginPackageStore::loadGameplayZip(const std::filesystem::path& path, GameplayPackage& out, GameplayDigest& digest) {
    GameplayFileLock lock;
    if (!lock.open(path, error_)) return false;
    GameplayDigest after;
    if (!gameplayDigestFile(path, digest, error_) || !readGameplayPackageFile(path, out, error_) ||
        !gameplayDigestFile(path, after, error_)) return false;
    if (!(digest == after)) { error_ = "Gameplay ZIP changed while validating"; return false; }
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
        if (it->path().extension() == ".sfplugin" || it->path().extension() == ".zip") paths.push_back(it->path());
        it.increment(ec);
        if (paths.size() > kMaxPackages) { error_ = "Too many installed packages"; return; }
    }
    if (ec) { error_ = "Cannot read plugin directory: " + ec.message(); return; }
    std::sort(paths.begin(), paths.end());
    std::string errors;
    for (const auto& path : paths) {
        bool ok = false;
        if (path.extension() == ".zip") {
            ScriptPackage pkg;
            ok = loadZip(path, pkg) && path.filename().string() == pkg.id + ".zip" && registerScript_ && registerScript_(pkg);
            if (!ok && registerGameplay_) {
                GameplayPackage game;
                GameplayDigest digest;
                ok = loadGameplayZip(path, game, digest) && path.filename().string() == game.id + ".zip" &&
                     registerGameplay_(game, path, digest);
            }
        } else {
            ThemePackage pkg;
            ok = load(path, pkg) && path.filename().string() == pkg.id + ".sfplugin" && register_(pkg);
        }
        if (!ok) {
            if (!errors.empty()) errors += "; ";
            errors += "Invalid or duplicate package: " + path.filename().string();
        }
    }
    error_ = errors;
}

bool PluginPackageStore::install(const std::filesystem::path& source) {
    if (dir_.empty()) { error_ = "Plugin package directory is unavailable"; return false; }
    const bool zip = source.extension() == ".zip";
    if (!zip && source.extension() != ".sfplugin") { error_ = "Select a .zip or .sfplugin package"; return false; }
    ThemePackage input;
    ScriptPackage script;
    GameplayPackage gameplay;
    GameplayDigest gameplayDigest;
    bool isGameplay = false;
    if (zip) {
        if (!loadZip(source, script)) {
            if (!registerGameplay_ || !loadGameplayZip(source, gameplay, gameplayDigest)) return false;
            isGameplay = true;
        }
    } else if (!load(source, input)) return false;
    const std::string id = isGameplay ? gameplay.id : zip ? script.id : input.id;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) { error_ = "Cannot create plugin directory: " + ec.message(); return false; }
    size_t count = 0;
    std::uint64_t zipBytes = 0;
    std::filesystem::directory_iterator it(dir_, ec), end;
    while (!ec && it != end) {
        if (it->path().extension() == ".sfplugin" || it->path().extension() == ".zip") ++count;
        if (zip && it->path().extension() == ".zip") {
            const auto existingSize = it->file_size(ec);
            if (ec || existingSize > kGameplayInstalledZipLimit - zipBytes) {
                error_ = "Cannot inspect installed ZIP storage"; return false;
            }
            zipBytes += existingSize;
        }
        it.increment(ec);
    }
    if (ec || count >= kMaxPackages) { error_ = "Cannot install: package directory unreadable or full"; return false; }
    const auto incomingZipSize = isGameplay ? gameplayDigest.size :
        zip ? std::filesystem::file_size(source, ec) : 0;
    if (ec) { error_ = "Cannot inspect source ZIP: " + ec.message(); return false; }
    if (zip && !isGameplay && incomingZipSize > kScriptArchiveLimit) {
        error_ = "Script ZIP changed or exceeds 20 MiB + 64 KiB"; return false;
    }
    if (zip && incomingZipSize > kGameplayInstalledZipLimit - zipBytes) {
        error_ = "Installed ZIP storage limit exceeded"; return false;
    }
    const auto target = dir_ / (id + (zip ? ".zip" : ".sfplugin"));
    if (zip && !copyBoundedZip(source, target, incomingZipSize, error_)) return false;
    if (!zip && !std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, ec)) {
        error_ = "Cannot install package (already installed or inaccessible): " + ec.message();
        return false;
    }
    ThemePackage copied;
    ScriptPackage copiedScript;
    GameplayPackage copiedGameplay;
    GameplayDigest copiedDigest;
    const bool registered = isGameplay
        ? loadGameplayZip(target, copiedGameplay, copiedDigest) && copiedGameplay.id == id &&
          copiedDigest == gameplayDigest && registerGameplay_ && registerGameplay_(copiedGameplay, target, copiedDigest)
        : zip
        ? loadZip(target, copiedScript) && copiedScript.id == id && registerScript_ && registerScript_(copiedScript)
        : load(target, copied) && copied.id == id && register_(copied);
    if (!registered) {
        std::error_code cleanup;
        std::filesystem::remove(target, cleanup);
        error_ = cleanup ? "Package rejected; could not remove copied file: " + cleanup.message() : "Package changed or plugin ID already exists";
        return false;
    }
    error_.clear();
    return true;
}

bool PluginPackageStore::uninstall(const std::string& id) {
    if (!validId(id)) { error_ = "Invalid package ID"; return false; }
    std::error_code ec;
    const auto theme = dir_ / (id + ".sfplugin"), script = dir_ / (id + ".zip");
    const bool haveTheme = std::filesystem::exists(theme, ec);
    if (ec) { error_ = "Cannot inspect package: " + ec.message(); return false; }
    const bool haveScript = std::filesystem::exists(script, ec);
    if (ec || haveTheme == haveScript) { error_ = "Package not found or ambiguous"; return false; }
    if (!std::filesystem::remove(haveTheme ? theme : script, ec) || ec) {
        error_ = "Cannot remove package: " + ec.message(); return false;
    }
    error_.clear();
    return true;
}

}  // namespace sf

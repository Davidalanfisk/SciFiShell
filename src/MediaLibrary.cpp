#include "MediaLibrary.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace sf {

namespace {
fs::path dataDir() {
    const char* base = std::getenv("LOCALAPPDATA");
    fs::path p = base ? fs::path(base) / "SciFiShell" / "media" : fs::path("media");
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path fromU8(const std::string& s) { return fs::u8path(s); }

MediaTrack makeTrack(const std::string& path, bool video) {
    MediaTrack t;
    t.path = path;
    t.video = video;
    const fs::path p = fromU8(path);
    t.title = u8(p.stem());
    t.folder = u8(p.parent_path().filename());
    return t;
}
}  // namespace

MediaLibrary::MediaLibrary() {
    load();
    if (tracks_.empty() && !folders_.empty()) rescan();
}

MediaLibrary::~MediaLibrary() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

bool MediaLibrary::isMediaFile(const std::string& path, bool* video) {
    static const char* audio[] = {".mp3", ".flac", ".wav", ".ogg", ".m4a", ".aac", ".wma", ".opus", ".aiff"};
    static const char* vid[] = {".mp4", ".mkv", ".avi", ".mov", ".webm", ".wmv", ".m4v", ".mpg", ".mpeg"};
    const std::string ext = lower(u8(fromU8(path).extension()));
    for (const char* e : audio)
        if (ext == e) { if (video) *video = false; return true; }
    for (const char* e : vid)
        if (ext == e) { if (video) *video = true; return true; }
    return false;
}

std::vector<std::string> MediaLibrary::folders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return folders_;
}

bool MediaLibrary::hasFolder(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(folders_.begin(), folders_.end(), path) != folders_.end();
}

void MediaLibrary::addFolder(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(folders_.begin(), folders_.end(), path) != folders_.end()) return;
        folders_.push_back(path);
    }
    saveFolders();
    rescan();
}

void MediaLibrary::removeFolder(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        folders_.erase(std::remove(folders_.begin(), folders_.end(), path), folders_.end());
    }
    saveFolders();
    rescan();
}

void MediaLibrary::clearFolders() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        folders_.clear();
    }
    saveFolders();
    rescan();
}

std::vector<MediaTrack> MediaLibrary::tracks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tracks_;
}

size_t MediaLibrary::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tracks_.size();
}

void MediaLibrary::rescan() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
    cancel_ = false;
    std::vector<std::string> roots = folders();
    scanning_ = true;
    scannedCount_ = 0;
    thread_ = std::thread([this, roots] { scanThread(roots); });
}

void MediaLibrary::scanThread(std::vector<std::string> roots) {
    std::vector<MediaTrack> found;
    for (const std::string& root : roots) {
        std::error_code ec;
        fs::recursive_directory_iterator it(fromU8(root), fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) {
            if (cancel_) { scanning_ = false; return; }
            bool video = false;
            std::error_code e2;
            if (it->is_regular_file(e2)) {
                const std::string path = u8(it->path());
                if (isMediaFile(path, &video)) {
                    found.push_back(makeTrack(path, video));
                    scannedCount_ = static_cast<int>(found.size());
                }
            }
            it.increment(ec);
        }
    }
    std::sort(found.begin(), found.end(), [](const MediaTrack& a, const MediaTrack& b) { return lower(a.path) < lower(b.path); });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks_ = std::move(found);
    }
    saveTracks();
    scanning_ = false;
}

void MediaLibrary::load() {
    {
        std::ifstream f(dataDir() / "folders.txt", std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) folders_.push_back(line);
        }
    }
    std::ifstream f(dataDir() / "tracks.db", std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.size() < 3) continue;
        tracks_.push_back(makeTrack(line.substr(2), line[0] == 'V'));
    }
}

void MediaLibrary::saveFolders() const {
    std::ofstream f(dataDir() / "folders.txt", std::ios::binary | std::ios::trunc);
    for (const std::string& s : folders()) f << s << "\n";
}

void MediaLibrary::saveTracks() const {
    std::ofstream f(dataDir() / "tracks.db", std::ios::binary | std::ios::trunc);
    for (const MediaTrack& t : tracks()) f << (t.video ? "V|" : "A|") << t.path << "\n";
}

void MediaLibrary::savePlaylist(const std::vector<std::string>& paths) const {
    std::ofstream f(dataDir() / "playlist.m3u8", std::ios::binary | std::ios::trunc);
    f << "#EXTM3U\n";
    for (const std::string& s : paths) f << s << "\n";
}

std::vector<std::string> MediaLibrary::loadPlaylist() const {
    std::vector<std::string> out;
    std::ifstream f(dataDir() / "playlist.m3u8", std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty() && line[0] != '#') out.push_back(line);
    }
    return out;
}

}  // namespace sf

#pragma once
// The media library: a list of user chosen folders, a background scanner that finds audio and video files in them, and a
// small on-disk database (%LOCALAPPDATA%\SciFiShell\media\) so the library is available instantly on the next start.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sf {

struct MediaTrack {
    std::string path;   // UTF-8
    std::string title;  // file name without extension
    std::string folder; // containing folder name
    bool video = false;
};

class MediaLibrary {
public:
    MediaLibrary();
    ~MediaLibrary();

    // Folders
    std::vector<std::string> folders() const;
    bool hasFolder(const std::string& path) const;
    void addFolder(const std::string& path);
    void removeFolder(const std::string& path);
    void clearFolders();

    // Scanning (background thread). Results replace the track list when the scan finishes.
    void rescan();
    bool scanning() const { return scanning_.load(); }
    int scanned() const { return scannedCount_.load(); }

    // Snapshot of the tracks (copy: the list can be swapped by the scanner at any time).
    std::vector<MediaTrack> tracks() const;
    size_t count() const;

    static bool isMediaFile(const std::string& path, bool* video = nullptr);

    // Playlists (.m3u8 files in the media folder)
    void savePlaylist(const std::vector<std::string>& paths) const;
    std::vector<std::string> loadPlaylist() const;

private:
    void load();
    void saveFolders() const;
    void saveTracks() const;
    void scanThread(std::vector<std::string> roots);

    mutable std::mutex mutex_;
    std::vector<std::string> folders_;
    std::vector<MediaTrack> tracks_;
    std::thread thread_;
    std::atomic<bool> scanning_{false}, cancel_{false};
    std::atomic<int> scannedCount_{0};
};

}  // namespace sf

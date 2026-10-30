#pragma once
// Background metadata for the media window: tags (artist, album, track number ...), album cover art and video preview
// frames. One worker thread; results are polled from the main thread. Windows only (shell property system); elsewhere the
// worker finds nothing and the window falls back to file names and generated tiles.
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Utils.h"

namespace sf {

struct TrackMeta {
    std::string title, artist, album, albumArtist;
    int track = 0, year = 0;
    double seconds = 0;
};

struct MetaResult {
    enum Kind { Tags, Art, Thumb } kind = Tags;
    std::string key;  // the path for tags, the caller's key for art / thumbnails
    TrackMeta meta;
    Bitmap bmp;
    bool ok = false;
};

class MediaMeta {
public:
    MediaMeta();
    ~MediaMeta();

    // urgent: something on screen is waiting for it, so it jumps the queue (also ahead of pictures).
    void requestTags(const std::string& path, bool urgent = false);
    // Cover art for the album that `path` (one of its tracks) belongs to: folder.jpg style files first, then embedded art.
    void requestArt(const std::string& key, const std::string& path);
    // A frame about 20% into a video, cover-cropped to 384 x 216.
    void requestThumb(const std::string& key, const std::string& path);
    bool poll(MetaResult& out);
    size_t pending() const;

private:
    struct Job {
        int kind;
        std::string key, path;
    };
    void run();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> tagJobs_, urgentJobs_;
    std::vector<Job> picJobs_;  // newest first: what is on screen now matters most
    std::deque<MetaResult> results_;
    bool quit_ = false, busy_ = false;
    std::thread thread_;
};

// Scales `src` to cover w x h (centre crop) with area averaging.
Bitmap scaleCover(const Bitmap& src, int w, int h);

}  // namespace sf

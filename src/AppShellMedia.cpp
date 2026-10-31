// Media player: a Winamp style background player driven from the settings hub pages (NOW PLAYING, FOLDERS, TRACKS, QUEUE).
// Audio plays through a MediaPlayer owned by the shell so it keeps going with every panel closed; video opens in the viewer.
#include <algorithm>
#include <cmath>
#include <atomic>
#include <filesystem>
#include <random>
#include <thread>

#include "AppShell.h"

namespace sf {

namespace {
constexpr int kTracksPerPage = 8;
constexpr int kQueuePerPage = 7;

std::string baseTitle(const std::string& path) {
    const auto s = std::filesystem::u8path(path).stem().u8string();
    return std::string(s.begin(), s.end());
}

std::string clock(double s) {
    const int t = static_cast<int>(std::max(0.0, s));
    char buf[24];
    std::snprintf(buf, sizeof buf, "%d:%02d", t / 60, t % 60);
    return buf;
}
}  // namespace

// A radio stream being opened on a worker thread (opening a URL can take seconds); the shell adopts the player once done.
struct StreamOpen {
    std::atomic<bool> done{false};
    bool ok = false;
    MediaPlayer mp;
};

void AppShell::mediaPlayAt(int index, int dir) {
    stream_.reset();
    nowLive_ = false;    for (int tries = 0; tries < 8; ++tries) {
        if (index < 0 || index >= static_cast<int>(queue_.size())) break;
        const std::string path = queue_[static_cast<size_t>(index)];
        queueIndex_ = index;
        nowTitle_ = baseTitle(path);
        bool video = false;
        MediaLibrary::isMediaFile(path, &video);
        nowVideo_ = video;
        mediaPlayer_.close();
        if (video && !viaWindow_) {
            OpenRequest req;
            req.path = path;
            req.name = nowTitle_;
            req.kind = FileKind::Video;
            const Vec3 f = vrOn_ ? head_.forward : Vec3{-std::sin(camera_.yaw), 0.0f, -std::cos(camera_.yaw)};
            const Vec3 e = vrOn_ ? head_.eye : camera_.position;
            req.cardPos = e + normalize(Vec3{f.x, 0.0f, f.z}) * 2.2f;
            req.cardYaw = std::atan2(-f.x, -f.z);
            viewer_->open(req, head_);
            return;
        }
        if (mediaPlayer_.open(path)) {
            mediaPlayer_.setVolume(mediaVol_);
            mediaPlayer_.play();
            return;
        }
        index += dir;  // unreadable file: try the neighbour
    }
    nowTitle_.clear();
    queueIndex_ = -1;
}

void AppShell::mediaNext(int dir) {
    const int n = static_cast<int>(queue_.size());
    if (n == 0) return;
    int next;
    if (shuffle_ && n > 1) {
        static std::mt19937 rng{std::random_device{}()};
        do next = static_cast<int>(rng() % static_cast<unsigned>(n)); while (next == queueIndex_);
    } else {
        next = queueIndex_ + dir;
        if (next >= n || next < 0) {
            if (repeat_ == 0 && dir > 0 && queueIndex_ >= 0) { mediaPlayer_.close(); queueIndex_ = -1; nowTitle_.clear(); return; }
            next = (next + n) % n;
        }
    }
    mediaPlayAt(next, dir);
}

void AppShell::mediaPlayList(const std::vector<std::string>& paths, int start) {
    if (paths.empty()) return;
    queue_ = paths;
    viaWindow_ = true;
    mediaPlayAt(std::clamp(start, 0, static_cast<int>(paths.size()) - 1), 1);
}

void AppShell::mediaOpenFile(const std::string& path) {
    namespace fs = std::filesystem;
    std::vector<std::string> list;
    int start = 0;
    std::error_code ec;
    const fs::path file = fs::u8path(path);
    for (const auto& e : fs::directory_iterator(file.parent_path(), ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string p = e.path().u8string();
        if (p != path && !MediaLibrary::isMediaFile(p)) continue;
        if (p == path) start = static_cast<int>(list.size());
        list.push_back(p);
    }
    if (list.empty()) list.push_back(path);
    if (media_ && !media_->isOpen()) openPanel(media_.get());
    mediaPlayList(list, start);
    if (media_) {
        bool video = false;
        MediaLibrary::isMediaFile(path, &video);
        media_->showPlaying(video);
    }
}

void AppShell::mediaPlayStream(const std::string& name, const std::string& url) {
    mediaPlayer_.close();
    queue_.clear();
    queueIndex_ = -1;
    nowTitle_ = name;
    nowVideo_ = false;
    nowLive_ = true;
    auto st = std::make_shared<StreamOpen>();
    stream_ = st;
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    std::thread([st, url] {
        st->ok = st->mp.open(url);
        st->done = true;
    }).detach();
}

NowInfo AppShell::mediaNow() {
    NowInfo n;
    n.vol = mediaVol_;
    n.shuffle = shuffle_;
    n.repeat = repeat_;
    n.title = nowTitle_;
    if (stream_) {
        n.active = true;
        n.live = true;
        n.loading = true;
        return n;
    }
    if (nowTitle_.empty() || !mediaPlayer_.isOpen()) {
        if (nowVideo_ && !viaWindow_ && queueIndex_ >= 0 && queueIndex_ < static_cast<int>(queue_.size())) {  // playing in the viewer
            n.active = true;
            n.video = true;
            n.path = queue_[static_cast<size_t>(queueIndex_)];
        }
        return n;
    }
    n.active = true;
    n.live = nowLive_;
    n.video = nowVideo_;
    n.playing = mediaPlayer_.playing();
    n.pos = mediaPlayer_.position();
    n.dur = mediaPlayer_.duration();
    if (!nowLive_ && queueIndex_ >= 0 && queueIndex_ < static_cast<int>(queue_.size())) n.path = queue_[static_cast<size_t>(queueIndex_)];
    return n;
}

void AppShell::mediaStep() {
    if (stream_ && stream_->done.load()) {
        std::shared_ptr<StreamOpen> st = std::move(stream_);
        stream_.reset();
        if (st->ok) {
            mediaPlayer_ = std::move(st->mp);
            mediaPlayer_.setVolume(mediaVol_);
            mediaPlayer_.play();
        } else {
            nowTitle_.clear();
            nowLive_ = false;
        }
        return;
    }
    if (!mediaLib_ || !mediaPlayer_.isOpen() || nowLive_ || (nowVideo_ && !viaWindow_) || !mediaPlayer_.ended()) return;
    if (repeat_ == 2) { mediaPlayer_.seek(0); mediaPlayer_.play(); }
    else mediaNext(1);
}

void AppShell::mediaQueueSet(const std::vector<std::string>& paths, int start) {
    queue_ = paths;
    viaWindow_ = false;
    mediaPlayAt(start, 1);
    pendingMediaPage_ = "MEDIA TRACKS";
}

std::vector<SettingsCategory> AppShell::mediaCategories() {
    if (!mediaLib_) mediaLib_ = std::make_unique<MediaLibrary>();
    auto onOff = [](bool b) { return std::string(b ? "ON" : "OFF"); };
    auto refresh = [this](const char* page) { pendingMediaPage_ = page; };
    std::vector<SettingsCategory> out;

    SettingsCategory np{"MEDIA PLAYER", "Now playing, transport and volume", "MEDIA", {}};
    np.items.push_back({"NOW PLAYING", "Plays in the background", [this] { return nowTitle_.empty() ? std::string("NOTHING") : nowTitle_; }, nullptr});
    np.items.push_back({"PLAY / PAUSE", "Space also pauses video",
                        [this] { return std::string(mediaPlayer_.playing() ? "PLAYING" : "PAUSED"); },
                        [this] {
                            if (mediaPlayer_.isOpen()) { if (mediaPlayer_.playing()) mediaPlayer_.pause(); else mediaPlayer_.play(); }
                            else if (!queue_.empty()) mediaPlayAt(std::max(queueIndex_, 0), 1);
                        }});
    np.items.push_back({"PREVIOUS", "", nullptr, [this] { mediaNext(-1); }});
    np.items.push_back({"NEXT", "", nullptr, [this] { mediaNext(1); }});
    np.items.push_back({"STOP", "", nullptr, [this] { mediaPlayer_.close(); nowTitle_.clear(); queueIndex_ = -1; }});
    np.items.push_back({"POSITION", "Click to skip forward 15s",
                        [this] { return mediaPlayer_.isOpen() ? clock(mediaPlayer_.position()) + " / " + clock(mediaPlayer_.duration()) : std::string("--"); },
                        [this] { if (mediaPlayer_.isOpen()) mediaPlayer_.seek(mediaPlayer_.position() + 15.0); }});
    np.items.push_back({"BACK 15s", "", nullptr, [this] { if (mediaPlayer_.isOpen()) mediaPlayer_.seek(std::max(0.0, mediaPlayer_.position() - 15.0)); }});
    np.items.push_back({"VOLUME", "Click to step up, wraps round",
                        [this] { return std::to_string(static_cast<int>(mediaVol_ * 100.0f + 0.5f)) + "%"; },
                        [this] {
                            mediaVol_ = mediaVol_ >= 0.99f ? 0.0f : std::min(1.0f, mediaVol_ + 0.2f);
                            mediaPlayer_.setVolume(mediaVol_);
                        }});
    np.items.push_back({"SHUFFLE", "Random track order", [this, onOff] { return onOff(shuffle_); }, [this] { shuffle_ = !shuffle_; }});
    np.items.push_back({"REPEAT", "Off, whole queue or single track",
                        [this] { return std::string(repeat_ == 0 ? "OFF" : repeat_ == 1 ? "ALL" : "ONE"); },
                        [this] { repeat_ = (repeat_ + 1) % 3; }});
    np.items.push_back({"QUEUE", "Tracks waiting to play", [this] { return std::to_string(queue_.size()); }, nullptr});
    np.items.push_back({"LIBRARY", "Tracks in the database", [this] { return std::to_string(mediaLib_->count()); }, nullptr});
    out.push_back(std::move(np));

    SettingsCategory fo{"MEDIA FOLDERS", "Where your music and video are kept", "MEDIA", {}};
    const std::string home = homeDirectory();
    for (const char* name : {"Music", "Videos", "Downloads", "Desktop", "Documents"}) {
        const std::string p = home + "\\" + name;
        fo.items.push_back({std::string(name), "Toggle this folder in the library",
                            [this, p, onOff] { return onOff(mediaLib_->hasFolder(p)); },
                            [this, p] { if (mediaLib_->hasFolder(p)) mediaLib_->removeFolder(p); else mediaLib_->addFolder(p); }});
    }
    fo.items.push_back({"ADD CURRENT FOLDER", "The folder you are standing in", nullptr,
                        [this] { if (room_ && !room_->currentPath().empty()) mediaLib_->addFolder(room_->currentPath()); }});
    fo.items.push_back({"REMOVE CURRENT FOLDER", "The folder you are standing in", nullptr,
                        [this] { if (room_) mediaLib_->removeFolder(room_->currentPath()); }});
    fo.items.push_back({"CLEAR ALL FOLDERS", "Empties the library", nullptr, [this] { mediaLib_->clearFolders(); }});
    fo.items.push_back({"RESCAN", "Look for new files",
                        [this] { return mediaLib_->scanning() ? "SCANNING " + std::to_string(mediaLib_->scanned()) : std::string("IDLE"); },
                        [this] { mediaLib_->rescan(); }});
    fo.items.push_back({"FOLDERS", "Watched folders", [this] { return std::to_string(mediaLib_->folders().size()); }, nullptr});
    fo.items.push_back({"TRACKS FOUND", "", [this] { return std::to_string(mediaLib_->count()); }, nullptr});
    out.push_back(std::move(fo));

    // Track browser: a page of the library filtered by type.
    SettingsCategory tr{"MEDIA TRACKS", "Click a track to play it and queue the list", "MEDIA", {}};
    std::vector<MediaTrack> all = mediaLib_->tracks();
    std::vector<MediaTrack> list;
    for (const MediaTrack& t : all)
        if (trackFilter_ == 0 || (trackFilter_ == 1 && !t.video) || (trackFilter_ == 2 && t.video)) list.push_back(t);
    const int pages = std::max(1, (static_cast<int>(list.size()) + kTracksPerPage - 1) / kTracksPerPage);
    trackPage_ = std::clamp(trackPage_, 0, pages - 1);
    std::vector<std::string> paths;
    for (const MediaTrack& t : list) paths.push_back(t.path);
    tr.items.push_back({"SHOW", "Filter by type",
                        [this] { return std::string(trackFilter_ == 0 ? "ALL" : trackFilter_ == 1 ? "AUDIO" : "VIDEO"); },
                        [this, refresh] { trackFilter_ = (trackFilter_ + 1) % 3; trackPage_ = 0; refresh("MEDIA TRACKS"); }});
    tr.items.push_back({"PLAY ALL", "Queue everything shown",
                        [paths] { return std::to_string(paths.size()); },
                        [this, paths] { if (!paths.empty()) mediaQueueSet(paths, 0); }});
    tr.items.push_back({"< PREVIOUS PAGE", "", [this, pages] { return std::to_string(trackPage_ + 1) + "/" + std::to_string(pages); },
                        [this, refresh, pages] { trackPage_ = (trackPage_ + pages - 1) % pages; refresh("MEDIA TRACKS"); }});
    tr.items.push_back({"NEXT PAGE >", "", nullptr, [this, refresh, pages] { trackPage_ = (trackPage_ + 1) % pages; refresh("MEDIA TRACKS"); }});
    for (int i = trackPage_ * kTracksPerPage; i < std::min(static_cast<int>(list.size()), (trackPage_ + 1) * kTracksPerPage); ++i) {
        const MediaTrack& t = list[static_cast<size_t>(i)];
        tr.items.push_back({t.title, t.folder, [v = t.video] { return std::string(v ? "VIDEO" : ""); },
                            [this, paths, i] { mediaQueueSet(paths, i); }});
    }
    out.push_back(std::move(tr));

    SettingsCategory qu{"MEDIA QUEUE", "Playlist: reorder by queueing, save and load", "MEDIA", {}};
    const int qpages = std::max(1, (static_cast<int>(queue_.size()) + kQueuePerPage - 1) / kQueuePerPage);
    queuePage_ = std::clamp(queuePage_, 0, qpages - 1);
    qu.items.push_back({"CLEAR QUEUE", "", nullptr, [this, refresh] { queue_.clear(); queueIndex_ = -1; refresh("MEDIA QUEUE"); }});
    qu.items.push_back({"SAVE PLAYLIST", "To the media database folder", nullptr, [this] { mediaLib_->savePlaylist(queue_); }});
    qu.items.push_back({"LOAD PLAYLIST", "Replaces the queue", nullptr,
                        [this, refresh] { queue_ = mediaLib_->loadPlaylist(); queueIndex_ = -1; refresh("MEDIA QUEUE"); }});
    qu.items.push_back({"PAGE", "Next page of the queue", [this, qpages] { return std::to_string(queuePage_ + 1) + "/" + std::to_string(qpages); },
                        [this, refresh, qpages] { queuePage_ = (queuePage_ + 1) % qpages; refresh("MEDIA QUEUE"); }});
    for (int i = queuePage_ * kQueuePerPage; i < std::min(static_cast<int>(queue_.size()), (queuePage_ + 1) * kQueuePerPage); ++i) {
        const std::string p = queue_[static_cast<size_t>(i)];
        qu.items.push_back({baseTitle(p), "Click to play",
                            [this, i] { return std::string(i == queueIndex_ ? "PLAYING" : ""); },
                            [this, i, refresh] { mediaPlayAt(i, 1); refresh("MEDIA QUEUE"); }});
    }
    out.push_back(std::move(qu));
    return out;
}

}  // namespace sf

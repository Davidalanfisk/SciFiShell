#pragma once
// The media library database (SQLite): scraped metadata, watch progress and cached image locations.
// Thread safe; every call takes a mutex. Lives at %LOCALAPPDATA%\SciFiShell\media\media.db.
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace sf {

struct Info {
    std::string key, title, summary, genres, year, rating, img, extra, extra2;
    bool found = false;
};

struct Episode {
    int season = 0, number = 0, runtime = 0;  // runtime in minutes
    std::string name, summary, img, airdate;
};

struct Progress {
    double pos = 0, dur = 0;
    bool watched = false;
    long long last = 0;
    bool any = false;
};

class MediaDb {
public:
    MediaDb();
    ~MediaDb();
    bool ok() const { return db_ != nullptr; }
    static std::string dir();

    bool getInfo(const std::string& key, Info& out);
    void putInfo(const Info& in);

    void putEpisodes(const std::string& showKey, const std::vector<Episode>& eps);
    std::vector<Episode> episodes(const std::string& showKey);

    Progress progress(const std::string& path);
    void setProgress(const std::string& path, double pos, double dur, bool watched);
    void markWatched(const std::string& path, bool watched);
    std::map<std::string, Progress> allProgress();

    double duration(const std::string& path);
    void setDuration(const std::string& path, double seconds);

private:
    void exec(const char* sql);
    sqlite3* db_ = nullptr;
    std::mutex m_;
};

}  // namespace sf

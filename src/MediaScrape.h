#pragma once
// Filename classification (TV episode / movie / other) and the web scraper behind the media library.
// Sources (all free, no API keys): TVmaze (shows, episodes), iTunes Search (movies), Deezer (artists, albums),
// Wikipedia (biographies, summaries) and lrclib.net (lyrics). One worker thread; results land in the MediaDb.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "MediaDb.h"

namespace sf {

struct VidClass {
    enum Kind { Other, Show, Movie } kind = Other;
    std::string name;  // cleaned title of the show / movie
    int season = 0, episode = 0, year = 0;
};
VidClass classifyVideo(const std::string& path);
// Lower-case key used for DB rows.
std::string keyOf(const std::string& s);

class MediaScrape {
public:
    explicit MediaScrape(MediaDb& db);
    ~MediaScrape();

    void requestShow(const std::string& name);
    void requestMovie(const std::string& name, int year);
    void requestArtist(const std::string& name);
    void requestAlbum(const std::string& artist, const std::string& album);
    void requestLyrics(const std::string& artist, const std::string& title, double seconds);
    // Downloads and caches a picture scaled to cover w x h; returns the cached file once it exists, else queues it.
    std::string image(const std::string& url, int w, int h);

    static std::string showKey(const std::string& n) { return "show:" + keyOf(n); }
    static std::string movieKey(const std::string& n, int y) { return "movie:" + keyOf(n) + "|" + std::to_string(y); }
    static std::string artistKey(const std::string& n) { return "artist:" + keyOf(n); }
    static std::string albumKey(const std::string& a, const std::string& b) { return "album:" + keyOf(a) + "|" + keyOf(b); }
    static std::string lyricsKey(const std::string& a, const std::string& t) { return "lyrics:" + keyOf(a) + "|" + keyOf(t); }

    bool poll(std::string& key);  // a finished key (info or picture); main thread
    size_t pending() const;
    void progress(size_t& done, size_t& total) const;
    bool offline() const { return failures_ >= 3 && successes_ == 0; }

private:
    struct Job {
        int kind = 0;
        std::string key, a, b, c;
        int n = 0, w = 0, h = 0;
        double d = 0;
    };
    void push(Job j);
    void run();
    void doShow(const Job& j);
    void doMovie(const Job& j);
    void doArtist(const Job& j);
    void doAlbum(const Job& j);
    void doLyrics(const Job& j);
    void doImage(const Job& j);
    bool wiki(const std::string& term, Info& out, const std::string& mustHave = std::string(), std::string* pageOut = nullptr);
    void wikiFilm(const std::string& page, std::string& credits, std::string& plot);
    bool musicBrainz(const std::string& name, Info& out);
    bool fetchJson(const std::string& url, std::string& body, int* status);

    MediaDb& db_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::set<std::string> asked_;
    std::deque<std::string> done_;
    bool quit_ = false, busy_ = false;
    size_t queued_ = 0, finished_ = 0;  // batch counters for the progress bar; reset when the queue drains
    std::atomic<int> failures_{0}, successes_{0};
    std::thread th_;
};

}  // namespace sf

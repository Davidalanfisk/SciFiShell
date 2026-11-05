#include "MediaScrape.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>

#include "MediaMeta.h"
#include "MiniJson.h"
#include "Net.h"
#include "stb_image.h"
#include "stb_image_write.h"

namespace sf {

namespace fs = std::filesystem;

namespace {

std::string lowerS(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (std::isspace(static_cast<unsigned char>(s[a])) || s[a] == '-' || s[a] == '.')) ++a;
    while (b > a && (std::isspace(static_cast<unsigned char>(s[b - 1])) || s[b - 1] == '-' || s[b - 1] == '.')) --b;
    return s.substr(a, b - a);
}

std::string stripHtml(const std::string& s) {
    std::string o;
    bool tag = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '<') {
            tag = true;
            if (s.compare(i, 3, "<br") == 0 || s.compare(i, 4, "</p>") == 0) o += ' ';
        } else if (c == '>') tag = false;
        else if (!tag) o += c;
    }
    const std::pair<const char*, const char*> ent[] = {{"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&rsquo;", "'"}, {"&nbsp;", " "}, {"&lt;", "<"}, {"&gt;", ">"}};
    for (auto& e : ent) {
        size_t p;
        while ((p = o.find(e.first)) != std::string::npos) o.replace(p, std::strlen(e.first), e.second);
    }
    return trim(o);
}

bool isJunk(const std::string& w) {
    static const char* junk[] = {"1080p", "720p", "2160p", "480p", "4k", "uhd", "bluray", "brrip", "bdrip", "webrip", "web", "webdl", "hdtv", "dvdrip", "x264", "x265", "h264", "h265", "hevc", "xvid",
                                 "aac", "remux", "proper", "repack", "extended", "unrated", "hdr", "10bit", "dts", "ac3", "yify", "rarbg", "amzn", "nf"};
    for (const char* j : junk) if (w == j) return true;
    return false;
}

// "The.Matrix.1999.1080p.BluRay" -> name "The Matrix", year 1999.
std::string cleanTitle(std::string s, int* yearOut = nullptr) {
    std::string t;
    // keep "(1999)" style years, drop other bracketed groups
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '[') { const size_t e = s.find(']', i); i = e == std::string::npos ? s.size() : e; t += ' '; continue; }
        if (s[i] == '(') {
            const size_t e = s.find(')', i);
            const std::string in = e == std::string::npos ? "" : s.substr(i + 1, e - i - 1);
            if (in.size() == 4 && std::isdigit(static_cast<unsigned char>(in[0]))) t += " " + in + " ";
            i = e == std::string::npos ? s.size() : e;
            continue;
        }
        t += (s[i] == '.' || s[i] == '_') ? ' ' : s[i];
    }
    std::string out, w;
    std::vector<std::string> words;
    for (size_t i = 0; i <= t.size(); ++i) {
        if (i == t.size() || t[i] == ' ') { if (!w.empty()) words.push_back(w); w.clear(); }
        else w += t[i];
    }
    int year = 0;
    size_t yi = words.size();
    for (size_t i = 1; i < words.size(); ++i) {
        const std::string& w4 = words[i];
        if (w4.size() == 4 && std::all_of(w4.begin(), w4.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
            const int y = std::atoi(w4.c_str());
            if (y >= 1900 && y <= 2099) { year = y; yi = i; }
        }
    }
    for (size_t i = 0; i < yi && i < words.size(); ++i) {
        if (isJunk(lowerS(words[i]))) break;
        out += (out.empty() ? "" : " ") + words[i];
    }
    if (yearOut) *yearOut = year;
    return trim(out);
}

bool isSeasonFolder(const std::string& n) {
    static const std::regex re("^(season|series|s)[ ._-]*\\d+.*$", std::regex::icase);
    return std::regex_match(n, re);
}

}  // namespace

std::string keyOf(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c >= 0x80) o += static_cast<char>(std::tolower(c));
        else if (!o.empty() && o.back() != ' ') o += ' ';
    }
    return trim(o);
}

VidClass classifyVideo(const std::string& path) {
    VidClass r;
    const fs::path p = fs::u8path(path);
    const std::string stem = p.stem().u8string();
    std::vector<std::string> parents;
    for (fs::path q = p.parent_path(); q.has_filename() && parents.size() < 4; q = q.parent_path()) parents.push_back(q.filename().u8string());

    static const std::regex se("(?:^|[^A-Za-z0-9])[sS](\\d{1,2})[ ._-]*[eE](\\d{1,3})");
    static const std::regex nx("(?:^|[^0-9])(\\d{1,2})x(\\d{2,3})(?:[^0-9]|$)");
    std::smatch m;
    bool tv = false;
    size_t cut = 0;
    if (std::regex_search(stem, m, se) || std::regex_search(stem, m, nx)) {
        tv = true;
        r.season = std::atoi(m[1].str().c_str());
        r.episode = std::atoi(m[2].str().c_str());
        cut = static_cast<size_t>(m.position(0));
    } else if (!parents.empty() && isSeasonFolder(parents[0])) {
        // "01 - Pilot.mp4" inside "Season 1"
        std::smatch sm;
        static const std::regex sn("(\\d+)");
        if (std::regex_search(parents[0], sm, sn)) r.season = std::atoi(sm[1].str().c_str());
        static const std::regex en("^\\s*(?:e|ep|episode)?\\s*(\\d{1,3})");
        std::smatch em;
        if (std::regex_search(stem, em, en)) { r.episode = std::atoi(em[1].str().c_str()); tv = true; }
    }
    if (tv) {
        std::string name = cleanTitle(stem.substr(0, cut));
        if (name.empty()) {
            for (const std::string& pn : parents) {
                if (isSeasonFolder(pn)) continue;
                name = cleanTitle(pn);
                if (!name.empty()) break;
            }
        }
        if (!name.empty()) {
            r.kind = VidClass::Show;
            r.name = name;
            if (r.season <= 0) r.season = 1;
            return r;
        }
    }
    int year = 0;
    const std::string name = cleanTitle(stem, &year);
    bool movieFolder = false;
    for (const std::string& pn : parents) {
        const std::string l = lowerS(pn);
        if (l == "movies" || l == "movie" || l == "films" || l == "film") movieFolder = true;
    }
    if (!name.empty() && (year > 0 || movieFolder)) {
        r.kind = VidClass::Movie;
        r.name = name;
        r.year = year;
    }
    return r;
}

// ------------------------------------------------------------------------------------------------

MediaScrape::MediaScrape(MediaDb& db) : db_(db) {
    std::error_code ec;
    fs::create_directories(fs::u8path(MediaDb::dir() + "img"), ec);
    th_ = std::thread([this] { run(); });
}

MediaScrape::~MediaScrape() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void MediaScrape::push(Job j) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!asked_.insert(j.key).second) return;
        jobs_.push_back(std::move(j));
        ++queued_;
    }
    cv_.notify_one();
}

void MediaScrape::requestShow(const std::string& name) { Job j; j.kind = 0; j.key = showKey(name); j.a = name; push(j); }
void MediaScrape::requestMovie(const std::string& name, int year) { Job j; j.kind = 1; j.key = movieKey(name, year); j.a = name; j.n = year; push(j); }
void MediaScrape::requestArtist(const std::string& name) { Job j; j.kind = 2; j.key = artistKey(name); j.a = name; push(j); }
void MediaScrape::requestAlbum(const std::string& artist, const std::string& album) { Job j; j.kind = 3; j.key = albumKey(artist, album); j.a = artist; j.b = album; push(j); }
void MediaScrape::requestLyrics(const std::string& artist, const std::string& title, double seconds) { Job j; j.kind = 4; j.key = lyricsKey(artist, title); j.a = artist; j.b = title; j.d = seconds; push(j); }

std::string MediaScrape::image(const std::string& url, int w, int h) {
    if (url.empty()) return {};
    const std::string key = "img:" + url + "|" + std::to_string(w) + "x" + std::to_string(h);
    const size_t hv = std::hash<std::string>{}(key);
    char name[40];
    std::snprintf(name, sizeof name, "%016zx.jpg", hv);
    const std::string path = MediaDb::dir() + "img\\" + name;
    std::error_code ec;
    if (fs::exists(fs::u8path(path), ec)) return path;
    Job j;
    j.kind = 5;
    j.key = key;
    j.a = url;
    j.b = path;
    j.w = w;
    j.h = h;
    push(j);
    return {};
}

bool MediaScrape::poll(std::string& key) {
    std::lock_guard<std::mutex> lk(m_);
    if (done_.empty()) return false;
    key = done_.front();
    done_.pop_front();
    return true;
}

void MediaScrape::progress(size_t& done, size_t& total) const {
    std::lock_guard<std::mutex> lk(m_);
    done = finished_;
    total = queued_;
}

size_t MediaScrape::pending() const {
    std::lock_guard<std::mutex> lk(m_);
    return jobs_.size() + (busy_ ? 1 : 0);
}

bool MediaScrape::fetchJson(const std::string& url, std::string& body, int* status) {
    int st = 0;
    const bool ok = net::getString(url, body, &st);
    if (status) *status = st;
    if (st == 0) ++failures_;
    else { ++successes_; failures_ = 0; }
    return ok;
}

void MediaScrape::run() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            j = jobs_.front();
            jobs_.pop_front();
            busy_ = true;
        }
        // Rows already in the database never hit the network again.
        Info have;
        const bool cached = j.kind != 5 && db_.getInfo(j.key, have);
        if (!cached) {
            switch (j.kind) {
                case 0: doShow(j); break;
                case 1: doMovie(j); break;
                case 2: doArtist(j); break;
                case 3: doAlbum(j); break;
                case 4: doLyrics(j); break;
                case 5: doImage(j); break;
            }
        }
        std::lock_guard<std::mutex> lk(m_);
        busy_ = false;
        done_.push_back(j.key);
        if (cached) { if (queued_) --queued_; } else ++finished_;  // database hits are instant: keep them out of the progress bar
        if (finished_ >= queued_) finished_ = queued_ = 0;
        if (failures_ >= 3 && successes_ == 0) {
            // offline: forget these requests so they are retried on the next visit
            asked_.erase(j.key);
        }
    }
}

// Name variants to try when the full tag does not match anything: "A feat. B", "A vs B", "A & B", "A - B".
std::vector<std::string> nameVariants(const std::string& full) {
    std::vector<std::string> v{full};
    std::string low = full;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t cut = std::string::npos;
    for (const char* sep : {" feat. ", " feat ", " ft. ", " ft ", " featuring ", " vs. ", " vs ", " with ", " & ", " and ", " + ", ", ", " / ", " - ", " x ", " presents ", " present "}) {
        const size_t p = low.find(sep);
        if (p != std::string::npos && p > 1 && (cut == std::string::npos || p < cut)) cut = p;
    }
    if (cut != std::string::npos) v.push_back(full.substr(0, cut));
    // drop trailing words one at a time ("Neurosis Jarboe" -> "Neurosis"), keeping at least one
    std::string t = full;
    for (int i = 0; i < 3; ++i) {
        const size_t sp = t.find_last_of(' ');
        if (sp == std::string::npos || sp < 2) break;
        t = t.substr(0, sp);
        v.push_back(t);
    }
    return v;
}

bool MediaScrape::musicBrainz(const std::string& name, Info& out) {
    std::string body;
    int st = 0;
    if (!fetchJson("https://musicbrainz.org/ws/2/artist/?fmt=json&limit=3&query=artist:" + net::urlEncode("\"" + name + "\""), body, &st)) return false;
    const Json js = Json::parse(body);
    const Json& a = js["artists"];
    for (size_t i = 0; i < a.size(); ++i) {
        if (keyOf(a.at(i)["name"].str()) != keyOf(name)) continue;
        const Json& m = a.at(i);
        out.year = m["life-span"]["begin"].str().substr(0, 4);
        std::string area = m["area"]["name"].str(m["country"].str());
        std::string typ = m["type"].str();
        out.extra2 = area + (area.empty() || typ.empty() ? "" : "  /  ") + typ;
        const Json& tg = m["tags"];
        std::vector<std::pair<int, std::string>> tags;
        for (size_t k = 0; k < tg.size(); ++k) tags.push_back({static_cast<int>(tg.at(k)["count"].num()), tg.at(k)["name"].str()});
        std::sort(tags.begin(), tags.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (size_t k = 0; k < tags.size() && k < 4; ++k) out.genres += (k ? ", " : "") + tags[k].second;
        return true;
    }
    return false;
}

// Strips wikitext markup from an infobox value: templates, refs, links and list bullets; entries are comma-separated.
static std::string wikiClean(std::string s) {
    auto eraseBetween = [&](const std::string& open, const std::string& close) {
        for (size_t p; (p = s.find(open)) != std::string::npos;) {
            const size_t e = s.find(close, p);
            s.erase(p, e == std::string::npos ? std::string::npos : e + close.size() - p);
        }
    };
    eraseBetween("<!--", "-->");
    eraseBetween("<ref", "</ref>");
    std::string low = s;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* op : {"{{plainlist|", "{{ubl|", "{{unbulleted list|", "{{hlist|", "{{flatlist|", "{{plain list|"})
        for (size_t p; (p = low.find(op)) != std::string::npos;) {
            s.erase(p, std::strlen(op));
            low.erase(p, std::strlen(op));
        }
    for (int guard = 0; guard < 8; ++guard) {  // drop the remaining templates, innermost first
        const size_t e = s.find("}}");
        if (e == std::string::npos) break;
        const size_t b = s.rfind("{{", e);
        const size_t from = b == std::string::npos ? 0 : b;
        s.erase(from, e + 2 - from);
    }
    for (size_t p; (p = s.find("[[")) != std::string::npos;) {
        const size_t e = s.find("]]", p);
        if (e == std::string::npos) break;
        std::string inner = s.substr(p + 2, e - p - 2);
        const size_t bar = inner.rfind('|');
        if (bar != std::string::npos) inner = inner.substr(bar + 1);
        s.replace(p, e + 2 - p, inner);
    }
    std::string out;
    bool pendingSep = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\'' || c == '}' || c == '{') continue;
        if (c == '<') {
            const size_t e = s.find('>', i);
            if (e != std::string::npos) { pendingSep = true; i = e; continue; }
        }
        if (c == '\n' || c == '*' || c == '|' || c == ',') { pendingSep = true; continue; }
        if (c == ' ' && (out.empty() || pendingSep)) continue;
        if (pendingSep && !out.empty()) out += ", ";
        pendingSep = false;
        out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == ',')) out.pop_back();
    return out;
}

static std::string infoboxField(const std::string& wt, const char* name) {
    size_t p = 0;
    const std::string n = name;
    while ((p = wt.find("\n|", p)) != std::string::npos) {
        size_t q = p + 2;
        while (q < wt.size() && wt[q] == ' ') ++q;
        if (wt.compare(q, n.size(), n) == 0) {
            size_t r = q + n.size();
            while (r < wt.size() && wt[r] == ' ') ++r;
            if (r < wt.size() && wt[r] == '=') {
                const size_t e = wt.find("\n|", r);
                return wikiClean(wt.substr(r + 1, std::min<size_t>(e == std::string::npos ? 900 : e - r - 1, 900)));
            }
        }
        p += 2;
    }
    return {};
}

// Credits (director, writers, cast, studio...) from the film infobox, and the plot section of the article.
void MediaScrape::wikiFilm(const std::string& page, std::string& credits, std::string& plot) {
    std::string body;
    int st = 0;
    std::string t = page;
    std::replace(t.begin(), t.end(), ' ', '_');
    if (fetchJson("https://en.wikipedia.org/w/api.php?action=parse&prop=wikitext&section=0&format=json&redirects=1&page=" + net::urlEncode(t), body, &st)) {
        const Json js = Json::parse(body);
        const std::string wt = js["parse"]["wikitext"]["*"].str();
        auto add = [&](const char* label, const char* field) {
            const std::string v = infoboxField(wt, field);
            if (!v.empty()) credits += std::string(label) + "\t" + v + "\n";
        };
        add("DIRECTED BY", "director");
        add("WRITTEN BY", "screenplay");
        if (credits.find("WRITTEN BY") == std::string::npos) add("WRITTEN BY", "writer");
        add("STARRING", "starring");
        add("MUSIC BY", "music");
        add("STUDIO", "production_company");
        add("COUNTRY", "country");
        add("BUDGET", "budget");
        add("BOX OFFICE", "gross");
    }
    if (fetchJson("https://en.wikipedia.org/w/api.php?action=query&prop=extracts&explaintext=1&exsectionformat=plain&format=json&redirects=1&titles=" + net::urlEncode(t), body, &st)) {
        const Json js = Json::parse(body);
        const Json& pages = js["query"]["pages"];
        const std::string text = pages.first()["extract"].str();
        for (const char* h : {"\nPlot\n", "\nSynopsis\n", "\nPremise\n", "\nPlot summary\n"}) {
            const size_t p = text.find(h);
            if (p == std::string::npos) continue;
            size_t e = text.find("\n\n\n", p + 6);
            if (e == std::string::npos) e = text.size();
            plot = text.substr(p + std::strlen(h), e - p - std::strlen(h));
            if (plot.size() > 3000) {
                plot.resize(3000);
                const size_t dot = plot.find_last_of('.');
                if (dot != std::string::npos && dot > 1000) plot.resize(dot + 1);
            }
            break;
        }
    }
}

bool MediaScrape::wiki(const std::string& term, Info& out, const std::string& mustHave, std::string* pageOut) {
    std::string body;
    int st = 0;
    if (!fetchJson("https://en.wikipedia.org/w/api.php?action=query&list=search&format=json&srlimit=3&srsearch=" + net::urlEncode(term), body, &st)) return false;
    const Json s = Json::parse(body);
    std::string title;
    for (size_t i = 0; i < 3; ++i) {
        const std::string tt = s["query"]["search"].at(i)["title"].str();
        if (tt.empty()) break;
        if (mustHave.empty() || keyOf(tt).find(keyOf(mustHave)) != std::string::npos) { title = tt; break; }
    }
    if (title.empty()) return false;
    if (pageOut) *pageOut = title;
    std::string t2 = title;
    std::replace(t2.begin(), t2.end(), ' ', '_');
    if (!fetchJson("https://en.wikipedia.org/api/rest_v1/page/summary/" + net::urlEncode(t2), body, &st)) return false;
    const Json j = Json::parse(body);
    const std::string ex = j["extract"].str();
    if (ex.empty()) return false;
    out.summary = ex;
    // the full intro (several paragraphs) is richer than the REST summary's first paragraph
    if (fetchJson("https://en.wikipedia.org/w/api.php?action=query&prop=extracts&exintro=1&explaintext=1&format=json&redirects=1&titles=" + net::urlEncode(t2), body, &st)) {
        const Json pj = Json::parse(body);
        const Json& pages = pj["query"]["pages"];
        std::string full = pages.first()["extract"].str();
        if (full.size() > ex.size() + 40) {
            if (full.size() > 2400) {
                full.resize(2400);
                const size_t dot = full.find_last_of('.');
                if (dot != std::string::npos && dot > 800) full.resize(dot + 1);
            }
            out.summary = full;
        }
    }
    out.extra2 = "https://en.wikipedia.org/wiki/" + net::urlEncode(t2);
    if (out.img.empty()) out.img = j["originalimage"]["source"].str(j["thumbnail"]["source"].str());
    return true;
}

void MediaScrape::doShow(const Job& j) {
    Info in;
    in.key = j.key;
    in.title = j.a;
    std::string body;
    int st = 0;
    bool ok = false;
    for (const std::string& nm : nameVariants(j.a)) {
        ok = fetchJson("https://api.tvmaze.com/singlesearch/shows?embed=episodes&q=" + net::urlEncode(nm), body, &st);
        if (ok) break;
        if (st != 404) return;  // offline: try again later
    }
    if (ok) {
        const Json js = Json::parse(body);
        if (!js["name"].str().empty()) {
            in.found = true;
            in.title = js["name"].str();
            in.summary = stripHtml(js["summary"].str());
            const Json& g = js["genres"];
            for (size_t i = 0; i < g.size(); ++i) in.genres += (i ? ", " : "") + g.at(i).str();
            in.year = js["premiered"].str().substr(0, 4);
            const double rt = js["rating"]["average"].num(0);
            if (rt > 0) { char b[16]; std::snprintf(b, sizeof b, "%.1f", rt); in.rating = b; }
            in.img = js["image"]["original"].str(js["image"]["medium"].str());
            std::string net_ = js["network"]["name"].str(js["webChannel"]["name"].str());
            in.extra = net_ + (net_.empty() ? "" : "  /  ") + js["status"].str();
            std::vector<Episode> eps;
            const Json& e = js["_embedded"]["episodes"];
            for (size_t i = 0; i < e.size(); ++i) {
                Episode ep;
                ep.season = static_cast<int>(e.at(i)["season"].num());
                ep.number = static_cast<int>(e.at(i)["number"].num());
                if (ep.number <= 0) continue;  // specials
                ep.name = e.at(i)["name"].str();
                ep.runtime = static_cast<int>(e.at(i)["runtime"].num());
                ep.airdate = e.at(i)["airdate"].str();
                ep.summary = stripHtml(e.at(i)["summary"].str());
                ep.img = e.at(i)["image"]["medium"].str();
                eps.push_back(ep);
            }
            db_.putEpisodes(j.key, eps);
        }
    }
    db_.putInfo(in);
}

void MediaScrape::doMovie(const Job& j) {
    Info in;
    in.key = j.key;
    in.title = j.a;
    in.year = j.n ? std::to_string(j.n) : "";
    std::string body;
    int st = 0;
    const std::string want = keyOf(j.a);
    std::string cast, director;
    // Source 1: IMDb's public suggestion feed (no key): title, year, poster, top cast.
    {
        std::string q;
        for (char c : j.a) q += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ') ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : ' ';
        const size_t f = q.find_first_not_of(' ');
        if (f != std::string::npos) {
            const std::string url = std::string("https://v2.sg.media-imdb.com/suggestion/") + q[f] + "/" + net::urlEncode(q) + ".json";
            if (fetchJson(url, body, &st)) {
                const Json js = Json::parse(body);
                const Json& d = js["d"];
                int best = -1, bestScore = -1;
                for (size_t i = 0; i < d.size(); ++i) {
                    const std::string qid = d.at(i)["qid"].str();
                    if (qid != "movie" && qid != "tvMovie" && qid != "video") continue;
                    const std::string nm = keyOf(d.at(i)["l"].str());
                    int score = 0;
                    if (nm == want) score += 4;
                    else if (nm.find(want) != std::string::npos || want.find(nm) != std::string::npos) score += 1;
                    else continue;
                    const int y = static_cast<int>(d.at(i)["y"].num());
                    if (j.n && y == j.n) score += 3;
                    else if (j.n && std::abs(y - j.n) == 1) score += 2;
                    if (qid == "movie") score += 1;
                    if (score > bestScore) { bestScore = score; best = static_cast<int>(i); }
                }
                if (best >= 0) {
                    const Json& m = d.at(static_cast<size_t>(best));
                    in.found = true;
                    in.title = m["l"].str();
                    if (m["y"].num() > 0) in.year = std::to_string(static_cast<int>(m["y"].num()));
                    cast = m["s"].str();
                    std::string img = m["i"]["imageUrl"].str();
                    const size_t p = img.rfind("._V1_");
                    if (p != std::string::npos) img = img.substr(0, p) + "._V1_UX600_.jpg";
                    in.img = img;
                }
            } else if (st != 404) return;
        }
    }
    // Source 2: iTunes (genre, runtime, rating, description)
    if (fetchJson("https://itunes.apple.com/search?media=movie&entity=movie&limit=10&term=" + net::urlEncode(j.a + (j.n ? " " + std::to_string(j.n) : "")), body, &st)) {
        const Json js = Json::parse(body);
        const Json& r = js["results"];
        for (size_t i = 0; i < r.size(); ++i) {
            if (keyOf(r.at(i)["trackName"].str()) != want) continue;
            const std::string y = r.at(i)["releaseDate"].str().substr(0, 4);
            if (j.n && y != std::to_string(j.n) && std::abs(std::atoi(y.c_str()) - j.n) > 1) continue;
            const Json& m = r.at(i);
            in.found = true;
            in.genres = m["primaryGenreName"].str();
            in.summary = m["longDescription"].str(m["shortDescription"].str());
            if (in.img.empty()) {
                std::string art = m["artworkUrl100"].str();
                const size_t p = art.find("100x100");
                if (p != std::string::npos) art.replace(p, 7, "600x900");
                in.img = art;
            }
            const int mins = static_cast<int>(m["trackTimeMillis"].num() / 60000);
            in.extra2 = (mins > 0 ? std::to_string(mins) + " min" : std::string()) + (m["contentAdvisoryRating"].str().empty() ? "" : "  /  " + m["contentAdvisoryRating"].str());
            director = m["artistName"].str();
            break;
        }
    }
    // Source 3: Wikipedia (intro, plot, full credits, and a poster if nothing else had one)
    std::string credits, plot;
    {
        Info w;
        w.img = in.img;
        std::string page;
        const std::string yr = in.year.empty() ? std::string() : " " + in.year;
        bool hit = wiki(in.title + yr + " film", w, in.title, &page);
        if (!hit && j.n) hit = wiki(in.title + " film", w, in.title, &page);
        if (hit) {
            in.found = true;
            if (in.summary.empty()) in.summary = w.summary;
            if (in.img.empty()) in.img = w.img;
            wikiFilm(page, credits, plot);
        }
    }
    if (credits.find("DIRECTED BY") == std::string::npos && !director.empty()) credits = "DIRECTED BY\t" + director + "\n" + credits;
    if (credits.find("STARRING") == std::string::npos && !cast.empty()) credits += "STARRING\t" + cast + "\n";
    in.extra = credits;
    if (!plot.empty()) in.summary += (in.summary.empty() ? "" : "\n\n") + plot;
    db_.putInfo(in);
}
void MediaScrape::doArtist(const Job& j) {
    Info in;
    in.key = j.key;
    in.title = j.a;
    std::string body;
    int st = 0;
    // Only an exact name match is trusted: a near match gives the wrong picture. Variants cover "A feat. B" style tags.
    std::string matched;
    for (const std::string& nm : nameVariants(j.a)) {
        const bool ok = fetchJson("https://api.deezer.com/search/artist?limit=5&q=" + net::urlEncode(nm), body, &st);
        if (!ok && st != 404) return;
        if (!ok) continue;
        const Json js = Json::parse(body);
        const Json& d = js["data"];
        for (size_t i = 0; i < d.size(); ++i) {
            if (keyOf(d.at(i)["name"].str()) != keyOf(nm)) continue;
            matched = nm;
            in.found = true;
            in.title = d.at(i)["name"].str();
            in.img = d.at(i)["picture_xl"].str(d.at(i)["picture_big"].str());
            if (in.img.find("/artist//") != std::string::npos) in.img.clear();  // Deezer's "no picture" placeholder
            const long long fans = static_cast<long long>(d.at(i)["nb_fan"].num());
            if (fans > 0) {
                char fb[32];
                if (fans >= 1000000) std::snprintf(fb, sizeof fb, "%.1fM FANS", fans / 1e6);
                else if (fans >= 1000) std::snprintf(fb, sizeof fb, "%lldK FANS", fans / 1000);
                else std::snprintf(fb, sizeof fb, "%lld FANS", fans);
                in.extra = fb;
            }
            break;
        }
        if (!matched.empty()) break;
    }
    const std::string who = matched.empty() ? j.a : matched;
    Info mb;
    if (musicBrainz(who, mb)) {
        in.found = true;
        in.year = mb.year;
        in.genres = mb.genres;
        in.extra2 = mb.extra2;
    }
    Info w;
    w.img = in.img.empty() ? std::string() : "x";  // the Deezer picture wins; Wikipedia's is the fallback
    if (wiki(who + " musician band", w, who)) {
        in.summary = w.summary;
        in.found = true;
        if (in.img.empty() && w.img != "x") in.img = w.img;
    }
    db_.putInfo(in);
}

// "Album (Deluxe Edition) [CD 2]", "Album - Disc 1" -> "Album"
static std::string cleanAlbum(std::string s) {
    for (const char open : {'(', '['}) {
        const size_t p = s.find(open);
        if (p != std::string::npos && p > 1) s.erase(p);
    }
    std::string low = s;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t cut = std::string::npos;
    for (const char* tag : {" disc ", " disk ", " cd ", " - disc", " - cd", " cd1", " cd2", " disc1", " disc2"}) {
        const size_t p = low.rfind(tag);
        if (p != std::string::npos && p > 1 && (cut == std::string::npos || p < cut)) cut = p;
    }
    if (cut != std::string::npos) s.erase(cut);
    while (!s.empty() && (s.back() == ' ' || s.back() == '-')) s.pop_back();
    return s.empty() ? std::string() : s;
}

void MediaScrape::doAlbum(const Job& j) {
    Info in;
    in.key = j.key;
    in.title = j.b;
    std::string body;
    int st = 0;
    std::string bt = cleanAlbum(j.b);
    if (bt.empty()) bt = j.b;
    const std::string akey = keyOf(j.a);
    const bool various = akey.empty() || akey.find("various") != std::string::npos || akey == "unknown artist";
    auto close = [&](const std::string& t) { const std::string a = keyOf(t), b = keyOf(bt); return !a.empty() && !b.empty() && (a == b || a.find(b) != std::string::npos || b.find(a) != std::string::npos); };
    auto artistOk = [&](const std::string& n) {
        if (various) return true;
        const std::string k = keyOf(n);
        for (const std::string& v : nameVariants(j.a)) {
            const std::string vk = keyOf(v);
            if (vk.size() > 1 && (k.find(vk) != std::string::npos || vk.find(k) != std::string::npos)) return true;
        }
        return false;
    };
    std::string id;
    bool offline = false;
    auto deezer = [&](const std::string& q) {
        if (!id.empty()) return;
        if (!fetchJson("https://api.deezer.com/search/album?limit=10&q=" + net::urlEncode(q), body, &st)) { if (st != 404) offline = true; return; }
        const Json js = Json::parse(body);
        const Json& d = js["data"];
        for (int pass = 0; pass < 2 && id.empty(); ++pass)
            for (size_t i = 0; i < d.size() && id.empty(); ++i) {
                const std::string t = d.at(i)["title"].str();
                if (pass == 0 ? keyOf(t) != keyOf(bt) : !close(t)) continue;
                if (!artistOk(d.at(i)["artist"]["name"].str())) continue;
                id = d.at(i)["id"].str();
                in.img = d.at(i)["cover_xl"].str(d.at(i)["cover_big"].str());
            }
    };
    deezer("artist:\"" + j.a + "\" album:\"" + bt + "\"");
    if (offline) return;
    deezer(j.a + " " + bt);
    if (id.empty() && !various) deezer(nameVariants(j.a).back() + " " + bt);
    if (id.empty()) deezer(bt);
    if (!id.empty() && fetchJson("https://api.deezer.com/album/" + id, body, &st)) {
        const Json a = Json::parse(body);
        in.found = true;
        in.title = a["title"].str(j.b);
        in.year = a["release_date"].str().substr(0, 4);
        const Json& g = a["genres"]["data"];
        for (size_t i = 0; i < g.size(); ++i) in.genres += (i ? ", " : "") + g.at(i)["name"].str();
        in.extra = a["label"].str();
        const int n = static_cast<int>(a["nb_tracks"].num());
        const int secs = static_cast<int>(a["duration"].num());
        in.extra2 = (n ? std::to_string(n) + " tracks" : std::string()) + (secs ? "  /  " + std::to_string(secs / 60) + " min" : std::string());
    }
    // MusicBrainz + Cover Art Archive covers the obscure releases Deezer lacks.
    if (in.img.empty()) {
        const std::string q = "releasegroup:\"" + bt + "\"" + (various ? std::string() : " AND artist:\"" + nameVariants(j.a).back() + "\"");
        if (fetchJson("https://musicbrainz.org/ws/2/release-group/?fmt=json&limit=5&query=" + net::urlEncode(q), body, &st)) {
            const Json js = Json::parse(body);
            const Json& r = js["release-groups"];
            for (size_t i = 0; i < r.size(); ++i) {
                if (!close(r.at(i)["title"].str())) continue;
                in.img = "https://coverartarchive.org/release-group/" + r.at(i)["id"].str() + "/front-500";
                in.found = true;
                if (in.year.empty()) in.year = r.at(i)["first-release-date"].str().substr(0, 4);
                break;
            }
        }
    }
    // iTunes as a last source for covers
    if (in.img.empty() && fetchJson("https://itunes.apple.com/search?media=music&entity=album&limit=8&term=" + net::urlEncode((various ? std::string() : j.a + " ") + bt), body, &st)) {
        const Json js = Json::parse(body);
        const Json& r = js["results"];
        for (size_t i = 0; i < r.size(); ++i) {
            if (!close(r.at(i)["collectionName"].str()) || !artistOk(r.at(i)["artistName"].str())) continue;
            std::string art = r.at(i)["artworkUrl100"].str();
            const size_t p = art.find("100x100");
            if (p != std::string::npos) art.replace(p, 7, "600x600");
            in.img = art;
            in.found = true;
            if (in.year.empty()) in.year = r.at(i)["releaseDate"].str().substr(0, 4);
            if (in.genres.empty()) in.genres = r.at(i)["primaryGenreName"].str();
            break;
        }
    }
    Info w;
    w.img = "x";
    if (wiki(bt + (various ? std::string() : " " + j.a) + " album", w, bt)) {
        in.summary = w.summary;
        in.found = true;
    }
    db_.putInfo(in);
}
void MediaScrape::doLyrics(const Job& j) {
    Info in;
    in.key = j.key;
    in.title = j.b;
    std::string body;
    int st = 0;
    std::string q = "artist_name=" + net::urlEncode(j.a) + "&track_name=" + net::urlEncode(j.b);
    if (j.d > 0) q += "&duration=" + std::to_string(static_cast<int>(j.d));
    bool ok = fetchJson("https://lrclib.net/api/get?" + q, body, &st);
    if (!ok && st == 0) return;
    if (ok) {
        const Json js = Json::parse(body);
        in.summary = js["plainLyrics"].str();
        if (js["instrumental"].b) in.summary = "(instrumental)";
    }
    if (in.summary.empty() && fetchJson("https://lrclib.net/api/search?q=" + net::urlEncode(j.a + " " + j.b), body, &st)) {
        const Json js = Json::parse(body);
        in.summary = js.at(0)["plainLyrics"].str();
    }
    in.found = !in.summary.empty();
    db_.putInfo(in);
}

void MediaScrape::doImage(const Job& j) {
    std::vector<unsigned char> data;
    int st = 0;
    // A definite failure (404, bad image) leaves an empty marker file so it is not fetched again on every launch.
    auto marker = [&] { std::ofstream(fs::u8path(j.b), std::ios::binary); };
    if (!net::get(j.a, data, &st)) {
        if (st == 0) ++failures_;
        else marker();
        return;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &n, 4);
    if (!px) { marker(); return; }
    Bitmap src;
    src.w = w;
    src.h = h;
    src.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    const Bitmap out = scaleCover(src, j.w, j.h);
    stbi_write_jpg(j.b.c_str(), out.w, out.h, 4, out.rgba.data(), 88);
}

}  // namespace sf

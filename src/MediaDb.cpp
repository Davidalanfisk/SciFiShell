#include "MediaDb.h"

#include <sqlite3.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace sf {

namespace {
std::string col(sqlite3_stmt* s, int i) {
    const unsigned char* t = sqlite3_column_text(s, i);
    return t ? reinterpret_cast<const char*>(t) : std::string();
}
void bindText(sqlite3_stmt* s, int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); }

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { if (s) sqlite3_finalize(s); }
};
}  // namespace

std::string MediaDb::dir() {
    const char* la = std::getenv("LOCALAPPDATA");
    return std::string(la ? la : ".") + "\\SciFiShell\\media\\";
}

MediaDb::MediaDb() {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir()), ec);
    if (sqlite3_open((dir() + "media.db").c_str(), &db_) != SQLITE_OK) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    sqlite3_busy_timeout(db_, 3000);
    exec("PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;");
    exec("CREATE TABLE IF NOT EXISTS info(key TEXT PRIMARY KEY,title TEXT,summary TEXT,genres TEXT,year TEXT,rating TEXT,img TEXT,extra TEXT,extra2 TEXT,found INTEGER,fetched INTEGER);"
         "CREATE TABLE IF NOT EXISTS episodes(show TEXT,season INTEGER,num INTEGER,name TEXT,summary TEXT,img TEXT,runtime INTEGER,airdate TEXT,PRIMARY KEY(show,season,num));"
         "CREATE TABLE IF NOT EXISTS progress(path TEXT PRIMARY KEY,pos REAL,dur REAL,watched INTEGER,last INTEGER);"
         "CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY,dur REAL);");
    // v2: matching got stricter, so drop earlier scraped results (except lyrics) and rescrape them
    sqlite3_stmt* v = nullptr;
    int ver = 0;
    if (sqlite3_prepare_v2(db_, "PRAGMA user_version", -1, &v, nullptr) == SQLITE_OK && sqlite3_step(v) == SQLITE_ROW) ver = sqlite3_column_int(v, 0);
    sqlite3_finalize(v);
    if (ver < 4) {
        exec("DELETE FROM info WHERE key LIKE 'movie:%' OR key LIKE 'album:%' OR key LIKE 'artist:%' OR (key NOT LIKE 'lyrics:%' AND (found=0 OR img=''));PRAGMA user_version=4;");
    }
}

MediaDb::~MediaDb() {
    if (db_) sqlite3_close(db_);
}

void MediaDb::exec(const char* sql) { sqlite3_exec(db_, sql, nullptr, nullptr, nullptr); }

bool MediaDb::getInfo(const std::string& key, Info& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return false;
    Stmt st(db_, "SELECT title,summary,genres,year,rating,img,extra,extra2,found FROM info WHERE key=?");
    if (!st.s) return false;
    bindText(st.s, 1, key);
    if (sqlite3_step(st.s) != SQLITE_ROW) return false;
    out.key = key;
    out.title = col(st.s, 0);
    out.summary = col(st.s, 1);
    out.genres = col(st.s, 2);
    out.year = col(st.s, 3);
    out.rating = col(st.s, 4);
    out.img = col(st.s, 5);
    out.extra = col(st.s, 6);
    out.extra2 = col(st.s, 7);
    out.found = sqlite3_column_int(st.s, 8) != 0;
    return true;
}

void MediaDb::putInfo(const Info& in) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return;
    Stmt st(db_, "INSERT OR REPLACE INTO info VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    if (!st.s) return;
    bindText(st.s, 1, in.key);
    bindText(st.s, 2, in.title);
    bindText(st.s, 3, in.summary);
    bindText(st.s, 4, in.genres);
    bindText(st.s, 5, in.year);
    bindText(st.s, 6, in.rating);
    bindText(st.s, 7, in.img);
    bindText(st.s, 8, in.extra);
    bindText(st.s, 9, in.extra2);
    sqlite3_bind_int(st.s, 10, in.found ? 1 : 0);
    sqlite3_bind_int64(st.s, 11, static_cast<sqlite3_int64>(std::time(nullptr)));
    sqlite3_step(st.s);
}

void MediaDb::putEpisodes(const std::string& showKey, const std::vector<Episode>& eps) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return;
    exec("BEGIN");
    for (const Episode& e : eps) {
        Stmt st(db_, "INSERT OR REPLACE INTO episodes VALUES(?,?,?,?,?,?,?,?)");
        if (!st.s) continue;
        bindText(st.s, 1, showKey);
        sqlite3_bind_int(st.s, 2, e.season);
        sqlite3_bind_int(st.s, 3, e.number);
        bindText(st.s, 4, e.name);
        bindText(st.s, 5, e.summary);
        bindText(st.s, 6, e.img);
        sqlite3_bind_int(st.s, 7, e.runtime);
        bindText(st.s, 8, e.airdate);
        sqlite3_step(st.s);
    }
    exec("COMMIT");
}

std::vector<Episode> MediaDb::episodes(const std::string& showKey) {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Episode> r;
    if (!db_) return r;
    Stmt st(db_, "SELECT season,num,name,summary,img,runtime,airdate FROM episodes WHERE show=? ORDER BY season,num");
    if (!st.s) return r;
    bindText(st.s, 1, showKey);
    while (sqlite3_step(st.s) == SQLITE_ROW) {
        Episode e;
        e.season = sqlite3_column_int(st.s, 0);
        e.number = sqlite3_column_int(st.s, 1);
        e.name = col(st.s, 2);
        e.summary = col(st.s, 3);
        e.img = col(st.s, 4);
        e.runtime = sqlite3_column_int(st.s, 5);
        e.airdate = col(st.s, 6);
        r.push_back(e);
    }
    return r;
}

Progress MediaDb::progress(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_);
    Progress p;
    if (!db_) return p;
    Stmt st(db_, "SELECT pos,dur,watched,last FROM progress WHERE path=?");
    if (!st.s) return p;
    bindText(st.s, 1, path);
    if (sqlite3_step(st.s) == SQLITE_ROW) {
        p.pos = sqlite3_column_double(st.s, 0);
        p.dur = sqlite3_column_double(st.s, 1);
        p.watched = sqlite3_column_int(st.s, 2) != 0;
        p.last = sqlite3_column_int64(st.s, 3);
        p.any = true;
    }
    return p;
}

void MediaDb::setProgress(const std::string& path, double pos, double dur, bool watched) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return;
    Stmt st(db_, "INSERT OR REPLACE INTO progress VALUES(?,?,?,?,?)");
    if (!st.s) return;
    bindText(st.s, 1, path);
    sqlite3_bind_double(st.s, 2, pos);
    sqlite3_bind_double(st.s, 3, dur);
    sqlite3_bind_int(st.s, 4, watched ? 1 : 0);
    sqlite3_bind_int64(st.s, 5, static_cast<sqlite3_int64>(std::time(nullptr)));
    sqlite3_step(st.s);
}

void MediaDb::markWatched(const std::string& path, bool watched) {
    Progress p = progress(path);
    setProgress(path, watched ? 0 : 0, p.dur, watched);
}

std::map<std::string, Progress> MediaDb::allProgress() {
    std::lock_guard<std::mutex> lk(m_);
    std::map<std::string, Progress> r;
    if (!db_) return r;
    Stmt st(db_, "SELECT path,pos,dur,watched,last FROM progress");
    if (!st.s) return r;
    while (sqlite3_step(st.s) == SQLITE_ROW) {
        Progress p;
        p.pos = sqlite3_column_double(st.s, 1);
        p.dur = sqlite3_column_double(st.s, 2);
        p.watched = sqlite3_column_int(st.s, 3) != 0;
        p.last = sqlite3_column_int64(st.s, 4);
        p.any = true;
        r[col(st.s, 0)] = p;
    }
    return r;
}

double MediaDb::duration(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return 0;
    Stmt st(db_, "SELECT dur FROM files WHERE path=?");
    if (!st.s) return 0;
    bindText(st.s, 1, path);
    return sqlite3_step(st.s) == SQLITE_ROW ? sqlite3_column_double(st.s, 0) : 0;
}

void MediaDb::setDuration(const std::string& path, double seconds) {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return;
    Stmt st(db_, "INSERT OR REPLACE INTO files VALUES(?,?)");
    if (!st.s) return;
    bindText(st.s, 1, path);
    sqlite3_bind_double(st.s, 2, seconds);
    sqlite3_step(st.s);
}

}  // namespace sf

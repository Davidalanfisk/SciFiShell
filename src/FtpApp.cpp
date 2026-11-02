#include "FtpApp.h"

#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#endif

namespace sf {

namespace fs = std::filesystem;

namespace {
#ifdef _WIN32
std::wstring wide(const std::string& s) { return fs::u8path(s).wstring(); }
std::string narrow(const std::wstring& w) { return fs::path(w).u8string(); }
#endif
}  // namespace

FtpApp::FtpApp(Renderer& renderer) : PanelApp(renderer, 1480, 1100, 1.9f) {}

FtpApp::~FtpApp() {
    if (worker_.joinable()) worker_.join();
#ifdef _WIN32
    if (ftp_) InternetCloseHandle(static_cast<HINTERNET>(ftp_));
    if (net_) InternetCloseHandle(static_cast<HINTERNET>(net_));
#endif
}

void FtpApp::setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    status_ = s;
    dirtyFromWorker_ = true;
}

void FtpApp::run(std::function<void()> job) {
    if (busy_) {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = "Still working - wait for the current transfer";
        return;
    }
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    worker_ = std::thread([this, job = std::move(job)] {
        job();
        busy_ = false;
        std::lock_guard<std::mutex> lk(mu_);
        dirtyFromWorker_ = true;
    });
}

void FtpApp::tick(double) {
    bool d = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        d = dirtyFromWorker_;
        dirtyFromWorker_ = false;
    }
    if (d) invalidate();
}

#ifdef _WIN32
void FtpApp::connect() {
    std::string host = host_, user = user_.empty() ? "anonymous" : user_, pass = user_.empty() && pass_.empty() ? "guest@example.com" : pass_;
    if (host.rfind("ftp://", 0) == 0) host = host.substr(6);
    while (!host.empty() && host.back() == '/') host.pop_back();
    int port = INTERNET_DEFAULT_FTP_PORT;
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
        port = std::atoi(host.c_str() + colon + 1);
        host.resize(colon);
    }
    if (host.empty()) {
        setStatus("Type a server name first");
        return;
    }
    setStatus("Connecting to " + host + " ...");
    run([this, host, user, pass, port] {
        if (ftp_) { InternetCloseHandle(static_cast<HINTERNET>(ftp_)); ftp_ = nullptr; }
        if (net_) { InternetCloseHandle(static_cast<HINTERNET>(net_)); net_ = nullptr; }
        HINTERNET net = InternetOpenW(L"SciFiShell", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
        if (!net) { setStatus("Could not start the network stack"); return; }
        DWORD to = 8000;
        InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
        InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
        InternetSetOptionW(net, INTERNET_OPTION_SEND_TIMEOUT, &to, sizeof(to));
        HINTERNET ftp = InternetConnectW(net, wide(host).c_str(), static_cast<INTERNET_PORT>(port), wide(user).c_str(), wide(pass).c_str(), INTERNET_SERVICE_FTP, INTERNET_FLAG_PASSIVE, 0);
        if (!ftp) {
            const DWORD e = GetLastError();
            InternetCloseHandle(net);
            setStatus(e == ERROR_INTERNET_LOGIN_FAILURE ? "Login refused by the server" : "Could not connect to " + host + " (error " + std::to_string(e) + ")");
            return;
        }
        net_ = net;
        ftp_ = ftp;
        {
            std::lock_guard<std::mutex> lk(mu_);
            connected_ = true;
            status_ = "Connected to " + host;
        }
        listing("");
    });
}

void FtpApp::disconnect() {
    if (busy_) return;
    if (worker_.joinable()) worker_.join();
    if (ftp_) InternetCloseHandle(static_cast<HINTERNET>(ftp_));
    if (net_) InternetCloseHandle(static_cast<HINTERNET>(net_));
    ftp_ = net_ = nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    connected_ = false;
    entries_.clear();
    sel_ = -1;
    cwd_ = "/";
    status_ = "Disconnected";
}

// Runs on the worker thread.
void FtpApp::listing(const std::string& cd) {
    HINTERNET ftp = static_cast<HINTERNET>(ftp_);
    if (!ftp) return;
    if (!cd.empty() && !FtpSetCurrentDirectoryW(ftp, wide(cd).c_str())) {
        setStatus("Could not open that folder");
        return;
    }
    std::vector<Entry> out;
    WIN32_FIND_DATAW fd{};
    HINTERNET h = FtpFindFirstFileW(ftp, L"*", &fd, INTERNET_FLAG_RELOAD, 0);
    if (h) {
        do {
            const std::string n = narrow(fd.cFileName);
            if (n == "." || n == "..") continue;
            Entry e;
            e.name = n;
            e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            out.push_back(std::move(e));
        } while (InternetFindNextFileW(h, &fd));
        InternetCloseHandle(h);
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return a.name < b.name;
    });
    wchar_t buf[1024] = L"/";
    DWORD len = 1024;
    FtpGetCurrentDirectoryW(ftp, buf, &len);
    std::lock_guard<std::mutex> lk(mu_);
    entries_ = std::move(out);
    cwd_ = narrow(buf);
    sel_ = -1;
    scroll_ = 0;
    status_ = std::to_string(entries_.size()) + " items in " + cwd_;
}

void FtpApp::download(const Entry& e) {
    if (e.dir) return;
    const char* up = std::getenv("USERPROFILE");
    fs::path dir = fs::path(up ? up : ".") / "Downloads";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path dest = dir / fs::u8path(e.name);
    setStatus("Downloading " + e.name + " ...");
    run([this, name = e.name, dest] {
        const BOOL ok = FtpGetFileW(static_cast<HINTERNET>(ftp_), wide(name).c_str(), dest.c_str(), FALSE, FILE_ATTRIBUTE_NORMAL, FTP_TRANSFER_TYPE_BINARY | INTERNET_FLAG_RELOAD, 0);
        setStatus(ok ? "Downloaded to " + dest.u8string() : "Download failed (error " + std::to_string(GetLastError()) + ")");
    });
}

void FtpApp::upload(const std::string& localPath) {
    if (!connected_) {
        setStatus("Connect to a server first");
        return;
    }
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(localPath), ec)) {
        setStatus("Pick a file to upload");
        return;
    }
    const std::string name = fs::u8path(localPath).filename().u8string();
    setStatus("Uploading " + name + " ...");
    run([this, localPath, name] {
        const BOOL ok = FtpPutFileW(static_cast<HINTERNET>(ftp_), fs::u8path(localPath).c_str(), wide(name).c_str(), FTP_TRANSFER_TYPE_BINARY, 0);
        if (ok) {
            listing("");
            setStatus("Uploaded " + name);
        } else setStatus("Upload failed (error " + std::to_string(GetLastError()) + ")");
    });
}
#else
void FtpApp::connect() { setStatus("FTP is only available on Windows"); }
void FtpApp::disconnect() {}
void FtpApp::listing(const std::string&) {}
void FtpApp::download(const Entry&) {}
void FtpApp::upload(const std::string&) { setStatus("FTP is only available on Windows"); }
#endif

void FtpApp::act(int id) {
    if (id >= 1 && id <= 3) focus_ = id - 1;
    else if (id == 4) connect();
    else if (id == 5) disconnect();
    else if (id == 6 && connected_) run([this] { listing(""); });
    else if (id == 7 && connected_) run([this] { listing(".."); });
    else if (id == 8 && connected_) {
        Entry e;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (sel_ >= 0 && sel_ < static_cast<int>(entries_.size())) { e = entries_[sel_]; ok = true; }
        }
        if (!ok || e.dir) setStatus("Select a file to download");
        else download(e);
    } else if (id == 9 && connected_) {
        if (requestUpload) requestUpload();
    }
}

bool FtpApp::onHover(float x, float y) {
    int h = -1;
    for (size_t i = 0; i < hits_.size(); ++i)
        if (hits_[i].r.contains(x, y)) h = static_cast<int>(i);
    if (h == hover_) return false;
    hover_ = h;
    return true;
}

void FtpApp::onClick(float x, float y, bool dbl) {
    for (const Hit& h : hits_)
        if (h.r.contains(x, y)) {
            act(h.id);
            return;
        }
    focus_ = -1;
    if (!list_.contains(x, y) || !connected_) return;
    const int row = static_cast<int>((y - list_.y0) / rowH_ + scroll_);
    Entry e;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (row < 0 || row >= static_cast<int>(entries_.size())) return;
        sel_ = row;
        e = entries_[row];
    }
    if (dbl) {
        if (e.dir) run([this, n = e.name] { listing(n); });
        else download(e);
    }
}

void FtpApp::onWheel(float x, float y, float notches) {
    if (!list_.contains(x, y)) return;
    std::lock_guard<std::mutex> lk(mu_);
    const float maxS = std::max(0.0f, static_cast<float>(entries_.size()) - list_.h() / rowH_);
    scroll_ = std::clamp(scroll_ - notches * 3.0f, 0.0f, maxS);
}

void FtpApp::onCtrl(const std::string& s) {
    if (s != "esc") return;
    if (focus_ >= 0) focus_ = -1;
    else close();
}

void FtpApp::onText(const std::string& s) {
    if (focus_ >= 0) field(focus_) += s;
}

void FtpApp::onKey(EditKey k) {
    if (focus_ < 0) return;
    std::string& f = field(focus_);
    if (k == EditKey::Backspace) {
        while (!f.empty() && (f.back() & 0xC0) == 0x80) f.pop_back();
        if (!f.empty()) f.pop_back();
    } else if (k == EditKey::Tab) focus_ = (focus_ + 1) % 3;
    else if (k == EditKey::Enter) {
        focus_ = -1;
        connect();
    } else if (k == EditKey::Paste) {
        if (char* t = SDL_GetClipboardText()) {
            for (const char* c = t; *c; ++c)
                if (static_cast<unsigned char>(*c) >= 0x20) f += *c;
            SDL_free(t);
        }
    }
}

void FtpApp::paint(Bitmap& bm) {
    using namespace ui;
    const Tokens t = tokens();
    std::vector<Entry> entries;
    std::string cwd, status;
    bool connected;
    int sel;
    float scroll;
    {
        std::lock_guard<std::mutex> lk(mu_);
        entries = entries_;
        cwd = cwd_;
        status = status_;
        connected = connected_;
        sel = sel_;
        scroll = scroll_;
    }
    const Rect c = panel(bm, "FTP", connected ? host_ : "NOT CONNECTED");
    hits_.clear();
    auto hot = [&](const Rect& r, int id) {
        hits_.push_back({r, id});
        return hover_ == static_cast<int>(hits_.size()) - 1 ? State::Hover : State::Normal;
    };

    float y = c.y0 + 4;
    const bool caret = (SDL_GetTicks() / 500) % 2 == 0;
    struct F {
        const char* l;
        float w;
    };
    float x = c.x0 + 4;
    int i = 0;
    for (F f : {F{"HOST", 430}, F{"USER", 260}, F{"PASSWORD", 260}}) {
        const Rect r{x, y, x + f.w, y + 52};
        hot(r, 1 + i);
        card(bm, r, focus_ == i ? State::Selected : State::Normal);
        std::string v = field(i);
        if (i == 2) v.assign(v.size(), '*');
        label(bm, r.x0 + 12, r.y0 + 20, f.l, 14, FontFace::UIBold, t.accent);
        label(bm, r.x0 + 12, r.y0 + 44, ellipsize(v, 22, FontFace::Mono, f.w - 24) + (focus_ == i && caret ? "|" : ""), 22, FontFace::Mono, t.text);
        x += f.w + 8;
        ++i;
    }
    const Rect cb{x, y, c.x1 - 4, y + 52};
    if (connected) button(bm, cb, "DISCONNECT", busy_ ? State::Disabled : hot(cb, 5), false, 20);
    else button(bm, cb, "CONNECT", busy_ ? State::Disabled : hot(cb, 4), true, 20);

    y += 62;
    x = c.x0 + 4;
    const Rect pr{x, y, x + 600, y + 46};
    card(bm, pr, State::Normal);
    label(bm, pr.x0 + 14, pr.y0 + 31, ellipsize(connected ? cwd : "-", 22, FontFace::Mono, pr.w() - 28), 22, FontFace::Mono, t.text);
    x = pr.x1 + 8;
    struct Bt {
        const char* l;
        int id;
        float w;
    };
    for (Bt b : {Bt{"UP", 7, 80}, Bt{"REFRESH", 6, 140}, Bt{"DOWNLOAD", 8, 170}, Bt{"UPLOAD", 9, 140}}) {
        const Rect r{x, y, x + b.w, y + 46};
        button(bm, r, b.l, connected && !busy_ ? hot(r, b.id) : State::Disabled, false, 20);
        x += b.w + 8;
    }

    y += 56;
    list_ = Rect{c.x0 + 4, y + 36, c.x1 - 4, c.y1 - 44};
    card(bm, Rect{c.x0 + 4, y, c.x1 - 4, c.y1 - 40}, State::Normal);
    label(bm, c.x0 + 24, y + 26, "NAME", 18, FontFace::UIBold, t.accent);
    labelRight(bm, c.x1 - 24, y + 26, "SIZE", 18, FontFace::UIBold, t.accent);
    const int vis = static_cast<int>(list_.h() / rowH_);
    for (int r = 0; r < vis; ++r) {
        const int idx = static_cast<int>(scroll) + r;
        if (idx >= static_cast<int>(entries.size())) break;
        const Entry& e = entries[idx];
        const float ry = list_.y0 + r * rowH_;
        if (idx == sel) fillRect(bm, list_.x0 + 2, ry, list_.x1 - 2, ry + rowH_ - 2, withAlpha(t.accent, 50));
        label(bm, list_.x0 + 20, ry + 28, e.dir ? "[+]" : "   ", 22, FontFace::Mono, t.accent);
        label(bm, list_.x0 + 80, ry + 28, ellipsize(e.name, 22, FontFace::UI, list_.w() - 360), 22, FontFace::UI, e.dir ? t.accent : t.text);
        labelRight(bm, list_.x1 - 20, ry + 28, e.dir ? "folder" : bytesText(e.size), 20, FontFace::UI, t.dim);
    }
    if (!connected) label(bm, list_.x0 + 20, list_.y0 + 40, "Not connected", 24, FontFace::UI, t.muted);
    else if (entries.empty()) label(bm, list_.x0 + 20, list_.y0 + 40, "This folder is empty", 24, FontFace::UI, t.muted);

    label(bm, c.x0 + 8, c.y1 - 12, ellipsize(status, 20, FontFace::UI, c.w() - 200), 20, FontFace::UI, busy_ ? t.accent : t.dim);
    if (connected) labelRight(bm, c.x1 - 8, c.y1 - 12, "Double-click: open folder / download", 18, FontFace::UI, t.muted);
}

}  // namespace sf

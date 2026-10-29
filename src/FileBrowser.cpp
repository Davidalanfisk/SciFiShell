#include "FileBrowser.h"

#include <windows.h>

#include <shellapi.h>

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "OsSettings.h"
#include "Thumbnails.h"

namespace sf {

namespace fs = std::filesystem;

namespace {
std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring toWide(const std::string& s) { return fs::u8path(s).wstring(); }
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string dateText(unsigned long long stamp) {
    FILETIME ft, lt;
    ft.dwLowDateTime = static_cast<DWORD>(stamp & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(stamp >> 32);
    SYSTEMTIME st;
    if (!stamp || !FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &st)) return "";
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d  %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}
std::wstring doubleNull(const std::vector<std::string>& paths) {
    std::wstring w;
    for (const auto& p : paths) {
        w += toWide(p);
        w += L'\0';
    }
    w += L'\0';
    return w;
}
bool popUtf8(std::string& s) {
    if (s.empty()) return false;
    while (!s.empty() && (s.back() & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
    return true;
}
}  // namespace

FileBrowser::FileBrowser(Renderer& renderer) : PanelApp(renderer, 1600, 900, 2.2f) {
    const char* up = std::getenv("USERPROFILE");
    const std::string home = up ? up : "C:\\";
    places_.push_back({"HOME", home});
    for (const char* n : {"Desktop", "Documents", "Downloads", "Pictures", "Music", "Videos"}) {
        const std::string p = home + "\\" + n;
        std::error_code ec;
        if (fs::is_directory(fs::u8path(p), ec)) places_.push_back({lower(n), p});
    }
    for (auto& pl : places_) {
        for (char& c : pl.first) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    navigate(home, false);
}

void FileBrowser::onOpen() { reload(); }

void FileBrowser::navigate(const std::string& path, bool remember) {
    if (remember && (history_.empty() || history_.back() != cwd_)) history_.push_back(cwd_);
    cwd_ = path;
    sel_.clear();
    anchor_ = -1;
    scroll_ = 0;
    search_.clear();
    edit_ = Edit::None;
    deleteArmed_ = 0;
    reload();
}

void FileBrowser::reload() {
    all_.clear();
    if (cwd_.empty()) {
        for (const auto& [n, info] : os::drives()) {
            Entry e;
            e.name = n + "  " + info;
            e.path = n + "\\";
            e.dir = true;
            all_.push_back(std::move(e));
        }
    } else {
        std::wstring pat = toWide(cwd_);
        if (!pat.empty() && pat.back() != L'\\') pat += L'\\';
        pat += L'*';
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring n = fd.cFileName;
                if (n == L"." || n == L"..") continue;
                if (!showHidden_ && (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
                Entry e;
                e.name = toUtf8(n);
                std::string base = cwd_;
                if (!base.empty() && base.back() != '\\') base += '\\';
                e.path = base + e.name;
                e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                e.stamp = (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                e.date = dateText(e.stamp);
                e.kind = static_cast<int>(classifyFile(e.path, e.dir));
                all_.push_back(std::move(e));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        } else {
            status_ = "Cannot open this folder";
        }
    }
    sortEntries();
    applyFilter();
    invalidate();
}

void FileBrowser::sortEntries() {
    std::stable_sort(all_.begin(), all_.end(), [&](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        int c = 0;
        if (sortCol_ == 1) c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        else if (sortCol_ == 2) c = a.stamp < b.stamp ? -1 : (a.stamp > b.stamp ? 1 : 0);
        if (c == 0) c = lower(a.name).compare(lower(b.name));
        return sortAsc_ ? c < 0 : c > 0;
    });
}

void FileBrowser::applyFilter() {
    view_.clear();
    const std::string q = lower(search_);
    for (const Entry& e : all_)
        if (q.empty() || lower(e.name).find(q) != std::string::npos) view_.push_back(e);
    sel_.clear();
    const float maxScroll = std::max(0.0f, static_cast<float>(view_.size()) - 1);
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
}

std::vector<std::string> FileBrowser::selectedPaths() const {
    std::vector<std::string> out;
    for (int i : sel_)
        if (i >= 0 && i < static_cast<int>(view_.size())) out.push_back(view_[i].path);
    return out;
}

bool FileBrowser::back() {
    if (edit_ != Edit::None) {
        edit_ = Edit::None;
        buf_.clear();
        invalidate();
        return true;
    }
    if (!search_.empty()) {
        search_.clear();
        applyFilter();
        return true;
    }
    return false;
}

void FileBrowser::startEdit(Edit e) {
    edit_ = e;
    deleteArmed_ = 0;
    if (e == Edit::Search) buf_ = search_;
    else if (e == Edit::NewFolder) buf_ = "New folder";
    else if (e == Edit::Rename) {
        if (sel_.size() != 1) {
            edit_ = Edit::None;
            status_ = "Select one item to rename";
            return;
        }
        buf_ = view_[*sel_.begin()].name;
    }
}

void FileBrowser::commitEdit() {
    const Edit e = edit_;
    edit_ = Edit::None;
    if (e == Edit::Search) {
        search_ = buf_;
        applyFilter();
        return;
    }
    const bool bad = buf_.empty() || buf_.find_first_of("\\/:*?\"<>|") != std::string::npos;
    if (bad) {
        status_ = "Invalid name";
        return;
    }
    std::error_code ec;
    std::string base = cwd_;
    if (!base.empty() && base.back() != '\\') base += '\\';
    if (e == Edit::NewFolder) {
        fs::create_directory(fs::u8path(base + buf_), ec);
        status_ = ec ? "Could not create the folder" : "Folder created";
    } else if (e == Edit::Rename && sel_.size() == 1) {
        const std::string from = view_[*sel_.begin()].path;
        fs::rename(fs::u8path(from), fs::u8path(base + buf_), ec);
        status_ = ec ? "Rename failed (name in use?)" : "Renamed";
    }
    const std::string keep = buf_;
    buf_.clear();
    reload();
    if (!ec)
        for (size_t i = 0; i < view_.size(); ++i)
            if (view_[i].name == keep) {
                sel_ = {static_cast<int>(i)};
                scroll_ = std::max(0.0f, static_cast<float>(i) - 3);
            }
}

void FileBrowser::fileOp(int op, const std::vector<std::string>& from, const std::string& to) {
    if (from.empty()) return;
    std::wstring src = doubleNull(from), dst = to.empty() ? L"" : toWide(to) + L'\0';
    SHFILEOPSTRUCTW s{};
    s.wFunc = op;
    s.pFrom = src.c_str();
    s.pTo = to.empty() ? nullptr : dst.c_str();
    s.fFlags = (op == FO_DELETE ? FOF_ALLOWUNDO | FOF_NOCONFIRMATION : FOF_RENAMEONCOLLISION | FOF_NOCONFIRMMKDIR) | FOF_NOERRORUI | FOF_SILENT;
    const int r = SHFileOperationW(&s);
    status_ = r == 0 && !s.fAnyOperationsAborted ? (op == FO_DELETE ? "Moved to the Recycle Bin" : "Done") : "Operation failed or was cancelled";
}

int FileBrowser::rowAt(float y) const {
    if (y < list_.y0 || y > list_.y1) return -1;
    const int r = static_cast<int>(scroll_) + static_cast<int>((y - list_.y0 + (scroll_ - std::floor(scroll_)) * rowH_) / rowH_);
    return r >= 0 && r < static_cast<int>(view_.size()) ? r : -1;
}

void FileBrowser::act(int id) {
    if (id != 10) deleteArmed_ = 0;
    if (id == 1) {  // back
        if (!history_.empty()) {
            const std::string p = history_.back();
            history_.pop_back();
            navigate(p, false);
        }
    } else if (id == 2) {  // up
        if (cwd_.empty()) return;
        fs::path p = fs::u8path(cwd_);
        if (p.has_relative_path() && p != p.root_path()) {
            fs::path parent = p.parent_path();
            if (parent.empty() || (p.root_path() == p)) navigate("");
            else navigate(parent.u8string());
        } else navigate("");
    } else if (id == 3) reload();
    else if (id == 4) startEdit(Edit::Search);
    else if (id == 5) startEdit(Edit::NewFolder);
    else if (id == 6) startEdit(Edit::Rename);
    else if (id == 7 || id == 8) {
        clip_ = selectedPaths();
        clipOp_ = id == 7 ? "copy" : "cut";
        status_ = clip_.empty() ? "Nothing selected" : (id == 7 ? "Copied" : "Cut") + std::string(" ") + std::to_string(clip_.size()) + " item(s)";
    } else if (id == 9) {
        if (clip_.empty() || cwd_.empty()) return;
        fileOp(clipOp_ == "cut" ? FO_MOVE : FO_COPY, clip_, cwd_);
        if (clipOp_ == "cut") clip_.clear();
        reload();
    } else if (id == 10) {
        const auto paths = selectedPaths();
        if (paths.empty()) { status_ = "Nothing selected"; return; }
        if (deleteArmed_ == 0) {
            deleteArmed_ = 1;
            status_ = "Click DELETE again to move " + std::to_string(paths.size()) + " item(s) to the Recycle Bin";
        } else {
            deleteArmed_ = 0;
            fileOp(FO_DELETE, paths, "");
            reload();
        }
    } else if (id == 11) {
        if (openRoom) openRoom(cwd_);
    } else if (id == 12) {
        showHidden_ = !showHidden_;
        reload();
    } else if (id >= 100 && id < 200) {
        const int i = id - 100;
        if (i < static_cast<int>(places_.size())) navigate(places_[i].second);
    } else if (id >= 200 && id < 300) {
        // breadcrumb: id = 200 + segment index; rebuild the path of that segment
        std::vector<std::string> segs;
        std::string cur;
        for (char ch : cwd_) {
            if (ch == '\\') { if (!cur.empty()) segs.push_back(cur); cur.clear(); }
            else cur += ch;
        }
        if (!cur.empty()) segs.push_back(cur);
        const int upTo = id - 200;
        if (upTo == 0 && segs.empty()) return;
        std::string p;
        for (int i = 0; i <= upTo && i < static_cast<int>(segs.size()); ++i) p += segs[i] + "\\";
        navigate(p);
    } else if (id >= 300 && id < 303) {
        const int col = id - 300;
        if (sortCol_ == col) sortAsc_ = !sortAsc_;
        else { sortCol_ = col; sortAsc_ = true; }
        sortEntries();
        applyFilter();
    } else if (id == 500) {
        navigate("");
    } else if (id >= 400) {  // drives
        const auto d = os::drives();
        const size_t i = static_cast<size_t>(id - 400);
        if (i < d.size()) navigate(d[i].first + "\\");
    }
}

bool FileBrowser::onHover(float x, float y) {
    int hr = rowAt(y);
    if (!list_.contains(x, y)) hr = -1;
    int hh = -1;
    for (size_t i = 0; i < hits_.size(); ++i)
        if (hits_[i].r.contains(x, y)) hh = static_cast<int>(i);
    if (hr == hoverRow_ && hh == hoverHit_) return false;
    hoverRow_ = hr;
    hoverHit_ = hh;
    return true;
}

void FileBrowser::onWheel(float, float, float notches) {
    const float maxScroll = std::max(0.0f, static_cast<float>(view_.size()) - list_.h() / rowH_ + 1);
    scroll_ = std::clamp(scroll_ - notches * 3.0f, 0.0f, maxScroll);
}

void FileBrowser::onClick(float x, float y, bool dbl) {
    for (const Hit& h : hits_)
        if (h.r.contains(x, y)) {
            if (edit_ != Edit::None && h.id != 4) { edit_ = Edit::None; buf_.clear(); }
            act(h.id);
            return;
        }
    if (list_.contains(x, y)) {
        if (edit_ != Edit::None) { edit_ = Edit::None; buf_.clear(); }
        deleteArmed_ = 0;
        const int r = rowAt(y);
        if (r < 0) { sel_.clear(); return; }
        const Uint16 mod = SDL_GetModState();
        if (dbl) {
            const Entry& e = view_[r];
            if (e.dir) navigate(e.path);
            else if (pickHandler) { auto h = std::move(pickHandler); pickHandler = nullptr; h(e.path); }
            else if (openFile) openFile(e.path);
            return;
        }
        if ((mod & KMOD_SHIFT) && anchor_ >= 0) {
            sel_.clear();
            for (int i = std::min(anchor_, r); i <= std::max(anchor_, r); ++i) sel_.insert(i);
        } else if (mod & KMOD_CTRL) {
            if (!sel_.erase(r)) sel_.insert(r);
            anchor_ = r;
        } else {
            sel_ = {r};
            anchor_ = r;
        }
    }
}

void FileBrowser::onKey(EditKey k) {
    if (edit_ == Edit::None) return;
    if (k == EditKey::Backspace) popUtf8(buf_);
    else if (k == EditKey::Enter) commitEdit();
    else if (k == EditKey::Paste) {
        if (char* t = SDL_GetClipboardText()) {
            std::string s = t;
            SDL_free(t);
            for (char c : s)
                if (c >= 0x20) buf_ += c;
        }
    }
    if (edit_ == Edit::Search) {  // live filtering
        search_ = buf_;
        applyFilter();
    }
}

void FileBrowser::onText(const std::string& s) {
    if (edit_ == Edit::None) return;
    buf_ += s;
    if (edit_ == Edit::Search) {
        search_ = buf_;
        applyFilter();
    }
}

void FileBrowser::onCtrl(const std::string& s) {
    if (s == "esc") back();
}

void FileBrowser::paint(Bitmap& bm) {
    using namespace ui;
    const Tokens t = tokens();
    Rect c = panel(bm, "FILES", cwd_.empty() ? "THIS PC" : cwd_);
    hits_.clear();
    auto hot = [&](const Rect& r, int id) {
        hits_.push_back({r, id});
        return hoverHit_ == static_cast<int>(hits_.size()) - 1 ? State::Hover : State::Normal;
    };

    // toolbar
    const float ty = c.y0 + 10, th = 40;
    float x = c.x0 + 16;
    struct B { const char* l; int id; float w; };
    for (B b : {B{"BACK", 1, 90}, B{"UP", 2, 70}, B{"REFRESH", 3, 120}}) {
        Rect r{x, ty, x + b.w, ty + th};
        button(bm, r, b.l, hot(r, b.id), false, 22);
        x += b.w + 8;
    }
    const float searchW = 360;
    Rect sr{c.x1 - 16 - searchW, ty, c.x1 - 16, ty + th};
    Rect pr{x + 8, ty, sr.x0 - 16, ty + th};
    card(bm, pr, State::Normal);
    {
        // breadcrumbs
        std::vector<std::string> segs;
        std::string cur;
        for (char ch : cwd_) {
            if (ch == '\\') { if (!cur.empty()) segs.push_back(cur); cur.clear(); }
            else cur += ch;
        }
        if (!cur.empty()) segs.push_back(cur);
        if (segs.empty()) label(bm, pr.x0 + 14, pr.y0 + 28, "THIS PC", 22, FontFace::UIBold, t.text);
        float total = 0;
        for (auto& s : segs) total += measureText(s, 22, FontFace::UIBold) + 34;
        size_t start = 0;
        while (start + 1 < segs.size() && total > pr.w() - 40) {
            total -= measureText(segs[start], 22, FontFace::UIBold) + 34;
            ++start;
        }
        float bx = pr.x0 + 14;
        if (start > 0) { label(bm, bx, pr.y0 + 28, "...", 22, FontFace::UIBold, t.muted); bx += 34; }
        for (size_t i = start; i < segs.size(); ++i) {
            const float w = measureText(segs[i], 22, FontFace::UIBold);
            Rect r{bx - 4, pr.y0 + 4, bx + w + 4, pr.y1 - 4};
            const bool h = hot(r, 200 + static_cast<int>(i)) == State::Hover;
            label(bm, bx, pr.y0 + 28, segs[i], 22, FontFace::UIBold, h ? t.accent : t.text);
            bx += w + 10;
            if (i + 1 < segs.size()) { label(bm, bx, pr.y0 + 28, ">", 22, FontFace::UI, t.muted); bx += 24; }
        }
    }
    {
        const bool on = edit_ == Edit::Search;
        card(bm, sr, on ? State::Selected : hot(sr, 4));
        if (on) hits_.push_back({sr, 4});
        const bool caret = on && (SDL_GetTicks() / 500) % 2 == 0;
        std::string shown = on ? buf_ : search_;
        if (shown.empty() && !on) label(bm, sr.x0 + 14, sr.y0 + 28, "SEARCH THIS FOLDER", 20, FontFace::UI, t.muted);
        else label(bm, sr.x0 + 14, sr.y0 + 28, ellipsize(shown, 22, FontFace::UI, sr.w() - 40) + (caret ? "|" : ""), 22, FontFace::UI, t.text);
    }

    // sidebar
    const float sbW = 250, top = ty + th + 14, bottom = c.y1 - 112;
    Rect sb{c.x0 + 16, top, c.x0 + 16 + sbW, bottom};
    card(bm, sb, State::Normal);
    float sy = sb.y0 + 34;
    label(bm, sb.x0 + 16, sy, "PLACES", 18, FontFace::UIBold, t.muted);
    sy += 12;
    for (size_t i = 0; i < places_.size(); ++i) {
        Rect r{sb.x0 + 8, sy, sb.x1 - 8, sy + 32};
        const bool cur = places_[i].second == cwd_;
        State s = hot(r, 100 + static_cast<int>(i));
        if (cur) s = State::Selected;
        if (s != State::Normal) fillRound(bm, r.x0, r.y0, r.x1, r.y1, 4, withAlpha(t.accent, cur ? 60 : 30));
        label(bm, r.x0 + 12, r.y0 + 23, places_[i].first, 20, FontFace::UI, cur ? t.accent : t.text);
        sy += 34;
    }
    sy += 14;
    label(bm, sb.x0 + 16, sy + 20, "DRIVES", 18, FontFace::UIBold, t.muted);
    sy += 28;
    const auto drives = os::drives();
    for (size_t i = 0; i < drives.size() && sy < sb.y1 - 36; ++i) {
        Rect r{sb.x0 + 8, sy, sb.x1 - 8, sy + 32};
        const bool cur = cwd_.size() >= 2 && lower(cwd_.substr(0, 2)) == lower(drives[i].first);
        State s = hot(r, 400 + static_cast<int>(i));
        if (cur) s = State::Selected;
        if (s != State::Normal) fillRound(bm, r.x0, r.y0, r.x1, r.y1, 4, withAlpha(t.accent, cur ? 60 : 30));
        label(bm, r.x0 + 12, r.y0 + 23, drives[i].first, 20, FontFace::UIBold, cur ? t.accent : t.text);
        sy += 34;
    }
    {
        Rect r{sb.x0 + 8, sb.y1 - 40, sb.x1 - 8, sb.y1 - 8};
        const State s = hot(r, 500);
        if (s != State::Normal || cwd_.empty()) fillRound(bm, r.x0, r.y0, r.x1, r.y1, 4, withAlpha(t.accent, cwd_.empty() ? 60 : 30));
        label(bm, r.x0 + 12, r.y0 + 23, "THIS PC", 20, FontFace::UI, cwd_.empty() ? t.accent : t.text);
    }

    // list
    Rect lp{sb.x1 + 16, top, c.x1 - 16, bottom};
    card(bm, lp, State::Normal);
    const float colSize = lp.x1 - 400, colDate = lp.x1 - 250;
    const float hy = lp.y0 + 8;
    struct Col { const char* l; float x; int id; };
    for (Col cl : {Col{"NAME", lp.x0 + 52, 300}, Col{"SIZE", colSize, 301}, Col{"MODIFIED", colDate, 302}}) {
        Rect r{cl.x - 6, hy, cl.x + 110, hy + 30};
        hot(r, cl.id);
        std::string txt = cl.l;
        if (sortCol_ == cl.id - 300) txt += sortAsc_ ? "  ^" : "  v";
        label(bm, cl.x, hy + 22, txt, 18, FontFace::UIBold, sortCol_ == cl.id - 300 ? t.accent : t.muted);
    }
    list_ = {lp.x0 + 6, hy + 36, lp.x1 - 6, lp.y1 - 6};
    line(bm, lp.x0 + 10, hy + 33, lp.x1 - 10, hy + 33, 1, withAlpha(t.border, 160));
    const int first = static_cast<int>(scroll_);
    const float frac = (scroll_ - first) * rowH_;
    const int visible = static_cast<int>(list_.h() / rowH_) + 2;
    for (int i = 0; i < visible; ++i) {
        const int idx = first + i;
        if (idx >= static_cast<int>(view_.size())) break;
        const Entry& e = view_[idx];
        const float y0 = list_.y0 + i * rowH_ - frac;
        if (y0 < list_.y0 - 1 || y0 + rowH_ > list_.y1 + rowH_ * 0.5f) continue;
        const bool selected = sel_.count(idx) > 0;
        if (selected) fillRound(bm, list_.x0, y0, list_.x1, y0 + rowH_ - 2, 4, withAlpha(t.accent, 70));
        else if (idx == hoverRow_) fillRound(bm, list_.x0, y0, list_.x1, y0 + rowH_ - 2, 4, withAlpha(t.accent, 28));
        Color8 kc = e.dir ? t.warn : (e.kind == 1 ? t.alt : (e.kind == 2 ? t.danger : (e.kind == 3 ? t.success : (e.kind == 4 ? t.accent : t.muted))));
        fillRound(bm, list_.x0 + 12, y0 + 7, list_.x0 + 34, y0 + 25, 3, withAlpha(kc, 200));
        if (e.dir) fillRect(bm, list_.x0 + 12, y0 + 5, list_.x0 + 22, y0 + 9, withAlpha(kc, 200));
        label(bm, list_.x0 + 46, y0 + 24, ellipsize(e.name, 22, e.dir ? FontFace::UIBold : FontFace::UI, colSize - list_.x0 - 70), 22, e.dir ? FontFace::UIBold : FontFace::UI, selected ? t.text : (e.dir ? t.text : t.dim));
        if (!e.dir) labelRight(bm, colSize + 90, y0 + 24, ui::bytesText(e.size), 20, FontFace::Mono, t.dim);
        label(bm, colDate, y0 + 24, e.date, 20, FontFace::Mono, t.muted);
    }
    if (view_.empty()) label(bm, list_.x0 + 20, list_.y0 + 40, search_.empty() ? "This folder is empty" : "Nothing matches the search", 24, FontFace::UI, t.muted);
    if (static_cast<float>(view_.size()) * rowH_ > list_.h()) {  // scroll bar
        const float tot = static_cast<float>(view_.size()) * rowH_, vh = list_.h();
        const float th2 = std::max(30.0f, vh * vh / tot), ty2 = list_.y0 + (vh - th2) * (scroll_ * rowH_ / (tot - vh + 1));
        fillRound(bm, lp.x1 - 9, ty2, lp.x1 - 5, ty2 + th2, 2, withAlpha(t.accent, 160));
    }

    // action bar
    const float ay = bottom + 14;
    if (edit_ == Edit::Rename || edit_ == Edit::NewFolder) {
        Rect r{c.x0 + 16, ay, c.x1 - 16, ay + 46};
        card(bm, r, State::Selected);
        const bool caret = (SDL_GetTicks() / 500) % 2 == 0;
        label(bm, r.x0 + 16, r.y0 + 31, edit_ == Edit::Rename ? "RENAME TO:" : "NEW FOLDER NAME:", 20, FontFace::UIBold, t.accent);
        label(bm, r.x0 + 230, r.y0 + 31, buf_ + (caret ? "|" : ""), 24, FontFace::UI, t.text);
        labelRight(bm, r.x1 - 16, r.y0 + 31, "ENTER  confirm     ESC  cancel", 18, FontFace::UI, t.muted);
    } else {
        struct A { const char* l; int id; float w; bool primary; };
        float ax = c.x0 + 16;
        const bool canPaste = !clip_.empty() && !cwd_.empty();
        for (A a : {A{"NEW FOLDER", 5, 190, false}, A{"RENAME", 6, 130, false}, A{"COPY", 7, 100, false}, A{"CUT", 8, 90, false},
                    A{"PASTE", 9, 110, false}, A{deleteArmed_ ? "CONFIRM DELETE" : "DELETE", 10, deleteArmed_ ? 210.0f : 120.0f, false}, A{"HIDDEN", 12, 120, false}}) {
            Rect r{ax, ay, ax + a.w, ay + 46};
            State s = hot(r, a.id);
            if (a.id == 9 && !canPaste) s = State::Disabled;
            if (a.id == 12 && showHidden_) s = State::Selected;
            if (a.id == 10 && deleteArmed_) s = State::Pressed;
            button(bm, r, a.l, s, false, 20);
            ax += a.w + 8;
        }
        Rect r{c.x1 - 16 - 260, ay, c.x1 - 16, ay + 46};
        button(bm, r, "OPEN IN 3D ROOM", cwd_.empty() ? State::Disabled : hot(r, 11), true, 20);
    }
    std::string st = pickHandler ? std::string("PICK A FILE: double-click the file to open it in the editor") : status_;
    if (st.empty()) st = std::to_string(view_.size()) + " items" + (sel_.empty() ? "" : "   |   " + std::to_string(sel_.size()) + " selected") + "   |   Double-click opens   |   Ctrl / Shift click selects several";
    label(bm, c.x0 + 20, c.y1 - 18, ellipsize(st, 20, FontFace::UI, c.w() - 40), 20, FontFace::UI, t.dim);
}

}  // namespace sf

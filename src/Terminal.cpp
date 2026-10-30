#include "Terminal.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace sf {

namespace {
constexpr size_t kMaxScrollback = 5000;

const Color8 kAnsi[16] = {
    {12, 14, 20, 255},    {205, 60, 70, 255},   {60, 190, 120, 255},  {220, 180, 70, 255},
    {70, 130, 230, 255},  {180, 90, 210, 255},  {60, 190, 200, 255},  {200, 206, 214, 255},
    {90, 100, 115, 255},  {245, 100, 110, 255}, {100, 235, 160, 255}, {250, 215, 110, 255},
    {110, 170, 255, 255}, {215, 130, 245, 255}, {110, 235, 245, 255}, {250, 252, 255, 255},
};

Color8 xterm256(int n) {
    if (n < 16) return kAnsi[std::clamp(n, 0, 15)];
    if (n < 232) {
        n -= 16;
        auto lv = [](int v) { return static_cast<uint8_t>(v ? 55 + v * 40 : 0); };
        return {lv(n / 36), lv((n / 6) % 6), lv(n % 6), 255};
    }
    const uint8_t g = static_cast<uint8_t>(8 + (n - 232) * 10);
    return {g, g, g, 255};
}

void appendUtf8(std::string& s, char32_t c) {
    if (c < 0x80) s += static_cast<char>(c);
    else if (c < 0x800) { s += static_cast<char>(0xC0 | (c >> 6)); s += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += static_cast<char>(0xE0 | (c >> 12)); s += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); s += static_cast<char>(0x80 | (c & 0x3F)); }
    else { s += static_cast<char>(0xF0 | (c >> 18)); s += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); s += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); s += static_cast<char>(0x80 | (c & 0x3F)); }
}

std::vector<int> nums(const std::string& p, int def) {
    std::vector<int> out;
    std::string cur;
    for (char ch : p + ";") {
        if (ch == ';' || ch == ':') {
            out.push_back(cur.empty() ? def : std::atoi(cur.c_str()));
            cur.clear();
        } else if (ch >= '0' && ch <= '9') cur += ch;
    }
    return out;
}
}  // namespace

Terminal::Terminal(Renderer& renderer) : PanelApp(renderer, 1600, 900, 2.2f) {
    title_ = "TERMINAL";
    cw_ = std::max(6.0f, measureText("M", 20.0f, FontFace::Mono));
    cols_ = std::max(40, static_cast<int>(1540.0f / cw_));
    rows_ = 30;
    cols0_ = cols_;
    rows0_ = rows_;
    bot_ = rows_ - 1;
    hasWsl_ = std::filesystem::exists("C:\\Windows\\System32\\wsl.exe");
    grid_.assign(rows_, blankRow());
}

Terminal::~Terminal() { stop(); }

Terminal::Row Terminal::blankRow() const { return Row(cols_); }

// Fullscreen gives the terminal more room: grow / shrink the character grid to fit and tell the pseudo console.
void Terminal::onResize(int w, int h) {
    int cols = cols0_, rows = rows0_;
    if (w != 1600 || h != 900) {
        cols = std::max(40, static_cast<int>((w - 72 - 40) / cw_));
        rows = std::max(10, static_cast<int>((h - 120 - 62 - 20) / lh_));
    }
    if (cols == cols_ && rows == rows_) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto fit = [&](std::vector<Row>& g) {
        for (Row& r : g) r.resize(cols);
        while (static_cast<int>(g.size()) > rows) g.erase(g.begin());
        while (static_cast<int>(g.size()) < rows) g.push_back(Row(cols));
    };
    const int drop = std::max(0, static_cast<int>(grid_.size()) - rows);
    fit(grid_);
    if (!saved_.empty()) fit(saved_);
    for (Row& r : scrollback_) r.resize(cols);
    cy_ = std::clamp(cy_ - drop, 0, rows - 1);
    cx_ = std::min(cx_, cols - 1);
    sx_ = std::min(sx_, cols - 1);
    sy_ = std::min(sy_, rows - 1);
    cols_ = cols;
    rows_ = rows;
    top_ = 0;
    bot_ = rows_ - 1;
    wrapPending_ = false;
    scrollOff_ = 0;
    if (pc_) ResizePseudoConsole(pc_, COORD{static_cast<SHORT>(cols_), static_cast<SHORT>(rows_)});
}

void Terminal::onOpen() {
    if (!alive_) start(shell_);
}

void Terminal::start(int shell) {
    stop();
    shell_ = shell;
    exitNote_.clear();
    grid_.assign(rows_, blankRow());
    scrollback_.clear();
    cx_ = cy_ = 0;
    top_ = 0;
    bot_ = rows_ - 1;
    pen_ = Cell{};
    alt_ = false;
    ps_ = P::Ground;
    scrollOff_ = 0;

    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, nullptr, 0) || !CreatePipe(&outR, &outW, nullptr, 0)) return;
    COORD size{static_cast<SHORT>(cols_), static_cast<SHORT>(rows_)};
    if (FAILED(CreatePseudoConsole(size, inR, outW, 0, &pc_))) {
        CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW);
        exitNote_ = "This version of Windows has no pseudo console support.";
        return;
    }
    CloseHandle(inR);
    CloseHandle(outW);
    in_ = inW;
    out_ = outR;

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<char> attr(bytes);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr.data());
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc_, sizeof pc_, nullptr, nullptr);

    std::wstring cmd = shell == 1 ? L"cmd.exe" : (shell == 2 ? L"wsl.exe" : L"powershell.exe -NoLogo");
    wchar_t home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                   home[0] ? home : nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) {
        exitNote_ = "Could not start the shell.";
        stop();
        return;
    }
    proc_ = pi.hProcess;
    thread_ = pi.hThread;
    alive_ = true;
    reader_ = std::thread([this, h = out_] {
        char buf[8192];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.append(buf, n);
        }
    });
}

void Terminal::stop() {
    alive_ = false;
    if (pc_) {
        ClosePseudoConsole(pc_);
        pc_ = nullptr;
    }
    if (proc_) {
        TerminateProcess(proc_, 0);
        CloseHandle(proc_);
        proc_ = nullptr;
    }
    if (thread_) {
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (in_) {
        CloseHandle(in_);
        in_ = nullptr;
    }
    if (reader_.joinable()) reader_.join();
    if (out_) {
        CloseHandle(out_);
        out_ = nullptr;
    }
}

void Terminal::send(const std::string& s) {
    if (!alive_ || !in_ || s.empty()) return;
    DWORD w = 0;
    WriteFile(in_, s.data(), static_cast<DWORD>(s.size()), &w, nullptr);
    scrollOff_ = 0;
}

// ---- screen model -------------------------------------------------------------------------------------------------

void Terminal::clearRow(Row& r, int from, int to) {
    for (int i = std::max(0, from); i < std::min<int>(to, static_cast<int>(r.size())); ++i) {
        r[i] = Cell{};
        r[i].bg = pen_.bg;
    }
}

void Terminal::scrollUp(int n) {
    for (int i = 0; i < n; ++i) {
        if (top_ == 0 && !alt_) {
            scrollback_.push_back(grid_[0]);
            if (scrollback_.size() > kMaxScrollback) scrollback_.pop_front();
        }
        grid_.erase(grid_.begin() + top_);
        grid_.insert(grid_.begin() + bot_, blankRow());
    }
}

void Terminal::scrollDown(int n) {
    for (int i = 0; i < n; ++i) {
        grid_.erase(grid_.begin() + bot_);
        grid_.insert(grid_.begin() + top_, blankRow());
    }
}

void Terminal::lineFeed() {
    if (cy_ == bot_) scrollUp(1);
    else if (cy_ < rows_ - 1) ++cy_;
}

void Terminal::reverseIndex() {
    if (cy_ == top_) scrollDown(1);
    else if (cy_ > 0) --cy_;
}

void Terminal::put(char32_t c) {
    if (wrapPending_) {
        cx_ = 0;
        lineFeed();
        wrapPending_ = false;
    }
    Cell& cell = grid_[cy_][cx_];
    cell = pen_;
    cell.ch = c;
    if (cx_ + 1 >= cols_) wrapPending_ = true;
    else ++cx_;
}

void Terminal::eraseDisplay(int mode) {
    if (mode == 0) {
        clearRow(grid_[cy_], cx_, cols_);
        for (int r = cy_ + 1; r < rows_; ++r) clearRow(grid_[r], 0, cols_);
    } else if (mode == 1) {
        for (int r = 0; r < cy_; ++r) clearRow(grid_[r], 0, cols_);
        clearRow(grid_[cy_], 0, cx_ + 1);
    } else {
        for (int r = 0; r < rows_; ++r) clearRow(grid_[r], 0, cols_);
    }
}

void Terminal::switchAlt(bool on) {
    if (on == alt_) return;
    alt_ = on;
    if (on) {
        saved_ = grid_;
        sx_ = cx_;
        sy_ = cy_;
        grid_.assign(rows_, blankRow());
    } else {
        grid_ = saved_;
        cx_ = sx_;
        cy_ = sy_;
    }
}

void Terminal::sgr() {
    std::vector<int> v = nums(params_, 0);
    for (size_t i = 0; i < v.size(); ++i) {
        const int c = v[i];
        if (c == 0) pen_ = Cell{};
        else if (c == 1) pen_.attr |= 1;
        else if (c == 4) pen_.attr |= 4;
        else if (c == 7) pen_.attr |= 2;
        else if (c == 22) pen_.attr &= ~1;
        else if (c == 24) pen_.attr &= ~4;
        else if (c == 27) pen_.attr &= ~2;
        else if (c >= 30 && c <= 37) pen_.fg = kAnsi[c - 30];
        else if (c >= 90 && c <= 97) pen_.fg = kAnsi[c - 90 + 8];
        else if (c >= 40 && c <= 47) pen_.bg = kAnsi[c - 40];
        else if (c >= 100 && c <= 107) pen_.bg = kAnsi[c - 100 + 8];
        else if (c == 39) pen_.fg = Color8{0, 0, 0, 0};
        else if (c == 49) pen_.bg = Color8{0, 0, 0, 0};
        else if ((c == 38 || c == 48) && i + 1 < v.size()) {
            Color8 col{};
            if (v[i + 1] == 5 && i + 2 < v.size()) {
                col = xterm256(v[i + 2]);
                i += 2;
            } else if (v[i + 1] == 2 && i + 4 < v.size()) {
                col = {static_cast<uint8_t>(v[i + 2]), static_cast<uint8_t>(v[i + 3]), static_cast<uint8_t>(v[i + 4]), 255};
                i += 4;
            } else break;
            (c == 38 ? pen_.fg : pen_.bg) = col;
        }
    }
}

void Terminal::csi(char fin) {
    const bool priv = !params_.empty() && params_[0] == '?';
    if (priv || (!params_.empty() && (params_[0] == '>' || params_[0] == '='))) {
        const std::string body = params_.substr(1);
        if (priv && (fin == 'h' || fin == 'l')) {
            for (int m : nums(body, 0)) {
                if (m == 25) cursorOn_ = fin == 'h';
                else if (m == 1049 || m == 1047 || m == 47) switchAlt(fin == 'h');
            }
        }
        return;
    }
    const std::vector<int> v = nums(params_, 0);
    auto arg = [&](size_t i, int def) { return i < v.size() && v[i] > 0 ? v[i] : def; };
    wrapPending_ = false;
    switch (fin) {
        case 'A': cy_ = std::max(top_ > cy_ ? 0 : top_, cy_ - arg(0, 1)); break;
        case 'B': cy_ = std::min(rows_ - 1, cy_ + arg(0, 1)); break;
        case 'C': cx_ = std::min(cols_ - 1, cx_ + arg(0, 1)); break;
        case 'D': cx_ = std::max(0, cx_ - arg(0, 1)); break;
        case 'E': cy_ = std::min(rows_ - 1, cy_ + arg(0, 1)); cx_ = 0; break;
        case 'F': cy_ = std::max(0, cy_ - arg(0, 1)); cx_ = 0; break;
        case 'G': cx_ = std::clamp(arg(0, 1) - 1, 0, cols_ - 1); break;
        case 'd': cy_ = std::clamp(arg(0, 1) - 1, 0, rows_ - 1); break;
        case 'H':
        case 'f': cy_ = std::clamp(arg(0, 1) - 1, 0, rows_ - 1); cx_ = std::clamp(arg(1, 1) - 1, 0, cols_ - 1); break;
        case 'J': eraseDisplay(v.empty() ? 0 : v[0]); break;
        case 'K': {
            const int m = v.empty() ? 0 : v[0];
            if (m == 0) clearRow(grid_[cy_], cx_, cols_);
            else if (m == 1) clearRow(grid_[cy_], 0, cx_ + 1);
            else clearRow(grid_[cy_], 0, cols_);
            break;
        }
        case 'L': { const int n = arg(0, 1); for (int i = 0; i < n && cy_ >= top_ && cy_ <= bot_; ++i) { grid_.erase(grid_.begin() + bot_); grid_.insert(grid_.begin() + cy_, blankRow()); } break; }
        case 'M': { const int n = arg(0, 1); for (int i = 0; i < n && cy_ >= top_ && cy_ <= bot_; ++i) { grid_.erase(grid_.begin() + cy_); grid_.insert(grid_.begin() + bot_, blankRow()); } break; }
        case '@': { const int n = std::min(arg(0, 1), cols_ - cx_); Row& r = grid_[cy_]; r.insert(r.begin() + cx_, n, Cell{}); r.resize(cols_); break; }
        case 'P': { const int n = std::min(arg(0, 1), cols_ - cx_); Row& r = grid_[cy_]; r.erase(r.begin() + cx_, r.begin() + cx_ + n); r.resize(cols_); break; }
        case 'X': clearRow(grid_[cy_], cx_, cx_ + arg(0, 1)); break;
        case 'S': scrollUp(arg(0, 1)); break;
        case 'T': scrollDown(arg(0, 1)); break;
        case 'm': sgr(); break;
        case 'r': {
            top_ = std::clamp(arg(0, 1) - 1, 0, rows_ - 1);
            bot_ = std::clamp(arg(1, rows_) - 1, top_, rows_ - 1);
            cx_ = 0;
            cy_ = top_;
            break;
        }
        case 's': sx_ = cx_; sy_ = cy_; break;
        case 'u': cx_ = sx_; cy_ = sy_; break;
        default: break;
    }
}

void Terminal::feed(const std::string& bytes) {
    for (unsigned char b : bytes) {
        switch (ps_) {
            case P::Ground:
                if (utfLeft_ > 0) {
                    if ((b & 0xC0) == 0x80) {
                        utfCp_ = (utfCp_ << 6) | (b & 0x3F);
                        if (--utfLeft_ == 0) put(utfCp_);
                        break;
                    }
                    utfLeft_ = 0;
                }
                if (b == 0x1B) ps_ = P::Esc;
                else if (b == '\r') { cx_ = 0; wrapPending_ = false; }
                else if (b == '\n' || b == 0x0B || b == 0x0C) { lineFeed(); wrapPending_ = false; }
                else if (b == '\b') { if (cx_ > 0) --cx_; wrapPending_ = false; }
                else if (b == '\t') { cx_ = std::min(cols_ - 1, (cx_ / 8 + 1) * 8); wrapPending_ = false; }
                else if (b < 0x20 || b == 0x7F) {}
                else if (b < 0x80) put(b);
                else if ((b & 0xE0) == 0xC0) { utfCp_ = b & 0x1F; utfLeft_ = 1; }
                else if ((b & 0xF0) == 0xE0) { utfCp_ = b & 0x0F; utfLeft_ = 2; }
                else if ((b & 0xF8) == 0xF0) { utfCp_ = b & 0x07; utfLeft_ = 3; }
                break;
            case P::Esc:
                if (b == '[') { ps_ = P::Csi; params_.clear(); }
                else if (b == ']') ps_ = P::Osc;
                else if (b == '(' || b == ')' || b == '#') ps_ = P::Charset;
                else {
                    ps_ = P::Ground;
                    if (b == '7') { sx_ = cx_; sy_ = cy_; }
                    else if (b == '8') { cx_ = sx_; cy_ = sy_; }
                    else if (b == 'M') reverseIndex();
                    else if (b == 'D') lineFeed();
                    else if (b == 'E') { cx_ = 0; lineFeed(); }
                    else if (b == 'c') { grid_.assign(rows_, blankRow()); cx_ = cy_ = 0; pen_ = Cell{}; top_ = 0; bot_ = rows_ - 1; }
                }
                break;
            case P::Charset: ps_ = P::Ground; break;
            case P::Csi:
                if (b >= 0x40 && b <= 0x7E) {
                    ps_ = P::Ground;
                    csi(static_cast<char>(b));
                } else if (params_.size() < 64) params_ += static_cast<char>(b);
                break;
            case P::Osc:
                if (b == 0x07) ps_ = P::Ground;
                else if (b == 0x1B) ps_ = P::OscEsc;
                break;
            case P::OscEsc: ps_ = P::Ground; break;
        }
    }
}

// ---- panel hooks --------------------------------------------------------------------------------------------------

void Terminal::tick(double dt) {
    blink_ += dt;
    sinceKick_ += dt;
    std::string data;
    {
        std::lock_guard<std::mutex> lk(mu_);
        data.swap(pending_);
    }
    if (!data.empty()) {
        feed(data);
        dirtyOut_ = true;
    }
    if (alive_ && proc_ && WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) {
        // let the last output drain before declaring the session over
        std::string tail;
        {
            std::lock_guard<std::mutex> lk(mu_);
            tail.swap(pending_);
        }
        if (!tail.empty()) feed(tail);
        alive_ = false;
        exitNote_ = "[process ended - pick a shell above to start a new session]";
        dirtyOut_ = true;
    }
    if (dirtyOut_ && sinceKick_ >= 0.03) {
        dirtyOut_ = false;
        sinceKick_ = 0;
        invalidate();
    }
}

bool Terminal::onHover(float x, float y) {
    int h = -1;
    for (int i = 0; i < 3; ++i)
        if (shellBtn_[i].contains(x, y)) h = i;
    if (h == hoverBtn_) return false;
    hoverBtn_ = h;
    return true;
}

void Terminal::onClick(float x, float y, bool) {
    for (int i = 0; i < 3; ++i)
        if (shellBtn_[i].contains(x, y) && (i != 2 || hasWsl_)) {
            start(i);
            return;
        }
}

void Terminal::onWheel(float, float, float notches) {
    scrollOff_ = std::clamp(scrollOff_ + static_cast<int>(notches * 3), 0, static_cast<int>(scrollback_.size()));
}

void Terminal::onKey(EditKey k) {
    switch (k) {
        case EditKey::Enter: send("\r"); break;
        case EditKey::Backspace: send("\x7f"); break;
        case EditKey::Tab: send("\t"); break;
        case EditKey::Left: send("\x1b[D"); break;
        case EditKey::Right: send("\x1b[C"); break;
        case EditKey::Up: send("\x1b[A"); break;
        case EditKey::Down: send("\x1b[B"); break;
        case EditKey::Home: send("\x1b[H"); break;
        case EditKey::End: send("\x1b[F"); break;
        case EditKey::Delete: send("\x1b[3~"); break;
        case EditKey::PageUp: if (scrollback_.empty() || alt_) send("\x1b[5~"); else onWheel(0, 0, 10); break;
        case EditKey::PageDown: if (scrollOff_ == 0) send("\x1b[6~"); else onWheel(0, 0, -10); break;
        case EditKey::Copy: send("\x03"); break;
        case EditKey::Cut: send("\x18"); break;
        case EditKey::SelectAll: send("\x01"); break;
        case EditKey::Undo: send("\x1a"); break;
        case EditKey::Save: send("\x13"); break;
        case EditKey::Edit: send("\x05"); break;
        case EditKey::Paste: {
            if (char* t = SDL_GetClipboardText()) {
                std::string s = t;
                SDL_free(t);
                std::string out;
                for (char c : s)
                    if (c != '\n') out += c;
                    else out += '\r';
                send(out);
            }
            break;
        }
        default: break;
    }
}

void Terminal::onText(const std::string& s) { send(s); }

void Terminal::onCtrl(const std::string& s) {
    if (s == "esc") send("\x1b");
}

void Terminal::paint(Bitmap& bm) {
    using namespace ui;
    const Tokens t = tokens();
    const char* names[3] = {"POWERSHELL", "CMD", "WSL"};
    Rect c = panel(bm, "TERMINAL", names[shell_]);
    float bx = c.x0 + 16;
    for (int i = 0; i < 3; ++i) {
        const float w = measureText(names[i], 22.0f, FontFace::UIBold) + 36;
        shellBtn_[i] = {bx, c.y0 + 8, bx + w, c.y0 + 46};
        const State s = (i == 2 && !hasWsl_) ? State::Disabled : (i == shell_ ? State::Selected : (i == hoverBtn_ ? State::Hover : State::Normal));
        button(bm, shellBtn_[i], names[i], s, false, 22);
        bx += w + 10;
    }
    if (scrollOff_ > 0) labelRight(bm, c.x1 - 16, c.y0 + 36, "SCROLLBACK " + std::to_string(scrollOff_) + " LINES  (type to return)", 20, FontFace::UI, t.warn);

    const float gx = c.x0 + 20, gy = c.y0 + 62;
    fillRect(bm, gx - 8, gy - 6, gx + cols_ * cw_ + 8, gy + rows_ * lh_ + 6, Color8{4,8,14,250});
    const int total = static_cast<int>(scrollback_.size());
    const int first = total - scrollOff_;  // index into scrollback + grid
    for (int r = 0; r < rows_; ++r) {
        const int idx = first + r;
        const Row* row = idx < 0 ? nullptr : (idx < total ? &scrollback_[idx] : (idx - total < rows_ ? &grid_[idx - total] : nullptr));
        if (!row) continue;
        const float y = gy + r * lh_;
        int col = 0;
        while (col < cols_) {
            const Cell& a = (*row)[col];
            auto resolve = [&](const Cell& x, Color8& fg, Color8& bg, bool& hasBg) {
                fg = x.fg.a ? x.fg : t.text;
                bg = x.bg;
                hasBg = x.bg.a != 0;
                if (x.attr & 2) {
                    Color8 f = fg;
                    fg = hasBg ? bg : Color8{4, 8, 14, 255};
                    bg = f;
                    hasBg = true;
                }
            };
            Color8 fg, bg;
            bool hasBg;
            resolve(a, fg, bg, hasBg);
            int end = col + 1;
            std::string run;
            appendUtf8(run, a.ch);
            while (end < cols_) {
                const Cell& b = (*row)[end];
                Color8 f2, b2;
                bool h2;
                resolve(b, f2, b2, h2);
                if (f2.r != fg.r || f2.g != fg.g || f2.b != fg.b || h2 != hasBg || (h2 && (b2.r != bg.r || b2.g != bg.g || b2.b != bg.b)) ||
                    (b.attr & 4) != (a.attr & 4)) break;
                appendUtf8(run, b.ch);
                ++end;
            }
            if (hasBg) fillRect(bm, gx + col * cw_, y, gx + end * cw_, y + lh_, bg);
            bool blank = true;
            for (char ch : run) if (ch != ' ') { blank = false; break; }
            if (!blank) {
                // draw per cell so proportional rounding never drifts the columns
                int cc = col;
                size_t pos = 0;
                while (pos < run.size()) {
                    size_t len = 1;
                    while (pos + len < run.size() && (static_cast<unsigned char>(run[pos + len]) & 0xC0) == 0x80) ++len;
                    if (run[pos] != ' ') drawText(bm, gx + cc * cw_, y + 18, run.substr(pos, len), 20.0f, FontFace::Mono, fg);
                    pos += len;
                    ++cc;
                }
            }
            if (a.attr & 4) fillRect(bm, gx + col * cw_, y + lh_ - 3, gx + end * cw_, y + lh_ - 2, fg);
            col = end;
        }
    }
    if (scrollOff_ == 0 && cursorOn_ && alive_ && std::fmod(blink_, 1.0) < 0.6) {
        fillRect(bm, gx + cx_ * cw_, gy + cy_ * lh_, gx + (cx_ + 1) * cw_, gy + cy_ * lh_ + lh_, withAlpha(t.accent, 200));
    }
    if (!exitNote_.empty()) label(bm, gx, gy + rows_ * lh_ + 40, exitNote_, 22, FontFace::UI, t.warn);
}

}  // namespace sf

#include "CodeEditor.h"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "TextRender.h"

namespace sf {

namespace {
namespace fs = std::filesystem;

constexpr float kFontPx = 24.0f;
constexpr int kGutter = 96, kPad = 18, kTop = 18, kRightMargin = 30, kTabWidth = 4;
constexpr size_t kMaxBytes = 2u * 1024 * 1024, kMaxRenderedLine = 4000;

enum Cls : uint8_t { Plain, Keyword, String, Comment, Number, Func, Type, Special };
enum LangFlags { kCaseInsensitive = 1, kMarkup = 2, kJson = 4, kMarkdown = 8, kPreproc = 16 };

struct LangDef {
    const char* name;
    const char* exts;  // space separated, lower case
    const char* keywords;
    const char* lineComment;
    const char* lineComment2;
    const char* blockStart;
    const char* blockEnd;
    const char* quotes;
    int flags;
};

const char* kCKeywords =
    "if else for while do switch case break continue return goto default struct class enum union typedef namespace "
    "using template typename const static constexpr inline virtual override final public private protected new delete "
    "this nullptr true false void int char float double bool long short unsigned signed auto sizeof include define "
    "ifdef ifndef endif pragma try catch throw explicit operator volatile extern mutable friend uint8_t uint32_t "
    "size_t std";
const char* kShaderKeywords =
    "if else for while do switch case break continue return discard struct cbuffer static const uniform in out inout "
    "float float2 float3 float4 float4x4 float3x3 int uint bool void Texture2D SamplerState sampler2D layout vec2 vec3 "
    "vec4 mat4 true false SV_Position SV_Target register";
const char* kGenericKeywords =
    "if else elif for foreach while do switch case break continue return default new class public private protected "
    "static final abstract extends implements interface import package namespace using void int long string bool "
    "boolean char float double true false null nil this self super try catch finally throw throws async await fn let "
    "mut pub use mod impl trait match loop where func go defer chan map range select var const type struct enum "
    "function echo extends as in is not and or";
const char* kScriptKeywords =
    "function var let const return if else for while do switch case break continue new delete this class extends "
    "super import export from default async await yield try catch finally throw typeof instanceof in of null "
    "undefined true false void interface type enum implements public private protected readonly static as echo";
const char* kPythonKeywords =
    "def class return if elif else for while in not and or is None True False import from as with try except finally "
    "raise pass break continue lambda yield global nonlocal assert del async await self if then fi do done esac "
    "function echo export local";
const char* kSqlKeywords =
    "select from where insert into values update set delete create table alter drop index join inner left right outer "
    "on group by order having limit as and or not null is in like distinct union primary key foreign references "
    "default int varchar text boolean true false";
const char* kBatKeywords = "echo set if else goto call exit for in do not exist errorlevel setlocal endlocal pause";
const char* kCmakeKeywords =
    "if else elseif endif foreach endforeach function endfunction set project add_executable add_library "
    "target_link_libraries target_include_directories find_package include option cmake_minimum_required";

const LangDef kLangs[] = {
    {"C / C++", "c h cpp hpp cc cxx", kCKeywords, "//", "", "/*", "*/", "\"'", kPreproc},
    {"HLSL / GLSL", "hlsl glsl", kShaderKeywords, "//", "", "/*", "*/", "\"", kPreproc},
    {"C# / Java", "cs java", kGenericKeywords, "//", "", "/*", "*/", "\"'", 0},
    {"Go / Rust / PHP", "go rs php", kGenericKeywords, "//", "#", "/*", "*/", "\"'`", 0},
    {"JavaScript / TypeScript", "js ts tsx jsx mjs", kScriptKeywords, "//", "", "/*", "*/", "\"'`", 0},
    {"Python", "py", kPythonKeywords, "#", "", "", "", "\"'", 0},
    {"Shell", "sh", kPythonKeywords, "#", "", "", "", "\"'", 0},
    {"PowerShell", "ps1", kPythonKeywords, "#", "", "<#", "#>", "\"'", kCaseInsensitive},
    {"Batch", "bat", kBatKeywords, "::", "REM ", "", "", "\"", kCaseInsensitive},
    {"SQL", "sql", kSqlKeywords, "--", "", "/*", "*/", "'\"", kCaseInsensitive},
    {"JSON", "json", "true false null", "", "", "", "", "\"", kJson},
    {"YAML", "yml yaml", "true false null yes no", "#", "", "", "", "\"'", 0},
    {"TOML / INI", "toml ini cfg", "true false", "#", ";", "", "", "\"'", 0},
    {"HTML / XML", "html htm xml svg vue", "", "", "", "<!--", "-->", "\"'", kMarkup},
    {"CSS", "css", "important", "", "", "/*", "*/", "\"'", 0},
    {"Markdown", "md", "", "", "", "", "", "", kMarkdown},
    {"CMake", "cmake", kCmakeKeywords, "#", "", "", "", "\"", kCaseInsensitive},
};
constexpr int kLangCount = static_cast<int>(sizeof(kLangs) / sizeof(kLangs[0]));

int findLang(const std::string& ext) {
    for (int i = 0; i < kLangCount; ++i) {
        std::istringstream ss(kLangs[i].exts);
        std::string e;
        while (ss >> e)
            if (e == ext) return i;
    }
    return -1;
}

const std::unordered_set<std::string>& keywordSet(int id) {
    static std::unordered_map<int, std::unordered_set<std::string>> cache;
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;
    std::unordered_set<std::string> set;
    std::istringstream ss(kLangs[id].keywords);
    std::string w;
    while (ss >> w) {
        if (kLangs[id].flags & kCaseInsensitive)
            for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        set.insert(w);
    }
    return cache.emplace(id, std::move(set)).first->second;
}

bool isIdentChar(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '$' || u >= 0x80;
}

// Colours one line. `inBlock` carries block-comment state across lines.
void highlight(int langId, const std::string& s, bool& inBlock, std::vector<uint8_t>& cls) {
    cls.assign(s.size(), Plain);
    if (langId < 0) return;
    const LangDef& L = kLangs[langId];
    const auto& kw = keywordSet(langId);
    const size_t n = s.size();
    const size_t bsLen = std::strlen(L.blockStart), beLen = std::strlen(L.blockEnd);
    auto starts = [&](size_t p, const char* t) {
        size_t l = std::strlen(t);
        return l > 0 && p + l <= n && s.compare(p, l, t) == 0;
    };
    auto fill = [&](size_t a, size_t b, Cls c) {
        for (size_t k = a; k < b && k < n; ++k) cls[k] = c;
    };

    if ((L.flags & kMarkdown) && n > 0 && s[0] == '#') {
        fill(0, n, Special);
        return;
    }
    bool inTag = false, tagName = false;
    size_t i = 0;
    while (i < n) {
        if (inBlock) {
            size_t e = beLen ? s.find(L.blockEnd, i) : std::string::npos;
            size_t stop = e == std::string::npos ? n : e + beLen;
            fill(i, stop, Comment);
            i = stop;
            if (e != std::string::npos) inBlock = false;
            continue;
        }
        const char c = s[i];
        if (bsLen && starts(i, L.blockStart)) {
            fill(i, i + bsLen, Comment);
            i += bsLen;
            inBlock = true;
            continue;
        }
        if (starts(i, L.lineComment) || starts(i, L.lineComment2)) {
            fill(i, n, Comment);
            break;
        }
        const bool code = !(L.flags & kMarkup) || inTag;
        if ((L.flags & kMarkup) && !inTag && c == '<') {
            inTag = tagName = true;
            cls[i++] = Special;
            continue;
        }
        if (inTag && c == '>') {
            inTag = false;
            cls[i++] = Special;
            continue;
        }
        if ((L.flags & kPreproc) && c == '#' && s.find_first_not_of(" \t") == i) {
            size_t j = i + 1;
            while (j < n && std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
            fill(i, j, Keyword);
            i = j;
            continue;
        }
        if (code && c != 0 && std::strchr(L.quotes, c)) {
            size_t j = i + 1;
            while (j < n && s[j] != c) j += (s[j] == '\\') ? 2 : 1;
            size_t end = std::min(j + 1, n);
            Cls kind = String;
            if (L.flags & kJson) {
                size_t k = end;
                while (k < n && (s[k] == ' ' || s[k] == '\t')) ++k;
                if (k < n && s[k] == ':') kind = Type;
            }
            fill(i, end, kind);
            i = end;
            continue;
        }
        if (code && std::isdigit(static_cast<unsigned char>(c)) && (i == 0 || !isIdentChar(s[i - 1]))) {
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '.' || s[j] == '_')) ++j;
            fill(i, j, Number);
            i = j;
            continue;
        }
        if (code && isIdentChar(c)) {
            size_t j = i;
            while (j < n && (isIdentChar(s[j]) || (inTag && s[j] == '-'))) ++j;
            std::string word = s.substr(i, j - i);
            if (inTag) {
                fill(i, j, tagName ? Special : Type);
                tagName = false;
            } else {
                if (L.flags & kCaseInsensitive)
                    for (char& ch : word) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (kw.count(word)) {
                    fill(i, j, Keyword);
                } else {
                    size_t k = j;
                    while (k < n && s[k] == ' ') ++k;
                    if (k < n && s[k] == '(') fill(i, j, Func);
                    else if (std::isupper(static_cast<unsigned char>(word[0])) && word.size() > 1) fill(i, j, Type);
                }
            }
            i = j;
            continue;
        }
        if (inTag && c == '/') {
            cls[i++] = Special;
            continue;
        }
        ++i;
    }
}

const Color8 kPalette[] = {
    {219, 227, 245, 255},  // plain
    {199, 156, 255, 255},  // keyword
    {152, 224, 140, 255},  // string
    {110, 125, 160, 255},  // comment
    {255, 191, 115, 255},  // number
    {115, 190, 255, 255},  // function
    {120, 220, 220, 255},  // type
    {255, 128, 150, 255},  // special (tags, headings)
};

size_t prevBoundary(const std::string& s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

size_t nextBoundary(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

int dispColOf(const std::string& s, size_t byte) {
    int col = 0;
    for (size_t i = 0; i < byte && i < s.size(); i = nextBoundary(s, i)) col += (s[i] == '\t') ? kTabWidth - col % kTabWidth : 1;
    return col;
}

size_t byteAtDisp(const std::string& s, int dcol) {
    int col = 0;
    size_t i = 0;
    while (i < s.size()) {
        int w = (s[i] == '\t') ? kTabWidth - col % kTabWidth : 1;
        if (col + w > dcol && col >= dcol) break;
        if (col >= dcol) break;
        col += w;
        i = nextBoundary(s, i);
    }
    return i;
}

void fillRect(Bitmap& bm, int x0, int y0, int x1, int y1, Color8 c) {
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, bm.w);
    y1 = std::min(y1, bm.h);
    for (int y = y0; y < y1; ++y) {
        uint8_t* p = &bm.rgba[(static_cast<size_t>(y) * bm.w + x0) * 4];
        for (int x = x0; x < x1; ++x, p += 4) {
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            p[3] = 255;
        }
    }
}

constexpr Color8 kBg{14, 18, 29, 255}, kGutterBg{11, 14, 24, 255}, kLineBg{22, 28, 46, 255},
    kGutterText{86, 98, 130, 255}, kGutterActive{170, 184, 215, 255}, kCaret{107, 158, 255, 255},
    kTrack{28, 34, 54, 255}, kThumb{70, 86, 130, 255};
}  // namespace

void CodeEditor::metrics() {
    if (cw_ > 0) return;
    cw_ = measureText("MMMMMMMMMM", kFontPx, FontFace::Mono) / 10.0f;
    if (cw_ < 1.0f) cw_ = kFontPx * 0.6f;
    lineH_ = std::ceil(textLineHeight(kFontPx, FontFace::Mono)) + 4.0f;
}

int CodeEditor::visibleLines() const { return std::max(1, static_cast<int>((pxH_ - 2 * kTop) / std::max(lineH_, 1.0f))); }

void CodeEditor::clear() {
    lines_.assign(1, std::string());
    curLine_ = curCol_ = wantDisp_ = scroll_ = hscroll_ = 0;
    scrollFrac_ = 0;
    dirty_ = readOnly_ = truncated_ = crlf_ = bom_ = false;
    path_.clear();
    language_ = "Plain text";
    langId_ = -1;
    statesDirty_ = true;
    touch();
}

bool CodeEditor::load(const std::string& path) {
    clear();
    metrics();
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    std::string data(kMaxBytes + 1, '\0');
    f.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(f.gcount()));
    if (data.size() > kMaxBytes) {
        data.resize(kMaxBytes);
        truncated_ = readOnly_ = true;  // never overwrite a file with a clipped copy
    }
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        bom_ = true;
        data.erase(0, 3);
    }
    if (data.find('\0', 0) != std::string::npos) {
        readOnly_ = true;  // looks binary
        std::replace(data.begin(), data.end(), '\0', '.');
    }
    crlf_ = data.find("\r\n") != std::string::npos;
    lines_.clear();
    size_t start = 0;
    for (;;) {
        size_t nl = data.find('\n', start);
        std::string line = data.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines_.push_back(std::move(line));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    path_ = path;
    std::string ext = fs::u8path(path).extension().string();
    if (!ext.empty()) ext.erase(0, 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    langId_ = findLang(ext);
    language_ = langId_ >= 0 ? kLangs[langId_].name : "Plain text";
    statesDirty_ = true;
    touch();
    return true;
}

bool CodeEditor::save() {
    if (readOnly_ || path_.empty()) return false;
    std::string out = bom_ ? "\xEF\xBB\xBF" : "";
    const char* eol = crlf_ ? "\r\n" : "\n";
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i) out += eol;
        out += lines_[i];
    }
    fs::path target = fs::u8path(path_), tmp = target;
    tmp += ".sfsave";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    dirty_ = false;
    touch();
    return true;
}

bool CodeEditor::saveAs(const std::string& path) {
    if (readOnly_) return false;
    const std::string old = path_;
    path_ = path;
    if (!save()) {
        path_ = old;
        return false;
    }
    std::string ext = fs::u8path(path).extension().string();
    if (!ext.empty()) ext.erase(0, 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    langId_ = findLang(ext);
    language_ = langId_ >= 0 ? kLangs[langId_].name : "Plain text";
    statesDirty_ = true;
    touch();
    return true;
}

int CodeEditor::cursorColumn() const { return dispColOf(lines_[curLine_], static_cast<size_t>(curCol_)); }

void CodeEditor::clampCursor() {
    curLine_ = std::clamp(curLine_, 0, lineCount() - 1);
    curCol_ = std::clamp(curCol_, 0, static_cast<int>(lines_[curLine_].size()));
}

void CodeEditor::ensureVisible() {
    metrics();
    int vis = visibleLines();
    if (curLine_ < scroll_) scroll_ = curLine_;
    if (curLine_ >= scroll_ + vis) scroll_ = curLine_ - vis + 1;
    int cols = std::max(8, static_cast<int>((pxW_ - kGutter - kPad - kRightMargin) / cw_));
    int d = cursorColumn();
    if (d < hscroll_) hscroll_ = d;
    if (d >= hscroll_ + cols - 1) hscroll_ = d - cols + 2;
    hscroll_ = std::max(0, hscroll_);
}

void CodeEditor::edited() {
    dirty_ = true;
    statesDirty_ = true;
    wantDisp_ = cursorColumn();
    ensureVisible();
    touch();
}

void CodeEditor::setCol(int line, int dcol) {
    curLine_ = std::clamp(line, 0, lineCount() - 1);
    curCol_ = static_cast<int>(byteAtDisp(lines_[curLine_], dcol));
}

void CodeEditor::insertText(const std::string& utf8) {
    if (readOnly_ || utf8.empty()) return;
    std::string seg;
    auto flush = [&] {
        if (seg.empty()) return;
        lines_[curLine_].insert(static_cast<size_t>(curCol_), seg);
        curCol_ += static_cast<int>(seg.size());
        seg.clear();
    };
    for (char c : utf8) {
        if (c == '\r') continue;
        if (c == '\n') {
            flush();
            std::string rest = lines_[curLine_].substr(static_cast<size_t>(curCol_));
            lines_[curLine_].erase(static_cast<size_t>(curCol_));
            lines_.insert(lines_.begin() + curLine_ + 1, rest);
            ++curLine_;
            curCol_ = 0;
        } else if (c == '\t' || static_cast<unsigned char>(c) >= 0x20) {
            seg.push_back(c);
        }
    }
    flush();
    edited();
}

void CodeEditor::newline() {
    if (readOnly_) return;
    const std::string& cur = lines_[curLine_];
    size_t indentLen = 0;
    while (indentLen < cur.size() && indentLen < static_cast<size_t>(curCol_) && (cur[indentLen] == ' ' || cur[indentLen] == '\t')) ++indentLen;
    std::string indent = cur.substr(0, indentLen);
    insertText("\n" + indent);
}

void CodeEditor::tab() { insertText(std::string(kTabWidth, ' ')); }

void CodeEditor::paste() {
    if (readOnly_) return;
    if (char* t = SDL_GetClipboardText()) {
        insertText(t);
        SDL_free(t);
    }
}

void CodeEditor::backspace() {
    if (readOnly_) return;
    if (curCol_ > 0) {
        size_t prev = prevBoundary(lines_[curLine_], static_cast<size_t>(curCol_));
        lines_[curLine_].erase(prev, static_cast<size_t>(curCol_) - prev);
        curCol_ = static_cast<int>(prev);
    } else if (curLine_ > 0) {
        int prevLen = static_cast<int>(lines_[curLine_ - 1].size());
        lines_[curLine_ - 1] += lines_[curLine_];
        lines_.erase(lines_.begin() + curLine_);
        --curLine_;
        curCol_ = prevLen;
    } else {
        return;
    }
    edited();
}

void CodeEditor::del() {
    if (readOnly_) return;
    std::string& cur = lines_[curLine_];
    if (static_cast<size_t>(curCol_) < cur.size()) {
        size_t next = nextBoundary(cur, static_cast<size_t>(curCol_));
        cur.erase(static_cast<size_t>(curCol_), next - static_cast<size_t>(curCol_));
    } else if (curLine_ + 1 < lineCount()) {
        cur += lines_[curLine_ + 1];
        lines_.erase(lines_.begin() + curLine_ + 1);
    } else {
        return;
    }
    edited();
}

void CodeEditor::moveLeft() {
    if (curCol_ > 0) curCol_ = static_cast<int>(prevBoundary(lines_[curLine_], static_cast<size_t>(curCol_)));
    else if (curLine_ > 0) {
        --curLine_;
        curCol_ = static_cast<int>(lines_[curLine_].size());
    }
    wantDisp_ = cursorColumn();
    ensureVisible();
    touch();
}

void CodeEditor::moveRight() {
    if (static_cast<size_t>(curCol_) < lines_[curLine_].size()) curCol_ = static_cast<int>(nextBoundary(lines_[curLine_], static_cast<size_t>(curCol_)));
    else if (curLine_ + 1 < lineCount()) {
        ++curLine_;
        curCol_ = 0;
    }
    wantDisp_ = cursorColumn();
    ensureVisible();
    touch();
}

void CodeEditor::moveUp() {
    if (curLine_ > 0) setCol(curLine_ - 1, wantDisp_);
    ensureVisible();
    touch();
}

void CodeEditor::moveDown() {
    if (curLine_ + 1 < lineCount()) setCol(curLine_ + 1, wantDisp_);
    ensureVisible();
    touch();
}

void CodeEditor::home() {
    const std::string& s = lines_[curLine_];
    size_t first = s.find_first_not_of(" \t");
    if (first == std::string::npos) first = 0;
    curCol_ = (static_cast<size_t>(curCol_) == first) ? 0 : static_cast<int>(first);
    wantDisp_ = cursorColumn();
    ensureVisible();
    touch();
}

void CodeEditor::end() {
    curCol_ = static_cast<int>(lines_[curLine_].size());
    wantDisp_ = cursorColumn();
    ensureVisible();
    touch();
}

void CodeEditor::page(int direction) {
    metrics();
    setCol(curLine_ + direction * std::max(1, visibleLines() - 1), wantDisp_);
    ensureVisible();
    touch();
}

void CodeEditor::docStart() {
    curLine_ = curCol_ = 0;
    wantDisp_ = 0;
    ensureVisible();
    touch();
}

void CodeEditor::docEnd() {
    curLine_ = lineCount() - 1;
    curCol_ = static_cast<int>(lines_[curLine_].size());
    wantDisp_ = cursorColumn();
    ensureVisible();
    touch();
}

void CodeEditor::scrollBy(float lines) {
    metrics();
    scrollFrac_ += lines;
    int whole = static_cast<int>(scrollFrac_);
    scrollFrac_ -= static_cast<float>(whole);
    int maxScroll = std::max(0, lineCount() - std::max(1, visibleLines() / 2));
    int next = std::clamp(scroll_ + whole, 0, maxScroll);
    if (next != scroll_) {
        scroll_ = next;
        touch();
    }
}

void CodeEditor::click(float px, float py) {
    metrics();
    int row = static_cast<int>(std::floor((py - kTop) / lineH_));
    int line = std::clamp(scroll_ + row, 0, lineCount() - 1);
    int dcol = std::max(0, hscroll_ + static_cast<int>(std::floor((px - (kGutter + kPad)) / cw_ + 0.5f)));
    setCol(line, dcol);
    wantDisp_ = cursorColumn();
    ensureVisible();
    touch();
}

void CodeEditor::recomputeStates() {
    states_.assign(lines_.size() + 1, 0);
    statesDirty_ = false;
    if (langId_ < 0 || !kLangs[langId_].blockStart[0]) return;
    bool inBlock = false;
    std::vector<uint8_t> cls;
    for (size_t i = 0; i < lines_.size(); ++i) {
        states_[i] = inBlock ? 1 : 0;
        highlight(langId_, lines_[i].size() > kMaxRenderedLine ? lines_[i].substr(0, kMaxRenderedLine) : lines_[i], inBlock, cls);
    }
}

void CodeEditor::render(Bitmap& bm, bool editing, bool caretOn) {
    metrics();
    bm.w = pxW_;
    bm.h = pxH_;
    bm.rgba.resize(static_cast<size_t>(pxW_) * pxH_ * 4);
    fillRect(bm, 0, 0, pxW_, pxH_, kBg);
    if (statesDirty_) recomputeStates();

    const int vis = visibleLines();
    const int textX = kGutter + kPad;
    const int cols = static_cast<int>((pxW_ - textX - kRightMargin) / cw_);
    const float ascent = textAscent(kFontPx, FontFace::Mono);
    std::vector<uint8_t> cls;

    for (int r = 0; r < vis; ++r) {
        int li = scroll_ + r;
        if (li >= lineCount()) break;
        int y0 = kTop + static_cast<int>(r * lineH_);
        float baseline = static_cast<float>(y0) + 2.0f + ascent;
        if (editing && li == curLine_) fillRect(bm, kGutter, y0, pxW_, y0 + static_cast<int>(lineH_), kLineBg);

        std::string line = lines_[li].size() > kMaxRenderedLine ? lines_[li].substr(0, kMaxRenderedLine) : lines_[li];
        bool inBlock = li < static_cast<int>(states_.size()) && states_[li];
        highlight(langId_, line, inBlock, cls);

        int dc = 0;
        size_t i = 0;
        while (i < line.size()) {
            size_t j = i;
            while (j < line.size() && cls[j] == cls[i]) ++j;
            int startDc = dc;
            std::string expanded;
            bool onlySpace = true;
            for (size_t k = i; k < j; k = nextBoundary(line, k)) {
                size_t kn = nextBoundary(line, k);
                if (line[k] == '\t') {
                    int w = kTabWidth - dc % kTabWidth;
                    expanded.append(static_cast<size_t>(w), ' ');
                    dc += w;
                } else {
                    expanded.append(line, k, kn - k);
                    if (line[k] != ' ') onlySpace = false;
                    ++dc;
                }
            }
            if (!onlySpace && dc > hscroll_ && startDc < hscroll_ + cols + 1)
                drawText(bm, textX + (startDc - hscroll_) * cw_, baseline, expanded, kFontPx, FontFace::Mono, kPalette[cls[i]]);
            i = j;
            if (dc > hscroll_ + cols + 1) break;
        }

        if (editing && caretOn && li == curLine_) {
            int cx = textX + static_cast<int>((cursorColumn() - hscroll_) * cw_);
            fillRect(bm, cx, y0 + 2, cx + 3, y0 + static_cast<int>(lineH_) - 2, kCaret);
        }
    }

    // The gutter is painted last so horizontally scrolled text disappears behind it.
    fillRect(bm, 0, 0, kGutter, pxH_, kGutterBg);
    fillRect(bm, kGutter - 1, 0, kGutter, pxH_, kTrack);
    for (int r = 0; r < vis; ++r) {
        int li = scroll_ + r;
        if (li >= lineCount()) break;
        std::string num = std::to_string(li + 1);
        float baseline = static_cast<float>(kTop + static_cast<int>(r * lineH_)) + 2.0f + ascent;
        float w = measureText(num, kFontPx, FontFace::Mono);
        drawText(bm, kGutter - 18.0f - w, baseline, num, kFontPx, FontFace::Mono, (editing && li == curLine_) ? kGutterActive : kGutterText);
    }

    if (lineCount() > vis) {
        int trackY0 = kTop, trackY1 = pxH_ - kTop;
        fillRect(bm, pxW_ - 14, trackY0, pxW_ - 8, trackY1, kTrack);
        float frac = static_cast<float>(vis) / static_cast<float>(lineCount());
        int thumbH = std::max(48, static_cast<int>((trackY1 - trackY0) * frac));
        int maxScroll = std::max(1, lineCount() - vis);
        int ty = trackY0 + static_cast<int>((trackY1 - trackY0 - thumbH) * std::min(1.0f, static_cast<float>(scroll_) / maxScroll));
        fillRect(bm, pxW_ - 14, ty, pxW_ - 8, ty + thumbH, kThumb);
    }
}

}  // namespace sf

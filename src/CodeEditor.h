#pragma once
// A small text/code editor model: UTF-8 document, cursor, scrolling, syntax highlighting and a CPU rasteriser that
// draws the visible part of the document into a bitmap (the viewer uploads it as the editor "screen").
#include <cstdint>
#include <string>
#include <vector>

#include "Utils.h"

namespace sf {

class CodeEditor {
public:
    static constexpr int kPxW = 1400, kPxH = 1000;  // size of the rendered editor surface

    bool load(const std::string& path);  // false when the file cannot be read
    bool save();                         // false when read-only or the write failed
    void clear();

    bool dirty() const { return dirty_; }
    bool readOnly() const { return readOnly_; }
    bool truncated() const { return truncated_; }
    const std::string& language() const { return language_; }
    int lineCount() const { return static_cast<int>(lines_.size()); }
    int cursorLine() const { return curLine_; }
    int cursorColumn() const;  // 0-based display column
    uint64_t version() const { return version_; }  // bumps on every visual change

    // Editing (ignored when read-only).
    void insertText(const std::string& utf8);
    void backspace();
    void del();
    void newline();
    void tab();
    void paste();

    // Navigation.
    void moveLeft();
    void moveRight();
    void moveUp();
    void moveDown();
    void home();
    void end();
    void page(int direction);
    void docStart();
    void docEnd();
    void scrollBy(float lines);
    void click(float px, float py);  // pixel position in the rendered surface

    void render(Bitmap& bm, bool editing, bool caretOn);

private:
    void metrics();
    void touch() { ++version_; }
    void edited();
    void ensureVisible();
    int visibleLines() const;
    void clampCursor();
    void recomputeStates();
    void setCol(int line, int dcol);

    std::vector<std::string> lines_{std::string()};
    int curLine_ = 0, curCol_ = 0;  // curCol_ is a byte offset on a code point boundary
    int wantDisp_ = 0;              // preferred display column for vertical moves
    int scroll_ = 0, hscroll_ = 0;
    float scrollFrac_ = 0;
    bool dirty_ = false, readOnly_ = false, truncated_ = false, crlf_ = false, bom_ = false;
    std::string path_, language_ = "Plain text";
    uint64_t version_ = 1;
    std::vector<uint8_t> states_;  // per line: 1 when the line starts inside a block comment
    bool statesDirty_ = true;
    int langId_ = 0;
    float cw_ = 0, lineH_ = 0;
};

}  // namespace sf

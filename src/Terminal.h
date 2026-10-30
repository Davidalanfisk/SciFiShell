#pragma once
// Terminal window: a real Windows pseudo console (ConPTY) running PowerShell / cmd / WSL, with an ANSI / VT screen model
// (colours, cursor addressing, scroll regions, alternate screen) and scrollback. The session survives closing the panel.
#include <windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "PanelApp.h"

namespace sf {

class Terminal : public PanelApp {
public:
    explicit Terminal(Renderer& renderer);
    ~Terminal() override;
    const char* name() const override { return "TERMINAL"; }
    bool wantsText() const override { return true; }
    bool keepAlive() const override { return alive_; }

protected:
    void onOpen() override;
    void tick(double dt) override;
    void paint(Bitmap& bm) override;
    bool onHover(float x, float y) override;
    void onClick(float x, float y, bool dbl) override;
    void onWheel(float x, float y, float notches) override;
    void onKey(EditKey k) override;
    void onText(const std::string& s) override;
    void onCtrl(const std::string& s) override;
    double repaintInterval() const override { return 0.5; }  // cursor blink
    bool fsResizable() const override { return true; }
    void onResize(int w, int h) override;

private:
    struct Cell {
        char32_t ch = U' ';
        Color8 fg{0, 0, 0, 0}, bg{0, 0, 0, 0};  // alpha 0 = default colour
        uint8_t attr = 0;                       // 1 bold, 2 inverse, 4 underline
    };
    using Row = std::vector<Cell>;

    void start(int shell);
    void stop();
    void send(const std::string& s);
    void feed(const std::string& bytes);
    void put(char32_t c);
    void lineFeed();
    void reverseIndex();
    void scrollUp(int n);
    void scrollDown(int n);
    void clearRow(Row& r, int from, int to);
    void csi(char fin);
    void sgr();
    void switchAlt(bool on);
    void eraseDisplay(int mode);
    Row blankRow() const;

    int cols_ = 100, rows_ = 31, cols0_ = 100, rows0_ = 30;
    float cw_ = 12, lh_ = 24;
    std::vector<Row> grid_, saved_;
    std::deque<Row> scrollback_;
    int cx_ = 0, cy_ = 0, sx_ = 0, sy_ = 0, top_ = 0, bot_ = 30, scrollOff_ = 0;
    bool wrapPending_ = false, alt_ = false, cursorOn_ = true;
    Cell pen_;

    // parser state
    enum class P { Ground, Esc, Csi, Osc, OscEsc, Charset } ps_ = P::Ground;
    std::string params_;
    char32_t utfCp_ = 0;
    int utfLeft_ = 0;

    // process
    HPCON pc_ = nullptr;
    HANDLE in_ = nullptr, out_ = nullptr, proc_ = nullptr, thread_ = nullptr;
    std::thread reader_;
    std::mutex mu_;
    std::string pending_;
    std::atomic<bool> alive_{false};
    int shell_ = 0;
    bool hasWsl_ = false;
    double sinceKick_ = 0, blink_ = 0;
    bool dirtyOut_ = false;
    ui::Rect shellBtn_[3];
    int hoverBtn_ = -1;
    std::string exitNote_;
};

}  // namespace sf

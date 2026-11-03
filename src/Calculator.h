#pragma once
// Calculator window with three modes: Standard, Scientific (functions, DEG/RAD) and Computer (64-bit integers in
// HEX / DEC / OCT / BIN with bitwise operators and a clickable bit grid). Input is an expression that is evaluated with
// a small recursive-descent parser, so brackets and operator precedence behave as written.
#include <string>
#include <vector>

#include "PanelApp.h"

namespace sf {

class Calculator : public PanelApp {
public:
    explicit Calculator(Renderer& renderer);
    const char* name() const override { return "CALC"; }
    bool wantsText() const override { return true; }

    enum class Mode { Standard, Scientific, Computer };
    void setMode(Mode m);
    void setBase(int b);
    Mode mode() const { return mode_; }
    // Script / test hook: feed keys exactly as typed ("12*(3+4)=").
    void feed(const std::string& s) { onText(s); invalidate(); }
    std::string result() const { return cur_; }

protected:
    void paint(Bitmap& bm) override;
    bool onHover(float x, float y) override;
    void onClick(float x, float y, bool dbl) override;
    void onKey(EditKey k) override;
    void onText(const std::string& s) override;
    void onCtrl(const std::string& s) override;

private:
    struct Key {
        const char* label;
        const char* act;  // text inserted into the expression, or a command (see press())
        bool primary = false;
        int span = 1;  // width in key units
    };
    struct Hit {
        ui::Rect r;
        int id;
    };
    std::vector<std::vector<Key>> layout() const;  // [rows][cols] of the keypad for the current mode
    void press(const std::string& act);
    void insert(const std::string& s);
    void evaluate();
    void clear();
    void backspace();
    void negate();
    void toggleBit(int bit);
    bool evalDouble(const std::string& e, double& out) const;
    bool evalInt(const std::string& e, long long& out) const;
    std::string fmtDouble(double v) const;
    std::string fmtInt(long long v, int base) const;
    std::string shown(const std::string& e) const;
    bool keyEnabled(const Key& k) const;
    std::string closed(const std::string& e) const;  // appends missing closing brackets

    Mode mode_ = Mode::Standard;
    int base_ = 10;
    bool deg_ = true;
    std::string cur_;      // the expression being typed, or the result after "="
    std::string history_;  // the previous expression
    bool fresh_ = false;   // cur_ holds a result: a digit starts a new entry, an operator continues from it
    bool error_ = false;
    std::vector<Hit> hits_;
    std::vector<Key> keys_;  // the keypad as drawn; a hit id below 1000 indexes into it
    long long val_ = 0;      // the value shown in the Computer readouts
    int hover_ = -1;
};

}  // namespace sf

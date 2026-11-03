#include "Calculator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace sf {

namespace {

constexpr double kPi = 3.14159265358979323846;

// UTF-8 glyphs kept as escapes so the source does not depend on the compiler's input encoding.
#define G_DIV "\xC3\xB7"
#define G_MUL "\xC3\x97"
#define G_SQRT "\xE2\x88\x9A"
#define G_PI "\xCF\x80"
#define G_PM "\xC2\xB1"
#define G_SQ "\xC2\xB2"

int digitVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ----- floating point expression parser (Standard / Scientific) -----
struct DoubleParser {
    const std::string& s;
    bool deg;
    size_t i = 0;
    bool ok = true;

    char peek() {
        while (i < s.size() && s[i] == ' ') ++i;
        return i < s.size() ? s[i] : '\0';
    }
    bool startsPrimary() {
        const char c = peek();
        return c == '(' || c == '.' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
    }
    double expr() {
        double v = term();
        for (;;) {
            const char c = peek();
            if (c == '+') { ++i; v += term(); }
            else if (c == '-') { ++i; v -= term(); }
            else return v;
        }
    }
    double term() {
        double v = unary();
        for (;;) {
            const char c = peek();
            if (c == '*') { ++i; v *= unary(); }
            else if (c == '/') {
                ++i;
                const double d = unary();
                if (d == 0) ok = false;
                else v /= d;
            } else if (startsPrimary()) v *= unary();  // 2(3+4), 3pi
            else return v;
        }
    }
    double unary() {
        const char c = peek();
        if (c == '-') { ++i; return -unary(); }
        if (c == '+') { ++i; return unary(); }
        return power();
    }
    double power() {
        double b = postfix();
        if (peek() == '^') {
            ++i;
            const double e = unary();
            b = std::pow(b, e);
            if (!std::isfinite(b)) ok = false;
        }
        return b;
    }
    double postfix() {
        double v = primary();
        for (;;) {
            const char c = peek();
            if (c == '!') {
                ++i;
                if (v < 0 || v > 170 || v != std::floor(v)) { ok = false; return 0; }
                double f = 1;
                for (int k = 2; k <= static_cast<int>(v); ++k) f *= k;
                v = f;
            } else if (c == '%') { ++i; v /= 100.0; }
            else return v;
        }
    }
    double primary() {
        const char c = peek();
        if (c == '(') {
            ++i;
            const double v = expr();
            if (peek() == ')') ++i;
            else ok = false;
            return v;
        }
        if ((c >= '0' && c <= '9') || c == '.') {
            const size_t st = i;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) ++i;
            if (i < s.size() && s[i] == 'e') {  // exponent only when digits follow, otherwise e is the constant
                size_t j = i + 1;
                if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
                if (j < s.size() && s[j] >= '0' && s[j] <= '9') {
                    while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
                    i = j;
                }
            }
            return std::atof(s.substr(st, i - st).c_str());
        }
        if (c >= 'a' && c <= 'z') {
            const size_t st = i;
            while (i < s.size() && s[i] >= 'a' && s[i] <= 'z') ++i;
            const std::string n = s.substr(st, i - st);
            if (n == "pi") return kPi;
            if (n == "e") return std::exp(1.0);
            if (peek() != '(') { ok = false; return 0; }
            ++i;
            const double a = expr();
            if (peek() == ')') ++i;
            else ok = false;
            return func(n, a);
        }
        ok = false;
        return 0;
    }
    double func(const std::string& n, double a) {
        const double toRad = deg ? kPi / 180.0 : 1.0, fromRad = deg ? 180.0 / kPi : 1.0;
        double r = 0;
        if (n == "sin") r = std::sin(a * toRad);
        else if (n == "cos") r = std::cos(a * toRad);
        else if (n == "tan") {
            const double c = std::cos(a * toRad);
            if (std::fabs(c) < 1e-15) { ok = false; return 0; }
            r = std::tan(a * toRad);
        } else if (n == "asin") { if (a < -1 || a > 1) ok = false; else r = std::asin(a) * fromRad; }
        else if (n == "acos") { if (a < -1 || a > 1) ok = false; else r = std::acos(a) * fromRad; }
        else if (n == "atan") r = std::atan(a) * fromRad;
        else if (n == "log") { if (a <= 0) ok = false; else r = std::log10(a); }
        else if (n == "ln") { if (a <= 0) ok = false; else r = std::log(a); }
        else if (n == "sqrt") { if (a < 0) ok = false; else r = std::sqrt(a); }
        else ok = false;
        if (std::fabs(r) < 1e-14 && (n == "sin" || n == "cos" || n == "tan")) r = 0;  // sin(pi) is 0, not 1.2e-16
        return r;
    }
};

// ----- 64-bit integer parser (Computer) -----
struct IntParser {
    const std::string& s;
    int base;
    size_t i = 0;
    bool ok = true;
    using U = unsigned long long;

    char peek() {
        while (i < s.size() && s[i] == ' ') ++i;
        return i < s.size() ? s[i] : '\0';
    }
    bool two(char a, char b) {
        peek();
        return i + 1 < s.size() && s[i] == a && s[i + 1] == b;
    }
    U orExpr() {
        U v = xorExpr();
        while (peek() == '|') { ++i; v |= xorExpr(); }
        return v;
    }
    U xorExpr() {
        U v = andExpr();
        while (peek() == '^') { ++i; v ^= andExpr(); }
        return v;
    }
    U andExpr() {
        U v = shiftExpr();
        while (peek() == '&') { ++i; v &= shiftExpr(); }
        return v;
    }
    U shiftExpr() {
        U v = addExpr();
        for (;;) {
            if (two('<', '<')) { i += 2; v <<= (addExpr() & 63); }
            else if (two('>', '>')) { i += 2; v = static_cast<U>(static_cast<long long>(v) >> (addExpr() & 63)); }
            else return v;
        }
    }
    U addExpr() {
        U v = mulExpr();
        for (;;) {
            const char c = peek();
            if (c == '+') { ++i; v += mulExpr(); }
            else if (c == '-') { ++i; v -= mulExpr(); }
            else return v;
        }
    }
    U mulExpr() {
        U v = unary();
        for (;;) {
            const char c = peek();
            if (c == '*') { ++i; v *= unary(); }
            else if (c == '/' || c == '%') {
                ++i;
                const long long d = static_cast<long long>(unary());
                if (d == 0) { ok = false; return 0; }
                const long long a = static_cast<long long>(v);
                if (a == std::numeric_limits<long long>::min() && d == -1) v = (c == '/') ? v : 0;
                else v = static_cast<U>(c == '/' ? a / d : a % d);
            } else return v;
        }
    }
    U unary() {
        const char c = peek();
        if (c == '-') { ++i; return static_cast<U>(0) - unary(); }
        if (c == '+') { ++i; return unary(); }
        if (c == '~') { ++i; return ~unary(); }
        return primary();
    }
    U primary() {
        const char c = peek();
        if (c == '(') {
            ++i;
            const U v = orExpr();
            if (peek() == ')') ++i;
            else ok = false;
            return v;
        }
        if (digitVal(c) >= 0) {
            U v = 0;
            while (i < s.size() && digitVal(s[i]) >= 0) {
                const int d = digitVal(s[i]);
                if (d >= base) { ok = false; return 0; }
                v = v * static_cast<U>(base) + static_cast<U>(d);
                ++i;
            }
            return v;
        }
        ok = false;
        return 0;
    }
};

}  // namespace

Calculator::Calculator(Renderer& renderer) : PanelApp(renderer, 1000, 1300, 1.2f) {
    title_ = "CALCULATOR";
}

void Calculator::setMode(Mode m) {
    if (m == mode_) return;
    mode_ = m;
    clear();
    base_ = 10;
    invalidate();
}

std::string Calculator::closed(const std::string& e) const {
    int open = 0;
    for (char c : e) {
        if (c == '(') ++open;
        else if (c == ')') --open;
    }
    std::string r = e;
    while (open-- > 0) r += ')';
    return r;
}

bool Calculator::evalDouble(const std::string& e, double& out) const {
    DoubleParser p{e, deg_};
    out = p.expr();
    return p.ok && p.peek() == '\0' && std::isfinite(out);
}

bool Calculator::evalInt(const std::string& e, long long& out) const {
    IntParser p{e, base_};
    out = static_cast<long long>(p.orExpr());
    return p.ok && p.peek() == '\0';
}

std::string Calculator::fmtDouble(double v) const {
    if (std::fabs(v) < 1e-13) v = 0;
    char b[48];
    std::snprintf(b, sizeof b, "%.12g", v);
    return b;
}

std::string Calculator::fmtInt(long long v, int base) const {
    if (base == 10) return std::to_string(v);
    unsigned long long u = static_cast<unsigned long long>(v);
    if (u == 0) return "0";
    std::string r;
    while (u) {
        r += "0123456789ABCDEF"[u % static_cast<unsigned>(base)];
        u /= static_cast<unsigned>(base);
    }
    std::reverse(r.begin(), r.end());
    return r;
}

std::string Calculator::shown(const std::string& e) const {
    std::string r;
    const bool comp = mode_ == Mode::Computer;
    for (size_t i = 0; i < e.size(); ++i) {
        const char c = e[i];
        if (c == '*') r += G_MUL;
        else if (c == '/') r += G_DIV;
        else if (comp && c == '&') r += " AND ";
        else if (comp && c == '|') r += " OR ";
        else if (comp && c == '^') r += " XOR ";
        else if (comp && c == '%') r += " MOD ";
        else if (comp && c == '~') r += "NOT ";
        else if (comp && c == '<' ) { r += " << "; ++i; }
        else if (comp && c == '>') { r += " >> "; ++i; }
        else if (!comp && e.compare(i, 5, "sqrt(") == 0) { r += G_SQRT "("; i += 4; }
        else if (!comp && e.compare(i, 2, "pi") == 0) { r += G_PI; ++i; }
        else r += c;
    }
    return r;
}

void Calculator::clear() {
    cur_.clear();
    history_.clear();
    fresh_ = false;
    error_ = false;
}

void Calculator::backspace() {
    if (error_) { error_ = false; return; }
    fresh_ = false;
    if (cur_.empty()) return;
    if (mode_ != Mode::Computer) {
        const auto isAlpha = [](char c) { return c >= 'a' && c <= 'z'; };
        if (cur_.back() == '(' ) {
            cur_.pop_back();
            while (!cur_.empty() && isAlpha(cur_.back())) cur_.pop_back();
            return;
        }
        if (isAlpha(cur_.back())) {
            while (!cur_.empty() && isAlpha(cur_.back())) cur_.pop_back();
            return;
        }
    }
    cur_.pop_back();
}

void Calculator::negate() {
    error_ = false;
    fresh_ = false;
    if (cur_.empty()) { cur_ = "-"; return; }
    bool plain = true;
    for (size_t i = (cur_[0] == '-' ? 1 : 0); i < cur_.size(); ++i)
        if (!(digitVal(cur_[i]) >= 0 || cur_[i] == '.')) plain = false;
    if (cur_[0] == '-' && plain) cur_.erase(0, 1);
    else if (plain) cur_ = "-" + cur_;
    else if (cur_.size() > 3 && cur_.compare(0, 2, "-(") == 0 && cur_.back() == ')' && closed(cur_.substr(1)) == cur_.substr(1))
        cur_ = cur_.substr(2, cur_.size() - 3);
    else cur_ = "-(" + cur_ + ")";
}

void Calculator::insert(const std::string& s) {
    if (s.empty()) return;
    error_ = false;
    const bool op = std::string("+-*/^%&|<>!").find(s[0]) != std::string::npos;
    if (fresh_ && !op) cur_.clear();
    fresh_ = false;
    if (cur_.empty() && op && s != "-" && s != "~") cur_ = "0";
    if ((s == "+" || s == "*" || s == "/") && !cur_.empty() && std::string("+-*/").find(cur_.back()) != std::string::npos &&
        cur_.size() > 1)
        cur_.pop_back();
    cur_ += s;
}

void Calculator::evaluate() {
    if (cur_.empty()) return;
    const std::string e = closed(cur_);
    std::string out;
    bool good;
    if (mode_ == Mode::Computer) {
        long long v = 0;
        good = evalInt(e, v);
        if (good) { out = fmtInt(v, base_); val_ = v; }
    } else {
        double v = 0;
        good = evalDouble(e, v);
        if (good) out = fmtDouble(v);
    }
    history_ = shown(e);
    if (good) {
        history_ += " =";
        cur_ = out;
        fresh_ = true;
        error_ = false;
    } else {
        error_ = true;
    }
}

void Calculator::setBase(int b) {
    if (b == base_) return;
    if (!cur_.empty() && !error_) {
        long long v = 0;
        if (evalInt(closed(cur_), v)) {
            base_ = b;
            cur_ = fmtInt(v, base_);
            history_.clear();
            fresh_ = true;
            return;
        }
        cur_.clear();
    }
    base_ = b;
    history_.clear();
    fresh_ = false;
    error_ = false;
}

void Calculator::toggleBit(int bit) {
    unsigned long long v = static_cast<unsigned long long>(val_) ^ (1ULL << bit);
    val_ = static_cast<long long>(v);
    cur_ = fmtInt(val_, base_);
    history_.clear();
    fresh_ = true;
    error_ = false;
}

void Calculator::press(const std::string& act) {
    if (act == "CLR") clear();
    else if (act == "BS") backspace();
    else if (act == "=") evaluate();
    else if (act == "NEG") negate();
    else if (act == "DEG") deg_ = !deg_;
    else insert(act);
}

bool Calculator::keyEnabled(const Key& k) const {
    if (mode_ != Mode::Computer) return true;
    const std::string a = k.act;
    if (a == ".") return false;
    if (a.size() == 1 && digitVal(a[0]) >= 0) return digitVal(a[0]) < base_;
    return true;
}

std::vector<std::vector<Calculator::Key>> Calculator::layout() const {
    const Key blank{"", ""};
    if (mode_ == Mode::Standard) {
        return {
            {{"C", "CLR"}, {"DEL", "BS"}, {"%", "%"}, {G_DIV, "/"}},
            {{"7", "7"}, {"8", "8"}, {"9", "9"}, {G_MUL, "*"}},
            {{"4", "4"}, {"5", "5"}, {"6", "6"}, {"-", "-"}},
            {{"1", "1"}, {"2", "2"}, {"3", "3"}, {"+", "+"}},
            {{G_PM, "NEG"}, {"0", "0"}, {".", "."}, {"=", "=", true}},
        };
    }
    if (mode_ == Mode::Scientific) {
        return {
            {{"sin", "sin("}, {"cos", "cos("}, {"tan", "tan("}, {"C", "CLR"}, {"DEL", "BS"}, {"(", "("}, {")", ")"}},
            {{"asin", "asin("}, {"acos", "acos("}, {"atan", "atan("}, {"7", "7"}, {"8", "8"}, {"9", "9"}, {G_DIV, "/"}},
            {{"log", "log("}, {"ln", "ln("}, {G_SQRT, "sqrt("}, {"4", "4"}, {"5", "5"}, {"6", "6"}, {G_MUL, "*"}},
            {{"x" G_SQ, "^2"}, {"1/x", "^(-1)"}, {"n!", "!"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"-", "-"}},
            {{G_PI, "pi"}, {"e", "e"}, {"x^y", "^"}, {G_PM, "NEG"}, {"0", "0"}, {".", "."}, {"+", "+"}},
            {{"DEG", "DEG"}, {"%", "%"}, {"10^x", "10^("}, {"=", "=", true, 4}},
        };
    }
    return {
        {{"A", "A"}, {"B", "B"}, {"AND", "&"}, {"AC", "CLR"}, {"DEL", "BS"}, {"(", "("}, {")", ")"}},
        {{"C", "C"}, {"D", "D"}, {"OR", "|"}, {"7", "7"}, {"8", "8"}, {"9", "9"}, {G_DIV, "/"}},
        {{"E", "E"}, {"F", "F"}, {"XOR", "^"}, {"4", "4"}, {"5", "5"}, {"6", "6"}, {G_MUL, "*"}},
        {{"NOT", "~"}, {"<<", "<<"}, {">>", ">>"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"-", "-"}},
        {{"MOD", "%"}, blank, blank, {G_PM, "NEG"}, {"0", "0"}, {".", "."}, {"+", "+"}},
        {blank, blank, blank, {"=", "=", true, 4}},
    };
}

bool Calculator::onHover(float x, float y) {
    int h = -1;
    for (const Hit& t : hits_)
        if (t.r.contains(x, y)) { h = t.id; break; }
    if (h == hover_) return false;
    hover_ = h;
    return true;
}

void Calculator::onClick(float x, float y, bool) {
    for (const Hit& t : hits_) {
        if (!t.r.contains(x, y)) continue;
        if (t.id < 1000) {
            if (keyEnabled(keys_[static_cast<size_t>(t.id)])) press(keys_[static_cast<size_t>(t.id)].act);
        } else if (t.id < 1100) setMode(static_cast<Mode>(t.id - 1000));
        else if (t.id < 1200) setBase(t.id == 1100 ? 16 : t.id == 1101 ? 10 : t.id == 1102 ? 8 : 2);
        else if (t.id >= 2000) toggleBit(t.id - 2000);
        return;
    }
}

void Calculator::onKey(EditKey k) {
    if (k == EditKey::Backspace) backspace();
    else if (k == EditKey::Delete) clear();
    else if (k == EditKey::Enter) evaluate();
}

void Calculator::onCtrl(const std::string& s) {
    if (s == "esc") close();
}

void Calculator::onText(const std::string& s) {
    const bool comp = mode_ == Mode::Computer;
    for (char c : s) {
        if (c == '=') { evaluate(); continue; }
        if (c == ' ' || c == '\r' || c == '\n') continue;
        const int dv = digitVal(c);
        if (comp) {
            if (dv >= 0) {
                if (dv < base_) insert(std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(c)))));
                continue;
            }
            if (c == '<' || c == '>') {  // typed once or twice, the operator is always "<<" / ">>"
                if (cur_.empty() || cur_.back() != c) insert(std::string(2, c));
            }
            else if (std::string("+-*/%&|^~()").find(c) != std::string::npos) insert(std::string(1, c));
            continue;
        }
        if (dv >= 0 && dv <= 9) insert(std::string(1, c));
        else if (c == '.' || std::string("+-*/^%!()").find(c) != std::string::npos) insert(std::string(1, c));
        else if (c == 'p' || c == 'P') insert("pi");
        else if (mode_ == Mode::Scientific && c == 's') insert("sin(");
        else if (mode_ == Mode::Scientific && c == 'c') insert("cos(");
        else if (mode_ == Mode::Scientific && c == 't') insert("tan(");
        else if (mode_ == Mode::Scientific && c == 'l') insert("log(");
        else if (mode_ == Mode::Scientific && c == 'n') insert("ln(");
        else if (mode_ == Mode::Scientific && c == 'r') insert("sqrt(");
        else if (c == 'e' || c == 'E') insert("e");
    }
}

namespace {
// Shrinks the text until it fits; falls back to showing its tail.
std::string fitText(const std::string& t, float& px, float maxW, float minPx, FontFace f) {
    while (px > minPx && measureText(t, px, f) > maxW) px -= 4;
    if (measureText(t, px, f) <= maxW) return t;
    std::string r = t;
    while (r.size() > 4 && measureText("..." + r, px, f) > maxW) r.erase(0, 1);
    return "..." + r;
}
}  // namespace

void Calculator::paint(Bitmap& bm) {
    using namespace ui;
    const Tokens t = tokens();
    const char* modeName = mode_ == Mode::Standard ? "STANDARD" : mode_ == Mode::Scientific ? "SCIENTIFIC" : "COMPUTER";
    const Rect c = panel(bm, "CALCULATOR", modeName);
    hits_.clear();
    keys_.clear();
    const bool comp = mode_ == Mode::Computer;
    const float gap = 10;
    float y = c.y0 + 8;

    // Mode tabs.
    const char* names[3] = {"Standard", "Scientific", "Computer"};
    const float tabW = (c.w() - gap * 2) / 3;
    for (int m = 0; m < 3; ++m) {
        const Rect r{c.x0 + m * (tabW + gap), y, c.x0 + m * (tabW + gap) + tabW, y + 62};
        const State s = static_cast<int>(mode_) == m ? State::Selected : (hover_ == 1000 + m ? State::Hover : State::Normal);
        button(bm, r, names[m], s, static_cast<int>(mode_) == m, 28);
        hits_.push_back({r, 1000 + m});
    }
    y += 62 + 16;

    // Display.
    const float dispH = comp ? 170.0f : 250.0f;
    const Rect d{c.x0, y, c.x1, y + dispH};
    card(bm, d, State::Normal);
    const float inner = d.w() - 48;
    float hp = 32;
    const std::string hist = fitText(error_ ? shown(closed(cur_)) : history_, hp, inner, 22, FontFace::UI);
    labelRight(bm, d.x1 - 24, d.y0 + 20 + hp, hist, hp, FontFace::UI, t.dim);
    std::string big = error_ ? "Error" : (cur_.empty() ? "0" : shown(cur_));
    float bp = comp ? 70.0f : 96.0f;
    big = fitText(big, bp, inner, 34, FontFace::UIBold);
    labelRight(bm, d.x1 - 24, d.y1 - 26, big, bp, FontFace::UIBold, error_ ? t.danger : t.text);
    if (mode_ == Mode::Scientific) label(bm, d.x0 + 24, d.y0 + 52, deg_ ? "DEG" : "RAD", 24, FontFace::UIBold, t.accent);
    y += dispH + 14;

    if (comp) {
        // Value of what is typed; keeps the last good value while the expression is incomplete.
        if (!cur_.empty() && !error_) {
            long long v = 0;
            if (evalInt(closed(cur_), v)) val_ = v;
        } else if (cur_.empty()) val_ = 0;
        const unsigned long long u = static_cast<unsigned long long>(val_);
        const char* bn[4] = {"HEX", "DEC", "OCT", "BIN"};
        const int bb[4] = {16, 10, 8, 2};
        for (int i = 0; i < 4; ++i) {
            const Rect r{c.x0, y, c.x1, y + 42};
            const bool on = base_ == bb[i];
            const State s = on ? State::Selected : (hover_ == 1100 + i ? State::Hover : State::Normal);
            card(bm, r, s);
            label(bm, r.x0 + 18, r.y0 + 30, bn[i], 24, FontFace::UIBold, on ? t.accent : t.dim);
            std::string v = fmtInt(val_, bb[i]);
            if (bb[i] == 2 || bb[i] == 16) {  // group digits in fours
                std::string g;
                for (size_t k = 0; k < v.size(); ++k) {
                    if (k && (v.size() - k) % 4 == 0) g += ' ';
                    g += v[k];
                }
                v = g;
            }
            float px = 28;
            v = fitText(v, px, r.w() - 120, 16, FontFace::Mono);
            labelRight(bm, r.x1 - 18, r.y0 + 30, v, px, FontFace::Mono, on ? t.text : t.dim);
            hits_.push_back({r, 1100 + i});
            y += 42 + 6;
        }
        y += 8;
        // 64 clickable bits, 16 per row, most significant first.
        const float cw = (c.w() - 10) / 16.0f, ch = 36;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 16; ++col) {
                const int bit = 63 - row * 16 - col;
                const float x0 = c.x0 + col * cw + (col / 4) * 3.3f, y0 = y + row * (ch + 4);
                const Rect r{x0 + 1, y0, x0 + cw - 1, y0 + ch};
                const bool one = (u >> bit) & 1ULL;
                fillRect(bm, r.x0, r.y0, r.x1, r.y1, one ? mix(t.panel, t.accent, hover_ == 2000 + bit ? 0.5f : 0.35f)
                                                         : (hover_ == 2000 + bit ? t.panelHi : t.panel));
                strokeRect(bm, r.x0, r.y0, r.x1, r.y1, 1.5f, one ? t.accent : t.border);
                const std::string ds = one ? "1" : "0";
                const float tw = measureText(ds, 22, FontFace::Mono);
                drawText(bm, r.x0 + (r.w() - tw) * 0.5f, r.y0 + 26, ds, 22, FontFace::Mono, one ? t.text : t.muted);
                hits_.push_back({r, 2000 + bit});
            }
        }
        y += 4 * (ch + 4) + 10;
    }

    // Keypad.
    const auto rows = layout();
    const float nr = static_cast<float>(rows.size());
    const float kh = (c.y1 - y - (nr - 1) * gap) / nr;
    int unitsPerRow = 0;
    for (const Key& k : rows[0]) unitsPerRow += k.span;
    const float kw = (c.w() - (unitsPerRow - 1) * gap) / unitsPerRow;
    const float px = std::min(kh * 0.36f, 40.0f);
    for (size_t r = 0; r < rows.size(); ++r) {
        float x = c.x0;
        const float ky = y + static_cast<float>(r) * (kh + gap);
        for (const Key& k : rows[r]) {
            const float w = kw * k.span + gap * (k.span - 1);
            if (k.label[0]) {
                const Rect rc{x, ky, x + w, ky + kh};
                const int id = static_cast<int>(keys_.size());
                keys_.push_back(k);
                const bool en = keyEnabled(k);
                std::string lab = k.label;
                if (std::string(k.act) == "DEG") lab = deg_ ? "DEG" : "RAD";
                const State s = !en ? State::Disabled : (hover_ == id ? State::Hover : State::Normal);
                button(bm, rc, lab, s, k.primary, px);
                hits_.push_back({rc, id});
            }
            x += w + gap;
        }
    }
}

}  // namespace sf
